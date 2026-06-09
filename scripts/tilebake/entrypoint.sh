#!/usr/bin/env bash
# In-container orchestrator for tilebake. Runs the full pbf -> device-tiles
# chain, starting tileserver-gl internally on loopback. Driven entirely by
# env vars (set by tilebake.sh):
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
RUN=/tmp/run            # writable copy of the style bundle + our .mbtiles
MBTILES="$CACHE/tiles.mbtiles"

log() { printf '\n\033[1;36m== %s\033[0m\n' "$*" >&2; }

# World mode: no source pbf — render the whole globe from the cached Natural
# Earth / water-polygon data (coherent to ~z7). Feed the bundled empty pbf so
# planetiler has the OSM input it requires, and force global bounds.
if [ "${WORLD:-0}" = 1 ]; then
    IN_PBF=/opt/empty.osm.pbf
    BBOX="${BBOX:--180 -85 180 85}"
fi
: "${IN_PBF:?set IN_PBF, or WORLD=1 for a global low-zoom bake}"

# bbox: use the one passed, otherwise read the pbf's own extent — the header
# box (fast, present in Geofabrik extracts) with an extended full-scan fallback.
if [ -z "${BBOX:-}" ]; then
    raw=$(osmium fileinfo -g 'header.boxes[0]' "$IN_PBF" 2>/dev/null || true)
    [ -n "$raw" ] || raw=$(osmium fileinfo -e -g data.bbox "$IN_PBF")
    BBOX=$(printf '%s' "$raw" | tr -cd '0-9.,-' | tr ',' ' ')
    echo "  no --bbox given; derived from pbf: $BBOX" >&2
fi
read -r MINLON MINLAT MAXLON MAXLAT <<<"$BBOX"
[ -n "${MAXLAT:-}" ] || { echo "could not determine a bbox; pass --bbox explicitly" >&2; exit 1; }

mkdir -p "$CACHE" "$OUT" "$RUN"

# ---------------------------------------------------------------------------
# 1. pbf -> OpenMapTiles-schema vector .mbtiles
# ---------------------------------------------------------------------------
log "planetiler: $IN_PBF -> $MBTILES  (bounds $MINLON,$MINLAT,$MAXLON,$MAXLAT)"
cd "$CACHE"   # planetiler caches downloaded sources under ./data here
java ${JAVA_MEM:+-Xmx"$JAVA_MEM"} -jar /opt/planetiler.jar \
    --osm-path="$IN_PBF" \
    --output="$MBTILES" \
    --bounds="$MINLON,$MINLAT,$MAXLON,$MAXLAT" \
    --download --force

# ---------------------------------------------------------------------------
# 2. Assemble a tileserver config around our .mbtiles, reusing the bundled
#    style + fonts. Discover the vector style and the *data id* it references
#    (the {token} in its source url, e.g. mbtiles://{v3}) so we don't hard-code
#    names that drift between tileserver-gl-styles versions.
# ---------------------------------------------------------------------------
log "wiring tileserver-gl config"
cp -a /opt/tsdata/. "$RUN/"
cp "$MBTILES" "$RUN/tiles.mbtiles"

STYLE_REL=$(cd "$RUN/styles" && find . -maxdepth 3 -name style.json | sed 's#^\./##' | head -n1)
[ -n "$STYLE_REL" ] || { echo "no bundled style.json found under $RUN/styles" >&2; exit 1; }
STYLE_ID="${STYLE:-$(dirname "$STYLE_REL")}"

# The mbtiles source url looks like "mbtiles://{v3}"; the data entry must be
# named after that {token}, which is NOT necessarily the source key.
SRC_URL=$(jq -r '[.sources[] | select(.type=="vector")][0].url // empty' \
              "$RUN/styles/$STYLE_REL")
DATA_ID=$(printf '%s' "$SRC_URL" | sed -n 's/.*{\(.*\)}.*/\1/p')
[ -n "$DATA_ID" ] || DATA_ID=openmaptiles
echo "  style id: $STYLE_ID   data id: $DATA_ID   ($STYLE_REL, src url '$SRC_URL')" >&2

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
log "starting tileserver-gl on 127.0.0.1:$PORT"
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
echo "  serving $TILE_URL" >&2

# ---------------------------------------------------------------------------
# 4. Bake the device tile tree from the internal server.
# ---------------------------------------------------------------------------
log "maketiles: baking $FMT tiles z$ZMIN..$ZMAX -> $OUT"
FMT_FLAG=--jpg; [ "$FMT" = bin ] && FMT_FLAG=--bin
python3 /opt/maketiles.py \
    --src "$TILE_URL" --out "$OUT" \
    --bbox "$MINLON" "$MINLAT" "$MAXLON" "$MAXLAT" \
    --zoom "$ZMIN" "$ZMAX" \
    $FMT_FLAG --quality "$QUALITY" --delay 0

log "done -> $OUT  (copy its contents to the SD card under /sdcard/maps)"
