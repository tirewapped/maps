#!/usr/bin/env python3
"""
maketiles.py — build offline map tiles for the diptych 'maps' app.

The on-device 'maps' viewer (reticulous/main/maps.cpp) reads tiles straight off
the SD card. Two on-card formats, both 256x256:

    <out>/<z>/<x>/<y>.jpg   JPEG (default) — ~7-13x smaller, decoded on device
    <out>/<z>/<x>/<y>.bin   raw little-endian RGB565 — zero decode, but bulky

Default is JPEG: a 1 GB card reaches roughly world-z8/9, vs ~world-z6 for raw.
The device decodes JPEG on its worker task (esp_jpeg/TJpgDec), so the UI stays
responsive. Use --bin only if you specifically want zero-decode raw tiles.

Copy the output tree to the SD card so the device sees /sdcard/maps (the default
s.maps.tiledir).

Input — pick one:
  * a local z/x/y tile tree you rendered yourself (PNG/JPG), or
  * a URL template for your OWN tile server, e.g. 'https://host/{z}/{x}/{y}.png'.

DO NOT bulk-download from tile.openstreetmap.org — it violates the OSM tile
usage policy (https://operations.osmfoundation.org/policies/tiles/). Render your
own with Maperitive / QGIS / tilemaker, or point this at your own / keyed server.

Examples:
  ./maketiles.py --src /maps/render --out ./sdmaps \\
      --bbox 13.30 52.45 13.50 52.60 --zoom 12 16            # JPEG q80
  ./maketiles.py --src 'https://tiles.example/{z}/{x}/{y}.png' --out ./sdmaps \\
      --bbox 13.30 52.45 13.50 52.60 --zoom 12 16 --quality 85
  ./maketiles.py --src /maps/render --out ./sdmaps --bin ...  # raw RGB565

Requires: Pillow (and numpy only for --bin).
"""
import argparse
import io
import json
import math
import os
import sys
import time
import urllib.request

try:
    from PIL import Image
except ImportError:
    sys.exit("need Pillow: pip install pillow  (and numpy too for --bin)")

TILE = 256


def deg2tile(lon, lat, z):
    n = 1 << z
    x = int((lon + 180.0) / 360.0 * n)
    lat_r = math.radians(lat)
    y = int((1.0 - math.log(math.tan(lat_r) + 1.0 / math.cos(lat_r)) / math.pi) / 2.0 * n)
    return max(0, min(n - 1, x)), max(0, min(n - 1, y))


def to_rgb565_le(img):
    import numpy as np
    a = np.asarray(img.convert("RGB"), dtype=np.uint16)
    r, g, b = a[:, :, 0], a[:, :, 1], a[:, :, 2]
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return v.astype("<u2").tobytes()


def save_tile(img, base, fmt, quality):
    if img.size != (TILE, TILE):
        img = img.resize((TILE, TILE))
    if fmt == "bin":
        with open(base + ".bin", "wb") as f:
            f.write(to_rgb565_le(img))
    else:
        img.convert("RGB").save(base + ".jpg", "JPEG", quality=quality)


def load_tile(src, z, x, y, ua, delay):
    if "{z}" in src or "{x}" in src or "{y}" in src:
        url = src.format(z=z, x=x, y=y)
        req = urllib.request.Request(url, headers={"User-Agent": ua})
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                data = r.read()
            if delay:
                time.sleep(delay)
            return Image.open(io.BytesIO(data))
        except Exception as e:
            print(f"  fetch {url}: {e}", file=sys.stderr)
            return None
    for ext in (".png", ".jpg", ".jpeg", ".webp"):
        p = os.path.join(src, str(z), str(x), f"{y}{ext}")
        if os.path.exists(p):
            return Image.open(p)
    return None


def main():
    ap = argparse.ArgumentParser(description="Build offline map tiles for the diptych 'maps' app.")
    ap.add_argument("--src", required=True,
                    help="local z/x/y tile dir, OR a URL template '.../{z}/{x}/{y}.png' (your own server)")
    ap.add_argument("--out", required=True, help="output dir (copy its contents to SD /sdcard/maps)")
    ap.add_argument("--bbox", nargs=4, type=float, required=True,
                    metavar=("MINLON", "MINLAT", "MAXLON", "MAXLAT"), help="area to cover")
    ap.add_argument("--zoom", nargs=2, type=int, required=True, metavar=("ZMIN", "ZMAX"))
    fmt = ap.add_mutually_exclusive_group()
    fmt.add_argument("--jpg", dest="fmt", action="store_const", const="jpg", help="JPEG tiles (default)")
    fmt.add_argument("--bin", dest="fmt", action="store_const", const="bin", help="raw RGB565 tiles")
    ap.set_defaults(fmt="jpg")
    ap.add_argument("--quality", type=int, default=80, help="JPEG quality (default 80)")
    ap.add_argument("--ua", default="diptych-maketiles/1.0", help="HTTP User-Agent for --src URL mode")
    ap.add_argument("--delay", type=float, default=0.1, help="seconds between fetches in URL mode")
    args = ap.parse_args()

    minlon, minlat, maxlon, maxlat = args.bbox
    zmin, zmax = sorted(args.zoom)
    is_url = "{z}" in args.src
    if is_url and "openstreetmap.org" in args.src:
        sys.exit("refusing to scrape tile.openstreetmap.org — see the OSM tile policy in this script's header")

    total = missing = 0
    for z in range(zmin, zmax + 1):
        x0, y0 = deg2tile(minlon, maxlat, z)   # NW
        x1, y1 = deg2tile(maxlon, minlat, z)   # SE
        if x1 < x0:
            x0, x1 = x1, x0
        if y1 < y0:
            y0, y1 = y1, y0
        print(f"z{z}: x[{x0}..{x1}] y[{y0}..{y1}] = {(x1 - x0 + 1) * (y1 - y0 + 1)} tiles")
        for x in range(x0, x1 + 1):
            od = os.path.join(args.out, str(z), str(x))
            os.makedirs(od, exist_ok=True)
            for y in range(y0, y1 + 1):
                img = load_tile(args.src, z, x, y, args.ua, args.delay if is_url else 0)
                if img is None:
                    missing += 1
                    continue
                save_tile(img, os.path.join(od, str(y)), args.fmt, args.quality)
                total += 1

    # Merge with any existing manifest so appending zoom levels / areas to the
    # same dir widens the recorded range instead of clobbering it.
    mpath = os.path.join(args.out, "maps.json")
    zlo, zhi = zmin, zmax
    bb = list(args.bbox)
    if os.path.exists(mpath):
        try:
            prev = json.load(open(mpath))
            pz = prev.get("zoom")
            if pz:
                zlo, zhi = min(zlo, pz[0]), max(zhi, pz[1])
            pb = prev.get("bbox")
            if pb and len(pb) == 4:
                bb = [min(bb[0], pb[0]), min(bb[1], pb[1]), max(bb[2], pb[2]), max(bb[3], pb[3])]
        except Exception:
            pass
    with open(mpath, "w") as f:
        json.dump({"format": args.fmt, "tile": TILE, "bbox": bb, "zoom": [zlo, zhi]}, f, indent=2)
    print(f"\nwrote {total} {args.fmt} tiles ({missing} source missing) -> {args.out}")
    print(f"copy '{args.out}/'* to the SD card so the device sees /sdcard/maps/<z>/<x>/<y>.{args.fmt}")


if __name__ == "__main__":
    main()
