#!/usr/bin/env bash
# In-container orchestrator for tilebake. Runs the full pbf -> device-tiles
# chain, starting tileserver-gl internally on loopback. Driven entirely by
# env vars (set by tilebake):
#
#   IN_PBF   path to the source .osm.pbf            (required, mounted ro)
#   OUT      output dir for the z/x/y tree          (required, mounted rw)
#   BBOX     "MINLON MINLAT MAXLON MAXLAT"          (default: read from the pbf)
#   ZMIN ZMAX  zoom range                           (required)
#   FMT      jpg | bin                              (default jpg)
#   QUALITY  JPEG quality                           (default 80)
#   STYLE    force a tileserver style id            (default: auto-discover)
#   JAVA_MEM e.g. 4g -> planetiler -Xmx4g           (default: planetiler decides)
#
# planetiler caches its global source data (water polygons, natural earth)
# under /cache so re-runs are offline; that dir is meant to be a mounted,
# persistent volume.
set -euo pipefail

: "${OUT:?set OUT}"; : "${ZMIN:?set ZMIN}"; : "${ZMAX:?set ZMAX}"
FMT="${FMT:-jpg}"; QUALITY="${QUALITY:-80}"
PORT=8080
CACHE=/cache
RUN=/tmp/run            # ephemeral: style bundle + the vector .mbtiles
# The vector .mbtiles is a throwaway intermediate — once maketiles has baked the
# device tree it is dead weight. Keep it on ephemeral /tmp (discarded with the
# --rm container) rather than the mounted /cache, so the only thing that
# persists between runs is planetiler's ~1GB source download under /cache.
MBTILES="$RUN/tiles.mbtiles"

VERBOSE="${VERBOSE:-0}"
log()  { printf '\n\033[1;36m== %s\033[0m\n' "$*" >&2; }   # high-level phase, always
vlog() { [ "$VERBOSE" = 1 ] && printf '   %s\n' "$*" >&2 || true; }   # detail, -v only

# World mode: no source pbf — render the whole globe from the cached Natural
# Earth / water-polygon data (coherent to ~z7). Feed the bundled empty pbf so
# planetiler has the OSM input it requires, and force global bounds.
if [ "${WORLD:-0}" = 1 ]; then
    IN_PBF=/opt/empty.osm.pbf
    BBOX="${BBOX:--180 -85 180 85}"
fi
: "${IN_PBF:?set IN_PBF, or WORLD=1 for a global low-zoom bake}"

# bbox: use the one passed, otherwise read the pbf's own header extent. A direct
# header parse gives exactly four numbers (see pbfbbox.py — it replaced an osmium
# text scrape that mangled the box into a ~9x-too-large area). osmium's full scan
# is only the fallback for the rare extract that carries no header bbox.
if [ -z "${BBOX:-}" ]; then
    BBOX=$(python3 /opt/pbfbbox.py "$IN_PBF" || true)
    if [ -z "$BBOX" ]; then
        raw=$(osmium fileinfo -e -g data.bbox "$IN_PBF")
        BBOX=$(printf '%s' "$raw" | tr -cd '0-9.,-' | tr ',' ' ')
    fi
    vlog "bbox from pbf header: $BBOX"
fi
# Take exactly the first four fields, so a stray token can never shift the rest.
set -- $BBOX
MINLON=${1:-}; MINLAT=${2:-}; MAXLON=${3:-}; MAXLAT=${4:-}
[ -n "$MAXLAT" ] || { echo "could not determine a bbox; pass --bbox explicitly" >&2; exit 1; }

mkdir -p "$CACHE" "$OUT" "$RUN"

# ---------------------------------------------------------------------------
# 1. pbf -> OpenMapTiles-schema vector .mbtiles, rendered natively at the
#    requested zoom range: --maxzoom is the requested ceiling, so the whole
#    pipeline renders at the selected zoom (no overzoom). We deliberately set NO
#    --minzoom: a vector source with a *non-zero* minzoom renders BLANK at exactly
#    that bottom level in tileserver-gl/MapLibre (no parent tile to compose from),
#    which would clobber the layer beneath; starting at z0 keeps every baked zoom
#    above the source floor. NB planetiler/OpenMapTiles cap vector zoom (≈z15) and
#    OSM carries no real detail above ~z14, so a higher ceiling may error here and
#    in any case reveals no new detail — just larger tiles.
# ---------------------------------------------------------------------------
log "building vector tiles  z$ZMIN-$ZMAX"
vlog "planetiler: $IN_PBF -> $MBTILES  bounds $MINLON,$MINLAT,$MAXLON,$MAXLAT"
cd "$CACHE"   # planetiler caches downloaded sources under ./data here (persist)
# --tmpdir on ephemeral /tmp: the transient node/way location cache (can be
# large for big extracts, cleaned on success) stays off the mounted volume.
# Quiet unless -v: planetiler prints a progress block every ~10s. Drop it but
# keep ' ERR ' lines and planetiler's real exit (via PIPESTATUS, not the grep's).
if [ "$VERBOSE" = 1 ]; then
    java ${JAVA_MEM:+-Xmx"$JAVA_MEM"} -jar /opt/planetiler.jar \
        --osm-path="$IN_PBF" --output="$MBTILES" \
        --bounds="$MINLON,$MINLAT,$MAXLON,$MAXLAT" \
        --maxzoom="$ZMAX" \
        --tmpdir=/tmp/planetiler-tmp --download --force
else
    set +e
    java ${JAVA_MEM:+-Xmx"$JAVA_MEM"} -jar /opt/planetiler.jar \
        --osm-path="$IN_PBF" --output="$MBTILES" \
        --bounds="$MINLON,$MINLAT,$MAXLON,$MAXLAT" \
        --maxzoom="$ZMAX" \
        --tmpdir=/tmp/planetiler-tmp --download --force 2>&1 | grep -F ' ERR ' >&2
    rc=${PIPESTATUS[0]}; set -e
    [ "$rc" -eq 0 ] || { echo "planetiler failed — re-run tilebake with -v for the full log" >&2; exit 1; }
fi

# ---------------------------------------------------------------------------
# 2. Assemble a tileserver config around our .mbtiles, reusing the bundled
#    style + fonts. Discover the vector style and the *data id* it references
#    (the {token} in its source url, e.g. mbtiles://{v3}) so we don't hard-code
#    names that drift between tileserver-gl-styles versions.
# ---------------------------------------------------------------------------
vlog "wiring tileserver-gl config"
# tiles.mbtiles is already in $RUN (planetiler wrote it there); just lay the
# style bundle alongside it. cp -a won't clobber it — tsdata ships no .mbtiles.
cp -a /opt/tsdata/. "$RUN/"

STYLE_REL=$(cd "$RUN/styles" && find . -maxdepth 3 -name style.json | sed 's#^\./##' | head -n1)
[ -n "$STYLE_REL" ] || { echo "no bundled style.json found under $RUN/styles" >&2; exit 1; }
STYLE_ID="${STYLE:-$(dirname "$STYLE_REL")}"

# The mbtiles source url looks like "mbtiles://{v3}"; the data entry must be
# named after that {token}, which is NOT necessarily the source key.
SRC_URL=$(jq -r '[.sources[] | select(.type=="vector")][0].url // empty' \
              "$RUN/styles/$STYLE_REL")
DATA_ID=$(printf '%s' "$SRC_URL" | sed -n 's/.*{\(.*\)}.*/\1/p')
[ -n "$DATA_ID" ] || DATA_ID=openmaptiles
vlog "style id: $STYLE_ID   data id: $DATA_ID   ($STYLE_REL, src url '$SRC_URL')"

cat > "$RUN/config.json" <<EOF
{
  "options": {
    "paths": { "root": "$RUN", "fonts": "fonts", "styles": "styles", "mbtiles": "" }
  },
  "styles": { "$STYLE_ID": { "style": "$STYLE_REL" } },
  "data":   { "$DATA_ID": { "mbtiles": "tiles.mbtiles" } }
}
EOF

# ---------------------------------------------------------------------------
# 3. Serve raster tiles internally on loopback. GL render needs an X display;
#    mirror the image's own entrypoint (Xvfb on :99) rather than depend on the
#    xvfb-run wrapper being present.
# ---------------------------------------------------------------------------
vlog "starting tileserver-gl on 127.0.0.1:$PORT"
cd "$RUN"
rm -f /tmp/.X99-lock
export DISPLAY=:99
Xvfb "$DISPLAY" -nolisten unix >/tmp/xvfb.log 2>&1 &
node /usr/src/app --config "$RUN/config.json" -p "$PORT" -b 127.0.0.1 \
    >/tmp/tileserver.log 2>&1 &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true' EXIT

TILE_URL="http://127.0.0.1:$PORT/styles/$STYLE_ID/{z}/{x}/{y}.png"
PROBE="http://127.0.0.1:$PORT/styles/$STYLE_ID/0/0/0.png"
for i in $(seq 1 120); do
    if curl -sf -o /dev/null "$PROBE"; then break; fi
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "tileserver-gl exited early; log:" >&2; cat /tmp/tileserver.log >&2; exit 1
    fi
    [ "$i" = 120 ] && { echo "tileserver-gl not ready after 120s; log:" >&2; \
        cat /tmp/tileserver.log >&2; exit 1; }
    sleep 1
done
vlog "serving $TILE_URL"

# ---------------------------------------------------------------------------
# 4. Bake the device tile tree from the internal server.
# ---------------------------------------------------------------------------
log "rasterizing + tiling  z$ZMIN-$ZMAX  ->  $OUT"
FMT_FLAG=--jpg; [ "$FMT" = bin ] && FMT_FLAG=--bin
# Quiet unless -v: maketiles prints a per-zoom progress line to stdout — drop it,
# but keep stderr so genuine failures (decode/fetch/tileserver) still surface.
if [ "$VERBOSE" = 1 ]; then
    python3 /opt/maketiles.py \
        --src "$TILE_URL" --out "$OUT" \
        --bbox "$MINLON" "$MINLAT" "$MAXLON" "$MAXLAT" \
        --zoom "$ZMIN" "$ZMAX" \
        $FMT_FLAG --quality "$QUALITY" --delay 0
else
    python3 /opt/maketiles.py \
        --src "$TILE_URL" --out "$OUT" \
        --bbox "$MINLON" "$MINLAT" "$MAXLON" "$MAXLAT" \
        --zoom "$ZMIN" "$ZMAX" \
        $FMT_FLAG --quality "$QUALITY" --delay 0 >/dev/null \
        || { echo "tiling failed — re-run tilebake with -v for the full log" >&2; exit 1; }
fi

# Drop the vector intermediate now that the device tree is baked. The --rm
# container reclaims /tmp on exit regardless; this just frees it as soon as it's
# dead weight rather than holding it until the run ends.
rm -rf "$MBTILES" /tmp/planetiler-tmp

log "done -> $OUT  (copy its contents to the SD card under /sdcard/maps)"
