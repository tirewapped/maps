# maps — internals

## Tile format

`<z>/<x>/<y>.bin` — raw RGB565, 256×256 = 131 072 bytes per tile. No
compression, no palette, no header. Cheap to mmap-style read; the cost
is SD capacity, which is the right trade-off for ESP32-S3 PSRAM-
constrained devices.

## Two halves

- **Worker task** — owns the tile cache (LRU in PSRAM, ~16 tiles by
  default), does the slippy-math (`lon/lat → tile(z, x, y) + pixel
  offset`), reads tiles from SD on demand via `fs_*` workers.
- **LCD compositor** — the LVGL canvas in `maps_lcd.cpp` composites
  the worker's tile output onto the screen, draws the position marker,
  centers on the GPS fix.

Why split: tile read is potentially slow (SD I/O via fs worker). Doing
it on the LCD task would stutter the UI. The worker task pre-fetches
the 3×3 grid around the current centre while the LCD task animates.

## GPS as ephemeral storage

The map reads ephemeral `gps.*` keys (no `s.` prefix; lost on reboot,
synced live). Today the keys are written by reticulous-tdeck's
`gps.cpp`. Any future "GPS service" abstraction just needs to write
the same keys for the maps straddle to keep working.

Relevant keys:

- `gps.fix.lat` / `gps.fix.lon` — WGS84 degrees
- `gps.fix.alt_m` — altitude in metres
- `gps.fix.fix_quality` — 0 = none, 1 = 2D, 2 = 3D
- `gps.fix.heading_deg` — for orienting the marker, if present

## Cache policy

LRU, default capacity 16 tiles (configurable). At 131 KB / tile that's
2 MB of PSRAM — comfortable on an 8 MB part.

## Why this straddle isn't in the RNS family

It has nothing to do with Reticulum. It is the first **non-RNS feature
straddle** built on the platform — a useful end-to-end example of
adding an LCD program that consumes spangap-core + spangap-lcd and
nothing else.

It is published under `reticulous/` (GitHub org) for discoverability
alongside the other straddles a T-Deck owner is likely to want, but
the dep graph is RNS-free.

## `maketiles.py` — what it does

Workstation script (Python, not run on the device). Inputs:

- A bounding box (or polygon) and zoom range.
- A source basemap — raster TMS endpoint, MBTiles, GeoTIFF.

Outputs: the `/<z>/<x>/<y>.bin` tree, ready to copy to the SD card.

Run it once per region you care about; map tiles are large but
storage-cheap. Public-domain basemaps (OSM, OpenTopo) are fine to
bake.
