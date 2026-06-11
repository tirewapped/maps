# maps — internals

## Tile format

`/<z>/<x>/<y>` standard slippy-map tiles, 256×256, in one of two encodings;
the worker tries them in order:

- **`.jpg`** — baseline JPEG (what `tilebake` writes by default, quality 80).
  Decoded on the worker via `esp_jpeg` (TJpgDec). **Baseline only** —
  progressive / arithmetic / CMYK / 12-bit JPEGs are rejected by the decoder.
- **`.bin`** — raw little-endian RGB565, exactly `256*256*2 = 131072` bytes, no
  header. LVGL's native canvas order (byte-swapped for the ST7789 on flush).
  Bigger on disk but zero-decode; the fallback when no `.jpg` is present.

Slippy coords are **global**: tile `(z, x, y)` is the same geographic square no
matter which source produced it, so layering different maps is just an
overwrite by path (see the bake pipeline's compose step).

**SD cluster-size gotcha:** a tile tree is 10⁵+ tiny files. On a card with large
FAT clusters (exFAT on big SDXC cards defaults to 64–128 KB) each ~7 KB tile
consumes a whole cluster, so `du` on the card can read ~10× the real data. Format
the card **FAT32 with small clusters** (8 KB ≈ one cluster per tile); the
firmware's own `format sd [KB]` / `CONFIG_SPANGAP_SDCARD_ALLOC_KB` default to
8 KB for the same reason (the device's FatFs has no exFAT support, so big cards
must be FAT32 anyway).

## Two halves

- **Worker task** (`maps_lcd.cpp`) — owns the PSRAM tile cache (LRU, ~16 tiles),
  the slippy-map math (`lon/lat ↔ global pixel`), and the view-centre / follow /
  zoom state. Subscribes to `gps.lat`/`gps.lon` and `s.maps.*`; on any change it
  fetches the visible tiles (or overzoom ancestors) from SD into the cache and
  hands a composite request to the LCD task.
- **LCD task** — owns the LVGL canvas, the control buttons, the drag/pinch
  handlers and the zoom pill. `composite()` blits cached tiles (scaled for
  overzoom) into the canvas, draws the marker, toggles the status label.

Why split: SD tile reads are slow (fs worker); doing them on the LCD task would
stutter the UI. The cache is shared under `s_mux`; control flags (pan delta,
recentre, zoom-step) pass lcd→worker under `s_ctrlMux`.

## On-device controls

- **Follow / pan** — follows the GPS fix; a drag (touch or trackball-press)
  free-pans and unlocks follow; the bottom-right "centre me" button re-locks.
- **Zoom** — pinch (two-finger; live-scales the canvas for feedback, then steps
  the zoom by the nearest power-of-two of the pinch ratio on release) **and**
  grey `+`/`−` buttons bottom-left. Both post a step to the worker.
- **Zoom capped to available data** — the worker only changes to a zoom where a
  real (non-overzoom) tile exists at the view centre (`realTileAt`, a cache hit
  or an `fs_stat`). So you can't zoom in past where the SD actually has detail;
  and if you pan out of a high-zoom area's coverage it **auto-zooms-out** until a
  real tile reappears. The effective level is persisted to `s.maps.zoom`.
- **Zoom pill** — a rounded `z<NN>` overlay (bottom-left, above the buttons)
  flashes for 2 s on any effective-zoom change (button, pinch, or auto-zoom-out),
  hidden by a one-shot LVGL timer.
- **Overzoom fallback** — a tile missing at the display zoom is filled by
  upscaling the nearest existing lower-zoom ancestor, so a coarse base "shows
  through" instead of going grey (also how the bake's edge-dropped tiles render).

## GPS as ephemeral storage

The map reads ephemeral `gps.*` keys (no `s.` prefix; lost on reboot, synced
live). Today they're written by hw-tdeck's `gps.cpp`; any future "GPS service"
abstraction just needs to write the same keys.

- `gps.lat` / `gps.lon` — WGS84 degrees (the fix the map centres on)
- `gps.sats_view` / `gps.snr` — shown on the acquisition screen before a fix

## Cache policy

LRU, default 16 tiles. Sized for the visible grid + overzoom ancestors + a pan
margin. JPEG decode failures are *not* cached (so a transient bad tile is
retried), and the decoder logs a throttled warning rather than silently looking
like "no tiles here".

## The bake pipeline (`tilebake`)

Tiles are produced on a workstation, never on the device. `tilebake` runs the
whole chain in one Docker image (built from `tilebake-image/`, tagged with a
content hash of its inputs so an edit auto-rebuilds it):

```
.osm.pbf ─planetiler→ OpenMapTiles vector .mbtiles
         ─tileserver-gl (GL render, headless via Xvfb)→ raster PNGs
         ─maketiles.py→ /<z>/<x>/<y>.{jpg,bin} device tree
```

- **planetiler** turns the pbf into an OMT-schema vector `.mbtiles`. It
  auto-downloads Natural Earth + water polygons once (~1 GB, cached under
  `.tilebake-cache/data`, then offline). `--bounds` = the map's bbox.
  - `--maxzoom` is the requested ceiling, so the whole pipeline renders natively
    at the selected zoom (no overzoom). Caveat from the data, not the tool:
    planetiler/OpenMapTiles cap vector zoom around **z15**, and OSM-via-OMT
    carries no real map detail above **~z14** — so a higher ceiling may be
    rejected by planetiler, and where it isn't it only yields *larger* tiles, not
    *more* detail (the deepest meaningful zoom is the data's, ~z14).
  - We deliberately set **no `--minzoom`** (always render from z0). A vector
    source with a *non-zero* minzoom renders **blank** at exactly that bottom
    level in tileserver-gl/MapLibre (no parent tile to compose it from), which
    would clobber the layer beneath. Rendering from z0 keeps every baked zoom
    above the source floor.
- **tileserver-gl** GL-renders the vector tiles to raster PNGs on loopback,
  reusing the image's bundled `basic-preview` style + Noto fonts. Needs an X
  display, so the entrypoint runs Xvfb.
- **maketiles.py** fetches the raster tiles for the zoom range over the bbox and
  writes the device tree. Also usable standalone against any `{z}/{x}/{y}` URL or
  rendered tree (it refuses `tile.openstreetmap.org` per the tile policy).
- **bbox** is read straight from each pbf's header (`pbfbbox.py`, a ~30-line
  protobuf parse — no osmium, instant on multi-GB files). osmium's full scan is
  only an in-container fallback for extracts lacking a header bbox.

### Per-map render cache + incremental

Each map renders once into its own tree under `.tilebake-cache/render/<dir>/`,
stamped with a signature (`source size+mtime | zoom range | format | quality |
image hash`). A re-run skips a map whose signature still matches, so changing one
extract re-renders only that one; the image-hash term means a pipeline change
invalidates every cache.

### Compose — layering by containment

`compose.py` (host-side, so `--out` can be a card Docker can't mount) rebuilds
`--out` from the caches, **coarsest ceiling first**:

- the shallowest map is the **base** — every tile copied (nothing lies beneath);
- each deeper map is an **overlay**, clipped to its region.

Because tile names are global, a later map cleanly overwrites (or composites
onto) an earlier one, so the most-detailed map wins wherever it covers. The clip
takes one of two forms:

- **`.poly` polygon (pixel-accurate).** Drop the extract's binding polygon —
  Geofabrik ships `<name>.poly` next to `<name>.osm.pbf`, the exact cut shape —
  into the map's folder. compose rasterises it into a 256×256 mask per tile:
  tiles wholly inside are hard-linked as-is, wholly-outside dropped, and the ones
  the border crosses are **composited** — detailed pixels inside the polygon, the
  layer already in `--out` showing through outside it (the nearest lower-zoom
  ancestor upscaled when no same-zoom tile exists, the same overzoom the device
  does). This kills the regional extract's "roads fade to plain land" edge: the
  seam now follows the real data boundary at the pixel.
- **bbox (whole-tile).** With no `.poly`, a tile is kept only if it lies fully
  inside the map's bbox — dropping the rectangle edge and too-zoomed-out tiles.
  Exact for a bbox-cut extract (its true boundary *is* the rectangle).

Unmodified tiles are hard-linked from the cache when `--out` is on the same
filesystem (instant, no extra space) or copied to a card; composited border tiles
are decoded, blended and re-encoded (needs Pillow; `--bin` also needs numpy).

Before any of this, `tilebake` prints a rough per-map + total size estimate
(tile count × a calibrated per-tile mean) and waits for confirmation.

## Why this straddle isn't in the RNS family

It has nothing to do with Reticulum — the first **non-RNS feature straddle** on
the platform, an end-to-end example of an LCD program built on spangap-core +
spangap-lcd and nothing else. Published under `reticulous/` (GitHub org) for
discoverability; the dep graph is RNS-free.
