#!/usr/bin/env bash
# tilebake.sh — turn OSM .pbf extracts into the device map-tile tree the 'maps'
# app reads off SD. The whole chain (planetiler -> tileserver-gl -> maketiles.py)
# runs inside one container; the tile server lives entirely internal to it.
#
# Two ways to run:
#
#   1. Batch (zero-config) — the default. Make zoom-named subdirs of ./pbf and
#      run with no arguments:
#
#          pbf/0-7/                     (empty  -> whole world from Natural Earth)
#          pbf/7-9/germany.osm.pbf
#          pbf/10-16/berlin.osm.pbf
#          ./tilebake.sh
#
#      Each .pbf is baked at its folder's <zmin>-<zmax> range, bbox read from
#      the file. An EMPTY zoom folder bakes the whole world at that range (no
#      pbf needed — low zooms come from the cached Natural Earth/water data).
#      Folders run lowest-zoom-first; everything merges into ./out.
#
#   2. Single file / world — explicit one-offs:
#
#          ./tilebake.sh --pbf berlin.osm.pbf --zoom 10 16 [--bbox W S E N]
#          ./tilebake.sh --world --zoom 0 7
#
# Then copy ./out/* onto the SD card so the device sees /sdcard/maps/<z>/<x>/<y>.jpg
#
# Options:
#   --pbf FILE              single-file mode: this .pbf (needs --zoom)
#   --world                 single global low-zoom bake (needs --zoom; no pbf)
#   --pbf-dir DIR           batch mode root             (default ./pbf)
#   --out DIR               output tile tree            (default ./out)
#   --zoom ZMIN ZMAX        zoom range (single-file / --world mode)
#   --bbox W S E N          MINLON MINLAT MAXLON MAXLAT (default: read from pbf)
#   --bin                   raw RGB565 tiles instead of JPEG
#   --quality N             JPEG quality                (default 80)
#   --style ID              force a tileserver style id (default: auto)
#   --java-mem SIZE         planetiler heap, e.g. 6g    (default: auto)
#   --cache DIR             planetiler source cache     (default ./.tilebake-cache)
#   --image NAME            image tag                   (default maps-tilebake)
#   --rebuild               force a docker build even if the image exists
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)   # the maps/scripts dir

PBF="" PBF_DIR="./pbf" OUT="./out" ZOOM=() BBOX=() FMT=jpg QUALITY=80
STYLE="" JAVA_MEM="" CACHE="./.tilebake-cache" IMAGE="maps-tilebake" REBUILD=0
WORLD_MODE=0

die() { echo "tilebake: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --pbf)      PBF="$2"; shift 2;;
        --world)    WORLD_MODE=1; shift;;
        --pbf-dir)  PBF_DIR="$2"; shift 2;;
        --out)      OUT="$2"; shift 2;;
        --zoom)     ZOOM=("$2" "$3"); shift 3;;
        --bbox)     BBOX=("$2" "$3" "$4" "$5"); shift 5;;
        --bin)      FMT=bin; shift;;
        --quality)  QUALITY="$2"; shift 2;;
        --style)    STYLE="$2"; shift 2;;
        --java-mem) JAVA_MEM="$2"; shift 2;;
        --cache)    CACHE="$2"; shift 2;;
        --image)    IMAGE="$2"; shift 2;;
        --rebuild)  REBUILD=1; shift;;
        -h|--help)  sed -n '2,38p' "$0"; exit 0;;
        *)          die "unknown arg: $1 (try --help)";;
    esac
done

command -v docker >/dev/null || die "docker not found on PATH"

mkdir -p "$OUT" "$CACHE"
OUT_ABS=$(cd "$OUT" && pwd)
CACHE_ABS=$(cd "$CACHE" && pwd)

# Build the image once up front (shared by every job).
if [ "$REBUILD" = 1 ] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "tilebake: building image $IMAGE ..." >&2
    docker build -f "$HERE/tilebake/Dockerfile" -t "$IMAGE" "$HERE"
fi

# run_job ZMIN ZMAX PBF_ABS [BBOX_STR]
# One container run: pbf -> vector -> internal raster server -> device tiles.
# An empty PBF_ABS means "world": no source file, global bounds, content from
# the cached Natural Earth / water data (good to ~z7).
# Runs as the invoking user (uid:gid) so $OUT and the cache stay host-owned and
# never need sudo to clean up; HOME=/tmp is set in the image because an
# arbitrary uid has no passwd entry and xvfb/java want a writable $HOME.
run_job() {
    local zmin="$1" zmax="$2" pbf="$3" bbox="${4:-}"
    if [ -z "$pbf" ]; then
        printf '\n\033[1;32m>>> tilebake: world (Natural Earth)  z%s-%s\033[0m\n' \
            "$zmin" "$zmax" >&2
        docker run --rm -i \
            --user "$(id -u):$(id -g)" \
            -v "$OUT_ABS":/out \
            -v "$CACHE_ABS":/cache \
            -e OUT=/out -e WORLD=1 \
            -e ZMIN="$zmin" -e ZMAX="$zmax" \
            -e FMT="$FMT" -e QUALITY="$QUALITY" \
            ${STYLE:+-e STYLE="$STYLE"} \
            ${JAVA_MEM:+-e JAVA_MEM="$JAVA_MEM"} \
            "$IMAGE"
        return
    fi
    printf '\n\033[1;32m>>> tilebake: %s  z%s-%s%s\033[0m\n' \
        "$(basename "$pbf")" "$zmin" "$zmax" "${bbox:+  bbox=$bbox}" >&2
    docker run --rm -i \
        --user "$(id -u):$(id -g)" \
        -v "$pbf":/in/source.osm.pbf:ro \
        -v "$OUT_ABS":/out \
        -v "$CACHE_ABS":/cache \
        -e IN_PBF=/in/source.osm.pbf \
        -e OUT=/out \
        -e BBOX="$bbox" \
        -e ZMIN="$zmin" -e ZMAX="$zmax" \
        -e FMT="$FMT" -e QUALITY="$QUALITY" \
        ${STYLE:+-e STYLE="$STYLE"} \
        ${JAVA_MEM:+-e JAVA_MEM="$JAVA_MEM"} \
        "$IMAGE"
}

abspath() { (cd "$(dirname "$1")" && printf '%s/%s\n' "$(pwd)" "$(basename "$1")"); }

# --- Single world bake ------------------------------------------------------
if [ "$WORLD_MODE" = 1 ]; then
    [ ${#ZOOM[@]} -eq 2 ] || die "--world needs --zoom ZMIN ZMAX"
    run_job "${ZOOM[0]}" "${ZOOM[1]}" ""
    exit 0
fi

# --- Single-file mode -------------------------------------------------------
if [ -n "$PBF" ]; then
    [ -f "$PBF" ] || die "pbf not found: $PBF"
    [ ${#ZOOM[@]} -eq 2 ] || die "--pbf needs --zoom ZMIN ZMAX"
    [ ${#BBOX[@]} -eq 0 ] || [ ${#BBOX[@]} -eq 4 ] || \
        die "--bbox needs 4 numbers: MINLON MINLAT MAXLON MAXLAT"
    run_job "${ZOOM[0]}" "${ZOOM[1]}" "$(abspath "$PBF")" "${BBOX[*]:-}"
    exit 0
fi

# --- Batch mode -------------------------------------------------------------
# Discover ./pbf/<zmin>-<zmax>/ folders; bake each contained pbf at that range,
# lowest zmin first so coarse coverage lands before detail.
[ -d "$PBF_DIR" ] || die "no --pbf given and batch dir not found: $PBF_DIR (see --help)"

ranges=()   # "zmin zmax dirname"  (dirname is NN-MM, no spaces — safe to sort)
for d in "$PBF_DIR"/*/; do
    [ -d "$d" ] || continue
    name=$(basename "$d")
    case "$name" in
        [0-9]*-[0-9]*) ;;
        *) echo "tilebake: skipping non-zoom dir '$name'" >&2; continue;;
    esac
    zmin=${name%%-*}; zmax=${name##*-}
    case "$zmin$zmax" in *[!0-9]*) echo "tilebake: skipping '$name' (non-numeric)" >&2; continue;; esac
    [ "$zmin" -le "$zmax" ] || { t=$zmin; zmin=$zmax; zmax=$t; }
    ranges+=("$zmin $zmax $name")
done
[ ${#ranges[@]} -gt 0 ] || die "no <zmin>-<zmax> folders with pbfs under $PBF_DIR/ (e.g. $PBF_DIR/0-6/world.osm.pbf)"

# Sort folders by zmin then zmax, ascending (entries have no spaces in fields
# we split on, since the only multi-word part is the NN-MM dir name).
IFS=$'\n' sorted=($(printf '%s\n' "${ranges[@]}" | sort -n -k1,1 -k2,2)); unset IFS

jobs=0
for r in "${sorted[@]}"; do
    set -- $r; zmin="$1"; zmax="$2"; name="$3"
    found=0
    # one glob: *.pbf matches both foo.pbf and foo.osm.pbf, no double-runs.
    for f in "$PBF_DIR/$name"/*.pbf; do
        [ -f "$f" ] || continue
        found=1; jobs=$((jobs + 1))
        run_job "$zmin" "$zmax" "$(abspath "$f")"
    done
    # An empty zoom folder means "bake the whole world at this range".
    if [ "$found" = 0 ]; then
        jobs=$((jobs + 1))
        run_job "$zmin" "$zmax" ""
    fi
done
[ "$jobs" -gt 0 ] || die "found zoom folders but no .pbf files inside them"
echo "tilebake: done — $jobs file(s) baked into $OUT_ABS" >&2
