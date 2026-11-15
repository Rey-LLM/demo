#!/usr/bin/env python3
"""Convert Natural Earth coastline GeoJSON into ftviz's compact binary asset.

Usage:
    python3 tools/build_coastlines.py ne_110m_coastline.geojson assets/coastlines_110m.bin
    python3 tools/build_coastlines.py ne_50m_coastline.geojson  assets/coastlines_50m.bin

Input: the GeoJSON coastline layers of Natural Earth (public domain), release
v5.1.2, from https://github.com/nvkelso/natural-earth-vector/tree/v5.1.2/geojson.
Only the Python standard library is used. The output is deterministic: the same
input gives the same bytes.

Output layout (little-endian):
    char[4]  magic        "FTCL"
    u32      version      1
    u32      polylines    P
    u32      points       N
    u32      offsets[P+1] first point of each polyline; offsets[P] == N
    f32      latlon[2N]   latitude, longitude in degrees, per point

A polyline of k points contributes k-1 segments. The renderer draws one
instance per segment.
"""

import json
import struct
import sys


def polylines(doc):
    for feature in doc["features"]:
        geom = feature["geometry"]
        if geom is None:
            continue
        if geom["type"] == "LineString":
            yield geom["coordinates"]
        elif geom["type"] == "MultiLineString":
            yield from geom["coordinates"]
        else:
            raise ValueError("unexpected geometry type: " + geom["type"])


def main(argv):
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    with open(argv[1], "r", encoding="utf-8") as f:
        doc = json.load(f)

    offsets = [0]
    latlon = []
    for line in polylines(doc):
        if len(line) < 2:
            continue
        for lon, lat, *_ in line:
            latlon.append((float(lat), float(lon)))
        offsets.append(len(latlon))

    with open(argv[2], "wb") as out:
        out.write(b"FTCL")
        out.write(struct.pack("<III", 1, len(offsets) - 1, len(latlon)))
        out.write(struct.pack("<%dI" % len(offsets), *offsets))
        for lat, lon in latlon:
            out.write(struct.pack("<ff", lat, lon))

    segments = len(latlon) - (len(offsets) - 1)
    print("%s: %d polylines, %d points, %d segments" % (argv[2], len(offsets) - 1, len(latlon), segments))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
