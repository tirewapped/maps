#!/usr/bin/env python3
# Print an .osm.pbf's header bounding box as "minlon minlat maxlon maxlat".
#
# Reads only the first OSMHeader blob (a few KB at the front of the file) — no
# osmium, no full scan, instant even on a multi-GB extract. Exits non-zero and
# prints nothing if the file has no usable header bbox, so callers can fall back.
#
# This replaces an `osmium fileinfo -g … | tr -cd '0-9.,-'` scrape that, when
# osmium printed anything beyond the four box numbers, swept the extra digits
# into a garbage oversized bbox — which silently made the bake cover ~9x the
# intended area. Parsing the header gives exactly four numbers, every time.
import sys, struct, zlib


def _varint(b, i):
    shift = res = 0
    while True:
        x = b[i]; i += 1
        res |= (x & 0x7f) << shift
        if not x & 0x80:
            return res, i
        shift += 7


def _fields(b):
    """Minimal protobuf decode -> {field_number: [value, ...]}."""
    i, n, out = 0, len(b), {}
    while i < n:
        tag, i = _varint(b, i); fn, wt = tag >> 3, tag & 7
        if wt == 0:    v, i = _varint(b, i)
        elif wt == 2:  ln, i = _varint(b, i); v = b[i:i + ln]; i += ln
        elif wt == 1:  v = b[i:i + 8]; i += 8
        elif wt == 5:  v = b[i:i + 4]; i += 4
        else: raise ValueError("bad wire type %d" % wt)
        out.setdefault(fn, []).append(v)
    return out


def _zz(n):                       # protobuf sint64 zigzag decode
    return (n >> 1) ^ -(n & 1)


def main():
    try:
        f = open(sys.argv[1], 'rb')
        hlen = struct.unpack('>I', f.read(4))[0]      # BlobHeader length (BE u32)
        bh = _fields(f.read(hlen))                    # 1=type, 3=datasize
        if bytes(bh[1][0]) != b'OSMHeader':
            return 1
        blob = _fields(f.read(bh[3][0]))              # 1=raw | 3=zlib_data
        if 1 in blob:
            data = bytes(blob[1][0])
        elif 3 in blob:
            data = zlib.decompress(bytes(blob[3][0]))
        else:
            return 1                                  # lzma/zstd — let caller fall back
        hb = _fields(data)                            # HeaderBlock: 1=bbox
        if 1 not in hb:
            return 1
        bb = _fields(bytes(hb[1][0]))                 # HeaderBBox: 1=L 2=R 3=T 4=B (nanodeg)
        w, e, n, s = (_zz(bb[1][0]) / 1e9, _zz(bb[2][0]) / 1e9,
                      _zz(bb[3][0]) / 1e9, _zz(bb[4][0]) / 1e9)
    except Exception:
        return 1
    # Reject a nonsensical box rather than feed a wrong area to the bake.
    if not (-180 <= w < e <= 180 and -90 <= s < n <= 90):
        return 1
    print("%.7f %.7f %.7f %.7f" % (w, s, e, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
