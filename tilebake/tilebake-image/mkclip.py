#!/usr/bin/env python3
# mkclip.py OUT.poly  OUTER  [HOLE ...]
#
# Emit an osmosis .poly that is OUTER with every HOLE punched out of it, so
# `osmium extract -p OUT.poly src.pbf` keeps the source's data inside OUTER but
# DROPS it inside any HOLE. tilebake uses this to clip a coarse map to (its own
# area MINUS every deeper map that overlaps it): the holes are the deeper maps'
# boundaries, so the coarse map's data is physically removed wherever a more
# detailed map will cover — the partition that lets the renderer stack sources
# without the coarse roads/rivers co-drawing inside the detailed area.
#
# OUTER and each HOLE is either
#   * a path to an existing .poly file  (its non-hole rings are used), or
#   * a bbox "minlon,minlat,maxlon,maxlat"  (a rectangle).
#
# Runs host-side (pure stdlib) so the bake driver can hash the result into a map's
# render-cache signature — change the set of deeper maps and the clip changes and
# the coarse map re-renders.
import os
import sys


def parse_poly_outers(path):
    """Return the OUTER rings of an osmosis .poly as [[(lon,lat),...], ...],
    skipping hole rings (names starting with '!')."""
    rings = []
    with open(path) as f:
        lines = [ln.strip() for ln in f]
    i = 1                                       # line 0 is the area name
    while i < len(lines):
        name = lines[i]; i += 1
        if name == "" or name == "END":
            if name == "END":
                break
            continue
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
        i += 1                                  # consume this ring's END
        if len(pts) >= 3 and not is_hole:
            rings.append(pts)
    return rings


def rings_of(token):
    """OUTER rings for a token: a .poly path -> its outer rings; a
    'minlon,minlat,maxlon,maxlat' bbox -> a single rectangle ring."""
    if os.path.isfile(token):
        return parse_poly_outers(token)
    parts = token.split(",")
    if len(parts) == 4:
        w, s, e, n = (float(v) for v in parts)
        return [[(w, s), (e, s), (e, n), (w, n), (w, s)]]
    raise SystemExit("mkclip: not a .poly file or 'w,s,e,n' bbox: %s" % token)


def main():
    out_path = sys.argv[1]
    outer = sys.argv[2]
    holes = sys.argv[3:]

    sections = []                               # (is_hole, ring)
    for r in rings_of(outer):
        sections.append((False, r))
    for h in holes:
        for r in rings_of(h):
            sections.append((True, r))

    with open(out_path, "w") as f:
        f.write("clip\n")
        for idx, (is_hole, ring) in enumerate(sections):
            f.write("%s%d\n" % ("!" if is_hole else "", idx + 1))
            for lon, lat in ring:
                f.write("   %.7f   %.7f\n" % (lon, lat))
            f.write("END\n")
        f.write("END\n")


if __name__ == "__main__":
    main()
