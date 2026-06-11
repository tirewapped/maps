# tilebake — one container that turns an .osm.pbf into device map tiles.
#
#   pbf ──► planetiler ──► OpenMapTiles-schema vector .mbtiles
#       ──► tileserver-gl (internal, GL-native raster render of that .mbtiles)
#       ──► maketiles.py  ──► /<z>/<x>/<y>.{jpg,bin} tree for the 'maps' app
#
# Based on maptiler/tileserver-gl so the GL-native + xvfb rendering stack
# (the hard part) is already solved. We add a JRE + planetiler for the
# pbf→vector step, Python/Pillow for the bake step, and an entrypoint that
# runs the whole chain with the server living entirely inside the container.
#
# Build context is the maps/tilebake dir; everything the image needs lives under
# tilebake-image/ and is COPY'd in below.
#   docker build -f tilebake-image/Dockerfile -t maps-tilebake .

# Pin to a digest in anger; a moving tag is fine for a workstation tool.
ARG TSG_TAG=latest
FROM maptiler/tileserver-gl:${TSG_TAG}

USER root
ENV DEBIAN_FRONTEND=noninteractive

# JRE for planetiler, Pillow+numpy for maketiles, xvfb so GL render works
# headless, jq/curl/git for the entrypoint's plumbing.
RUN apt-get update && apt-get install -y --no-install-recommends \
        openjdk-21-jre-headless \
        python3 python3-pil python3-numpy \
        osmium-tool \
        xvfb jq curl git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# planetiler: a single self-contained JAR. Default to the latest release
# asset; override PLANETILER_URL to pin a specific version for reproducibility.
ARG PLANETILER_URL=https://github.com/onthegomap/planetiler/releases/latest/download/planetiler.jar
RUN curl -fsSL "$PLANETILER_URL" -o /opt/planetiler.jar

# Reuse the OpenMapTiles GL style + fonts that the image already ships via its
# `tileserver-gl-styles` dependency (a maintainer-authored 'basic-preview'
# style known to render). We copy it out so the entrypoint can drop our
# freshly-built .mbtiles alongside it at run time, instead of hand-assembling a
# working GL style. node resolves the package path for us so it survives layout
# changes between image versions.
RUN STYLES_DIR="$(cd /usr/src/app && node -p \
        "require('path').dirname(require.resolve('tileserver-gl-styles/package.json'))")" \
    && mkdir -p /opt/tsdata \
    && cp -a "$STYLES_DIR"/. /opt/tsdata/ \
    && rm -f /opt/tsdata/*.mbtiles \
    && chmod -R a+rX /opt/tsdata

# An empty .osm.pbf for "world" bakes: planetiler insists on an OSM input even
# when every low-zoom layer comes from the cached Natural Earth / water data.
# Its OSM-derived layers come out empty; land/ocean/boundaries/places fill in.
RUN printf '<?xml version="1.0" encoding="UTF-8"?>\n<osm version="0.6" generator="tilebake"/>\n' \
        > /tmp/empty.osm \
    && osmium cat /tmp/empty.osm -o /opt/empty.osm.pbf \
    && rm -f /tmp/empty.osm

COPY tilebake-image/maketiles.py /opt/maketiles.py
COPY tilebake-image/pbfbbox.py /opt/pbfbbox.py
COPY tilebake-image/entrypoint.sh /opt/entrypoint.sh
RUN chmod +x /opt/entrypoint.sh /opt/maketiles.py

# Inputs arrive via env (see entrypoint.sh / tilebake):
#   IN_PBF, OUT, BBOX, ZMIN, ZMAX, FMT, QUALITY, STYLE, JAVA_MEM
ENV HOME=/tmp
WORKDIR /tmp
ENTRYPOINT ["/opt/entrypoint.sh"]
