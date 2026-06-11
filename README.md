# maps

## What is this?

**maps** is an offline slippy-map viewer for spangap LCD devices: an LCD
launcher program that blits pre-baked map tiles (JPEG or raw RGB565) read
from SD, centred on the live GPS fix, with drag-to-pan and pinch/button
zoom. Independent of the RNS family — published under `reticulous/` for
discoverability, but any LCD buildable straddle can consume it. Doubles as
the docs' "first non-RNS feature straddle" walkthrough.

## What this straddle owns

```
maps/
├── esp-idf/
│   ├── include/maps.h
│   ├── src/maps.cpp        worker task + lcd-side compositor
│   └── lcd/src/maps_lcd.cpp  the LCD launcher program (slice)
└── tilebake/
    ├── tilebake            one-container pbf -> device tiles (wraps the image below)
    └── tilebake-image/     the container build context
        ├── Dockerfile
        ├── entrypoint.sh   in-container orchestrator (planetiler -> tileserver -> maketiles)
        ├── maketiles.py    the tiler: bake rendered rasters into the device SD tree (also standalone)
        ├── pbfbbox.py      read a pbf's header bounding box (no osmium)
        └── compose.py      merge per-map render caches into --out (host-side, coarsest-first)
```

There is no browser half — this is an on-device viewer.

## How others use it

The straddle uses `spangap-lcd`'s activator pattern. Add `reticulous/maps`
to your app's `straddle.yaml` `requires:` list, and a "Maps" tile
appears in the LCD launcher.

The map reads its tiles from SD at the default path
`/sdcard/maps/<z>/<x>/<y>.jpg` (or `.bin`), 256×256. Tiles are read on
demand and cached in PSRAM by the maps worker task.

GPS fix is read via **ephemeral `gps.*` storage keys** — there is no
compile-time GPS dependency. The GNSS chip can live in the consuming
buildable straddle (today: in hw-tdeck's `gps.cpp`) until a GPS
service abstraction earns its own straddle.

**Controls:** follows the GPS fix; drag to free-pan, the bottom-right
button re-centres. Zoom with pinch or the bottom-left `+`/`−` buttons —
zoom is capped to where the SD actually has tiles (it won't zoom into
blur, and auto-zooms-out if you leave a detailed area's coverage), and a
`z<NN>` pill flashes for 2 s on each change. See [INTERNALS.md](INTERNALS.md).

## Dependencies

- [spangap-core](../../s/spangap-core) — fs (SD), storage (gps.*).
- [spangap-lcd](../../s/spangap-lcd) — the LVGL launcher this hooks into.

## Tile baking

Tiles are baked on a workstation, not the device. There are two entry
points, depending on what you already have:

**You have rendered raster tiles (a `z/x/y` tree or your own tile
server URL):** use `tilebake/tilebake-image/maketiles.py` directly. It takes a bbox and
zoom range and produces the `/<z>/<x>/<y>.{jpg,bin}` tree the device
reads. It is *not* a renderer — it will not turn an `.osm.pbf` into
tiles, and it refuses to scrape `tile.openstreetmap.org`.

**You only have raw OSM data (an `.osm.pbf` extract, e.g. from
Geofabrik):** use `tilebake/tilebake`. It builds and runs a single
container that does the whole chain — renders the pbf to vector tiles
(planetiler), rasterises them through an internal tileserver-gl, and
bakes the device tree via `maketiles.py` — so all you supply is the
pbf, a bbox and a zoom range:

```
./tilebake/tilebake --out ./tiles --pbf berlin-latest.osm.pbf \
    --bbox 13.10 52.35 13.75 52.60 --zoom 10 16
```

`--out` is required (created if absent — there is no implicit output
dir). Before writing anything, tilebake prints a rough per-map and
total disk estimate and waits for a `y`; pass `--yes` to skip.

**Batch / layered maps:** drop the `--bbox`/`--zoom` and point it at a
`./pbf/<ceiling>/` tree — one folder per map, named by its top zoom
(e.g. `7/` empty for a world fill, `13/germany.osm.pbf`,
`16/berlin.osm.pbf`). Each map is rendered once into its own cache
(`./.tilebake-cache/render/<dir>`) and only re-rendered when its source
or the pipeline changes, so re-runs skip untouched maps. `--out` is then
composed from the caches coarsest-first: the shallowest map fills
everything and each deeper one overlays only where it actually has data
— so detail wins where the extract covers (real OSM roads vs the world
fill's Natural Earth). Drop the map's `.poly` binding polygon next to its
`.pbf` (Geofabrik ships one per extract) and the overlay is clipped to
that polygon at **pixel** granularity: tiles the border crosses are
composited — detailed inside the polygon, the coarser layer showing
through outside — so the seam follows the real data boundary, not a
rectangle. With no `.poly`, it falls back to whole-tile clipping at the
bbox. See the script header.

Needs Docker. The first run downloads planetiler's global source data
(water polygons, natural earth — ~1 GB) into `./.tilebake-cache`;
subsequent runs reuse it and are offline. See
[tilebake-image/](tilebake/tilebake-image/) for the image and the
in-container orchestrator.

Copy the resulting tree to your SD card under `maps/` and the device
picks it up.

## Read next

- [INTERNALS.md](INTERNALS.md) — tile format, worker/lcd split,
  on-device controls + zoom capping, cache policy, and the `tilebake`
  bake pipeline (planetiler → tileserver-gl → maketiles, per-map render
  cache, containment compose).
- The consuming-app doc:
  [docs/maps.md](../hw-tdeck/docs/maps.md).
