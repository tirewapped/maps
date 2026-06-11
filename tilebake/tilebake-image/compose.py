#!/usr/bin/env python3
# compose.py OUT  CACHE1 BBOX1 POLY1  CACHE2 BBOX2 POLY2 ...
#
# Merge per-map render caches into the device tile tree OUT, coarsest first.
#   - The FIRST map is the base: every tile is copied (nothing lies beneath it).
#   - Each later (more detailed) map is an overlay, clipped to its region:
#       * with a .poly file  -> the real binding polygon, at PIXEL granularity:
#           tiles wholly inside the polygon are copied as-is; tiles wholly
#           outside are dropped; tiles the border crosses are COMPOSITED — the
#           detailed pixels inside the polygon, the coarser layer already in OUT
#           showing through outside it. The seam follows the true data boundary.
#       * with only a bbox   -> the old rectangular, whole-tile drop: a tile is
#           kept only if it lies fully inside the bbox, else dropped.
# Same-zoom tiles share z/x/y names (global slippy coords), so a later map
# cleanly overwrites (or composites onto) an earlier one — detailed always wins
# where it has data.
#
# BBOX is "minlon minlat maxlon maxlat"; POLY is a path to an osmosis .poly file
# (or empty). A present POLY takes precedence over BBOX. Runs host-side (OUT may
# be a removable card docker can't mount). Tiles that survive unmodified are
# hard-linked from the cache when OUT is on the same filesystem (instant, no
# extra space); composited border tiles are decoded, blended and re-encoded.
import sys, os, math, shutil, json
from PIL import Image, ImageDraw

EPS = 1e-9
TILE = 256
LAT_LIM = 85.05112878            # web-mercator clamp (poles -> +/-inf)


def tile_bounds(z, x, y):
    n = 1 << z
    w = x / n * 360.0 - 180.0
    e = (x + 1) / n * 360.0 - 180.0
    def lat(yy):
        return math.degrees(math.atan(math.sinh(math.pi * (1.0 - 2.0 * yy / n))))
    return w, lat(y + 1), e, lat(y)        # minlon, minlat, maxlon, maxlat


def project(lon, lat, z):
    """lon/lat -> global pixel (x, y) at zoom z (matches maketiles.deg2tile)."""
    n = 1 << z
    worldpx = float(TILE * n)
    lat = max(-LAT_LIM, min(LAT_LIM, lat))
    lat_r = math.radians(lat)
    gx = (lon + 180.0) / 360.0 * worldpx
    gy = (1.0 - math.log(math.tan(lat_r) + 1.0 / math.cos(lat_r)) / math.pi) / 2.0 * worldpx
    return gx, gy


# ---- .poly (osmosis polygon-filter format) ---------------------------------
# A name line, then one or more ring sections. Each section is a ring-id line
# (an id starting with '!' marks a hole), the "lon lat" vertex lines, and an
# "END". A final lone "END" closes the file. We keep (is_hole, [(lon,lat),...]).
def parse_poly(path):
    rings = []
    with open(path) as f:
        lines = [ln.strip() for ln in f]
    i = 1                                   # line 0 is the area name
    while i < len(lines):
        name = lines[i]; i += 1
        if name == "":
            continue
        if name == "END":                   # file terminator
            break
        is_hole = name.startswith("!")
        pts = []
        while i < len(lines) and lines[i] != "END":
            p = lines[i].split()
            if len(p) >= 2:
                try:
                    pts.append((float(p[0]), float(p[1])))
                except ValueError:
                    pass
            i += 1
        i += 1                              # consume this ring's END
        if len(pts) >= 3:
            rings.append((is_hole, pts))
    return rings


def poly_bbox(rings):
    xs = [lon for _, pts in rings for lon, _ in pts]
    ys = [lat for _, pts in rings for _, lat in pts]
    return min(xs), min(ys), max(xs), max(ys)


def build_mask(rings, z, x, y):
    """256x256 'L' mask: 255 inside the polygon, 0 outside (holes punched 0)."""
    m = Image.new("L", (TILE, TILE), 0)
    d = ImageDraw.Draw(m)
    ox, oy = x * TILE, y * TILE
    for is_hole, pts in rings:
        flat = [(gx - ox, gy - oy) for gx, gy in (project(lon, lat, z) for lon, lat in pts)]
        d.polygon(flat, fill=(0 if is_hole else 255))
    return m


# ---- tile I/O (jpg | raw RGB565 .bin) --------------------------------------
def load_rgb(path, fmt):
    if fmt == "bin":
        import numpy as np
        v = np.frombuffer(open(path, "rb").read(), dtype="<u2").reshape(TILE, TILE)
        r = ((v >> 11) & 0x1F) << 3
        g = ((v >> 5) & 0x3F) << 2
        b = (v & 0x1F) << 3
        return Image.fromarray(np.dstack([r, g, b]).astype("uint8"), "RGB")
    return Image.open(path).convert("RGB")


def save_rgb(img, path, fmt):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.exists(path):
        os.remove(path)
    if fmt == "bin":
        import numpy as np
        a = np.asarray(img.convert("RGB"), dtype=np.uint16)
        r, g, b = a[:, :, 0], a[:, :, 1], a[:, :, 2]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        open(path, "wb").write(v.astype("<u2").tobytes())
    else:
        img.convert("RGB").save(path, "JPEG", quality=80)


def underneath(out, z, x, y, fmt):
    """The coarser layer already in OUT beneath (z,x,y): the tile itself if
    present, else the nearest lower-zoom ancestor cropped+upscaled (the same
    overzoom the device does). None if nothing lies beneath."""
    cz, cx, cy = z, x, y
    while cz >= 0:
        p = os.path.join(out, str(cz), str(cx), "%d.%s" % (cy, fmt))
        if os.path.exists(p):
            img = load_rgb(p, fmt)
            if cz == z:
                return img
            scale = 1 << (z - cz)
            sub = TILE / scale                       # source span for this child
            ox = (x - cx * scale) * sub
            oy = (y - cy * scale) * sub
            s = max(1.0, sub)
            crop = img.crop((int(ox), int(oy), int(ox + s), int(oy + s)))
            return crop.resize((TILE, TILE))
        cz -= 1; cx >>= 1; cy >>= 1
    return None


def iter_tiles(cache):
    # Numeric zoom order (low -> high) so lower-zoom tiles are placed before the
    # higher zooms that may composite against them via underneath().
    zdirs = sorted((d for d in os.listdir(cache) if d.isdigit()), key=int)
    for zs in zdirs:
        zp = os.path.join(cache, zs)
        if not os.path.isdir(zp):
            continue
        for xs in os.listdir(zp):
            xp = os.path.join(zp, xs)
            if not (xs.isdigit() and os.path.isdir(xp)):
                continue
            for fn in os.listdir(xp):
                ys = os.path.splitext(fn)[0]
                if ys.isdigit():
                    yield int(zs), int(xs), int(ys), fn, os.path.join(xp, fn)


def link(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.exists(dst):
        os.remove(dst)
    try:
        os.link(src, dst)                  # same-fs: instant, shares the inode
    except OSError:
        shutil.copyfile(src, dst)          # cross-fs (e.g. SD card): copy


def main():
    out = sys.argv[1]
    rest = sys.argv[2:]
    maps = [(rest[i], rest[i + 1], rest[i + 2] if i + 2 < len(rest) else "")
            for i in range(0, len(rest), 3)]

    if os.path.isdir(out):                  # rebuild fresh — OUT is derived
        for e in os.listdir(out):
            p = os.path.join(out, e)
            shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
    else:
        os.makedirs(out, exist_ok=True)

    fmt = None
    union = None
    zlo = zhi = None
    copied = blended = 0
    for idx, (cache, bbox, poly) in enumerate(maps):
        if not os.path.isdir(cache):
            continue
        # idx 0 is the base fill: copy everything (nothing lies beneath it).
        rings = parse_poly(poly) if (idx > 0 and poly and os.path.isfile(poly)) else None
        pbb = poly_bbox(rings) if rings else None
        clip = tuple(map(float, bbox.split())) if (idx > 0 and not rings and bbox.strip()) else None

        for z, x, y, fn, src in iter_tiles(cache):
            tw, ts, te, tn = tile_bounds(z, x, y)
            tfmt = os.path.splitext(fn)[1].lstrip(".")
            dst = os.path.join(out, str(z), str(x), fn)

            if rings:
                # Quick reject: tile entirely outside the polygon's bbox.
                if (te < pbb[0] - EPS or tw > pbb[2] + EPS or
                        tn < pbb[1] - EPS or ts > pbb[3] + EPS):
                    continue
                mask = build_mask(rings, z, x, y)
                lo, hi = mask.getextrema()
                if hi == 0:
                    continue                         # wholly outside -> drop
                if lo == 255:
                    link(src, dst)                   # wholly inside -> as-is
                else:
                    under = underneath(out, z, x, y, tfmt)
                    if under is None:
                        link(src, dst)               # nothing beneath -> as-is
                    else:
                        comp = Image.composite(load_rgb(src, tfmt), under, mask)
                        save_rgb(comp, dst, tfmt)
                        blended += 1
            elif clip:
                if not (tw >= clip[0] - EPS and te <= clip[2] + EPS and
                        ts >= clip[1] - EPS and tn <= clip[3] + EPS):
                    continue
                link(src, dst)
            else:
                link(src, dst)

            copied += 1
            if fmt is None:
                fmt = tfmt
            zlo = z if zlo is None else min(zlo, z)
            zhi = z if zhi is None else max(zhi, z)
            if union is None:
                union = [tw, ts, te, tn]
            else:
                union = [min(union[0], tw), min(union[1], ts),
                         max(union[2], te), max(union[3], tn)]

    if fmt:
        with open(os.path.join(out, "maps.json"), "w") as f:
            json.dump({"format": fmt, "tile": TILE,
                       "bbox": [round(v, 5) for v in union],
                       "zoom": [zlo, zhi]}, f, indent=2)
    sys.stderr.write("  composed %d tiles (%d border tiles blended)\n" % (copied, blended))


if __name__ == "__main__":
    main()
