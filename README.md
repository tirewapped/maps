# maps

## What is this?

**maps** is an offline RGB565 slippy-map viewer for spangap LCD devices:
an LCD launcher program that blits pre-baked map tiles read from SD,
centred on the live GPS fix. Independent of the RNS family — published
under `reticulous/` for discoverability, but any LCD buildable straddle can
consume it. Doubles as the docs' "first non-RNS feature straddle"
walkthrough.

## What this straddle owns

```
maps/
├── esp-idf/
│   ├── include/maps.h
│   ├── src/maps.cpp        worker task + lcd-side compositor
│   └── lcd/src/maps_lcd.cpp  the LCD launcher program (slice)
└── scripts/
    └── maketiles.py        workstation tool: bake tiles into the device's SD format
```

There is no browser half — this is an on-device viewer.

## How others use it

The straddle uses `spangap-lcd`'s activator pattern. Add `reticulous/maps`
to your app's `straddle.yaml` `requires:` list, and a "Maps" tile
appears in the LCD launcher.

The map reads its tiles from SD at the default path
`/sdcard/maps/<z>/<x>/<y>.bin` (RGB565, 256×256). Tiles are read on
demand and cached in PSRAM by the maps worker task.

GPS fix is read via **ephemeral `gps.*` storage keys** — there is no
compile-time GPS dependency. The GNSS chip can live in the consuming
buildable straddle (today: in hw-tdeck's `gps.cpp`) until a GPS
service abstraction earns its own straddle.

## Dependencies

- [spangap-core](../../s/spangap-core) — fs (SD), storage (gps.*).
- [spangap-lcd](../../s/spangap-lcd) — the LVGL launcher this hooks into.

## Tile baking

Tiles are baked on a workstation, not the device. There are two entry
points, depending on what you already have:

**You have rendered raster tiles (a `z/x/y` tree or your own tile
server URL):** use `scripts/maketiles.py` directly. It takes a bbox and
zoom range and produces the `/<z>/<x>/<y>.{jpg,bin}` tree the device
reads. It is *not* a renderer — it will not turn an `.osm.pbf` into
tiles, and it refuses to scrape `tile.openstreetmap.org`.

**You only have raw OSM data (an `.osm.pbf` extract, e.g. from
Geofabrik):** use `scripts/tilebake.sh`. It builds and runs a single
container that does the whole chain — renders the pbf to vector tiles
(planetiler), rasterises them through an internal tileserver-gl, and
bakes the device tree via `maketiles.py` — so all you supply is the
pbf, a bbox and a zoom range:

```
./scripts/tilebake.sh --pbf berlin-latest.osm.pbf --out ./out \
    --bbox 13.10 52.35 13.75 52.60 --zoom 10 16
```

Needs Docker. The first run downloads planetiler's global source data
(water polygons, natural earth — ~1 GB) into `./.tilebake-cache`;
subsequent runs reuse it and are offline. See
[tilebake/](scripts/tilebake/) for the image and the in-container
orchestrator.

Copy the resulting tree to your SD card under `maps/` and the device
picks it up.

## Read next

- [INTERNALS.md](INTERNALS.md) — tile format, worker/lcd split,
  slippy-map math, cache policy.
- The consuming-app doc:
  [docs/maps.md](../hw-tdeck/docs/maps.md).
