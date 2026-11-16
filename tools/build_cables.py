#!/usr/bin/env python3
"""Convert submarine cable GeoJSON into ftviz's assets/cables.bin.

Usage:
    python3 -I tools/build_cables.py CABLES.geojson [LANDINGS.geojson] assets/cables.bin

CABLES.geojson: LineString / MultiLineString features, one per cable or
cable segment (lon, lat in degrees, WGS 84). LANDINGS.geojson (optional):
Point features for landing stations; without it the endpoints of every
polyline are used. The source dataset and its licence are recorded in
THIRD_PARTY_NOTICES.md; this script does not download anything.

Processing, deterministic (same input, same bytes):
  * every polyline is simplified with Douglas-Peucker on the sphere at a
    5 km tolerance (cross-track distance), endpoints kept;
  * each cable is classified into a corridor from the regions its vertices
    touch: TRANS_ATLANTIC, TRANS_PACIFIC, EUROPE_ASIA, INTRA_ASIA or OTHER;
  * polylines are sorted by (corridor, first point) for a stable order.

Output layout (little endian), read by src/model/cables.cpp:
    char[4] "FTCB", u32 version 1, u32 cable_count, u32 point_count,
    u32 landing_count, u32 reserved 0
    cable_count x { u32 first_point, u32 point_count, u8 corridor, u8[3] 0 }
    point_count x { f32 lat, f32 lon }
    landing_count x { f32 lat, f32 lon }

Only the Python standard library is used.
"""

import json
import math
import struct
import sys

EARTH_KM = 6371.0088
TOLERANCE_KM = 5.0
CORRIDORS = ["OTHER", "TRANS_ATLANTIC", "TRANS_PACIFIC", "EUROPE_ASIA", "INTRA_ASIA"]


def unit(lat, lon):
    la, lo = math.radians(lat), math.radians(lon)
    return (math.cos(la) * math.sin(lo), math.sin(la), math.cos(la) * math.cos(lo))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def norm(a):
    return math.sqrt(dot(a, a))


def angle(a, b):
    return math.atan2(norm(cross(a, b)), dot(a, b))


def cross_track_km(p, a, b):
    """Distance from p to the great-circle segment ab (km)."""
    n = cross(a, b)
    ln = norm(n)
    if ln < 1e-12:
        return angle(p, a) * EARTH_KM
    n = (n[0] / ln, n[1] / ln, n[2] / ln)
    d = math.asin(max(-1.0, min(1.0, dot(p, n))))
    # Closest point on the great circle; inside the segment if it lies between a and b.
    c = (p[0] - n[0] * math.sin(d), p[1] - n[1] * math.sin(d), p[2] - n[2] * math.sin(d))
    if abs(angle(a, c) + angle(c, b) - angle(a, b)) < 1e-9:
        return abs(d) * EARTH_KM
    return min(angle(p, a), angle(p, b)) * EARTH_KM


def douglas_peucker(points, tol_km):
    if len(points) <= 2:
        return points
    vec = [unit(lat, lon) for lat, lon in points]
    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    while stack:
        i, j = stack.pop()
        best, idx = -1.0, -1
        for k in range(i + 1, j):
            d = cross_track_km(vec[k], vec[i], vec[j])
            if d > best:
                best, idx = d, k
        if idx >= 0 and best > tol_km:
            keep[idx] = True
            stack.append((i, idx))
            stack.append((idx, j))
    return [p for p, k in zip(points, keep) if k]


def region(lat, lon):
    if lon < -30.0:
        return "AMERICAS_WEST" if lon < -100.0 or lon > 170.0 else "AMERICAS_EAST"
    if lon <= 60.0:
        return "EUROPE_AFRICA"
    if lon >= 90.0 and lat > -15.0:
        return "EAST_ASIA"
    return "SOUTH_ASIA_OCEANIA"


def classify(lines):
    regions = set()
    for line in lines:
        for lat, lon in line:
            regions.add(region(lat, lon))
    americas = regions & {"AMERICAS_WEST", "AMERICAS_EAST"}
    asia = regions & {"EAST_ASIA", "SOUTH_ASIA_OCEANIA"}
    if "AMERICAS_EAST" in regions and "EUROPE_AFRICA" in regions:
        return 1
    if americas and asia and "EUROPE_AFRICA" not in regions:
        return 2
    if "EUROPE_AFRICA" in regions and asia and not americas:
        return 3
    if regions == {"EAST_ASIA"} or regions == {"EAST_ASIA", "SOUTH_ASIA_OCEANIA"}:
        return 4
    return 0


def feature_lines(feature):
    geom = feature.get("geometry")
    if not geom:
        return []
    if geom["type"] == "LineString":
        parts = [geom["coordinates"]]
    elif geom["type"] == "MultiLineString":
        parts = geom["coordinates"]
    else:
        return []
    out = []
    for part in parts:
        line = [(float(c[1]), float(c[0])) for c in part]
        if len(line) >= 2:
            out.append(line)
    return out


def main(argv):
    if len(argv) not in (3, 4):
        print(__doc__, file=sys.stderr)
        return 2
    with open(argv[1], "r", encoding="utf-8") as f:
        cables = json.load(f)
    landings = []
    if len(argv) == 4:
        with open(argv[2], "r", encoding="utf-8") as f:
            for feat in json.load(f)["features"]:
                g = feat.get("geometry")
                if g and g["type"] == "Point":
                    landings.append((float(g["coordinates"][1]), float(g["coordinates"][0])))
    out_path = argv[-1]

    polylines = []  # (corridor, line)
    raw_points = 0
    for feat in cables["features"]:
        lines = feature_lines(feat)
        if not lines:
            continue
        corridor = classify(lines)
        for line in lines:
            raw_points += len(line)
            simplified = douglas_peucker(line, TOLERANCE_KM)
            polylines.append((corridor, simplified))
            if len(argv) == 3:
                landings.append(line[0])
                landings.append(line[-1])
    polylines.sort(key=lambda c: (c[0], c[1][0], c[1][-1], len(c[1])))
    landings = sorted(set((round(a, 5), round(b, 5)) for a, b in landings))

    header = []
    points = []
    for corridor, line in polylines:
        header.append((len(points), len(line), corridor))
        points.extend(line)
    with open(out_path, "wb") as out:
        out.write(b"FTCB")
        out.write(struct.pack("<5I", 1, len(header), len(points), len(landings), 0))
        for first, count, corridor in header:
            out.write(struct.pack("<2IB3x", first, count, corridor))
        for lat, lon in points:
            out.write(struct.pack("<2f", lat, lon))
        for lat, lon in landings:
            out.write(struct.pack("<2f", lat, lon))
    per = [0] * len(CORRIDORS)
    for c, _ in polylines:
        per[c] += 1
    print("%d polylines, %d -> %d points (DP %.0f km), %d landing points; %s" % (
        len(polylines), raw_points, len(points), TOLERANCE_KM, len(landings),
        ", ".join("%s %d" % (CORRIDORS[i], per[i]) for i in range(len(CORRIDORS)))))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
