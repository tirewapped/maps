#!/usr/bin/env python3
# style_compose.py MANIFEST STYLE_JSON
#
# Rewrite a single-source MapLibre style (the bundled OpenMapTiles 'basic-preview')
# into a MULTI-source composite, in place. The maps straddle bakes several vector
# tilesets at different zoom ceilings (a world fill, a country, a city...); to get
# ONE seamless raster out of tileserver-gl we serve them all together and stack the
# style's data layers once per source, coarsest source first so the most detailed
# draws on top.
#
# Because each coarse source was polygon-clipped at render time to exclude wherever
# a deeper map covers (see entrypoint.sh's render mode), the stacked layers form a
# clean partition: inside a detailed map only its own data exists, so nothing
# coarse co-draws there; across a border the next-coarser source shows through and,
# being a vector source, is OVERZOOMED crisply rather than upscaled as a blurry
# raster. Two detailed maps that meet in one tile both contribute their data.
#
# MANIFEST is the same JSON the bake driver writes:
#   {"sources":[{"id":"7", "maxzoom":7, ...}, {"id":"13", ...}, ...]}  coarsest-first.
# Sources are keyed POSITIONALLY (s0, s1, ... coarsest-first) so the style/data ids
# and the mbtiles://{token} are always valid, collision-free identifiers regardless
# of the maps' folder names (e.g. purely-numeric "7"/"13"). entrypoint.sh's data
# config and mbtiles symlinks use the same s<index> scheme.
import copy
import json
import sys


def main():
    manifest_path, style_path = sys.argv[1], sys.argv[2]
    n = len(json.load(open(manifest_path))["sources"])
    keys = ["s%d" % i for i in range(n)]        # coarsest -> deepest
    style = json.load(open(style_path))

    layers = style.get("layers", [])
    # A layer without a "source" (the "background" fill) is source-independent —
    # keep it once, beneath everything. The rest are the OMT data layers we clone.
    base_layers = [l for l in layers if not l.get("source")]
    data_layers = [l for l in layers if l.get("source")]

    new_layers = list(base_layers)
    for key in keys:                            # coarsest -> deepest (drawn on top)
        for l in data_layers:
            cl = copy.deepcopy(l)
            cl["id"] = "%s__%s" % (l["id"], key)
            cl["source"] = key
            new_layers.append(cl)

    style["layers"] = new_layers
    style["sources"] = {
        key: {"type": "vector", "url": "mbtiles://{%s}" % key} for key in keys
    }

    json.dump(style, open(style_path, "w"))


if __name__ == "__main__":
    main()
