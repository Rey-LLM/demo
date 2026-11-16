#!/usr/bin/env python3
"""Convert a NASA Black Marble night-lights image into assets/citylights.bin.

Usage:
    python3 -I tools/build_citylights.py BLACK_MARBLE.(tif|png|jpg) assets/citylights.bin

Input: an equirectangular (plate carree) Black Marble image covering
-180..180 longitude and 90..-90 latitude, e.g. NASA Earth Observatory's
"Black Marble 2016 color maps" (public domain, NASA). Colour input is
reduced to luminance. This script does not download anything.

Processing (deterministic):
  1. luminance, linear in [0, 1] (the source is treated as gamma 2.2);
  2. box-filtered to 2048 x 1024;
  3. log tone: v = log1p(k x) / log1p(k), k = 64, so dim towns stay visible
     next to megacities without clipping;
  4. a full mip chain down to 4 px rows (box filter of the toned values);
  5. BC4 (GL_COMPRESSED_RED_RGTC1) per level, min/max endpoints.

Output layout (little endian), read by src/model/citylights.cpp:
    char[4] "FTNL", u32 version 1, u32 width, u32 height, u32 levels,
    BC4 blocks of every level (row-major, level 0 first)

Needs numpy and Pillow (build machine only; nothing of them ships).
"""

import struct
import sys

import numpy as np
from PIL import Image

WIDTH, HEIGHT = 2048, 1024
TONE_K = 64.0


def load_luminance(path):
    Image.MAX_IMAGE_PIXELS = None  # Black Marble mosaics are large
    img = Image.open(path)
    img = img.convert("RGB") if img.mode not in ("L", "I;16", "I") else img
    a = np.asarray(img).astype(np.float64)
    if a.ndim == 3:
        a = a / 255.0
        lin = np.power(a, 2.2)
        y = 0.2126 * lin[..., 0] + 0.7152 * lin[..., 1] + 0.0722 * lin[..., 2]
    else:
        y = np.power(a / a.max() if a.max() > 0 else a, 2.2)
    return y


def box_resize(a, w, h):
    # Pillow's BOX filter on a 32-bit float image is an exact area average.
    img = Image.fromarray(a.astype(np.float32), mode="F")
    return np.asarray(img.resize((w, h), Image.Resampling.BOX)).astype(np.float64)


def bc4_encode(level):
    h, w = level.shape
    q = np.clip(np.rint(level * 255.0), 0, 255).astype(np.int32)
    blocks = q.reshape(h // 4, 4, w // 4, 4).transpose(0, 2, 1, 3).reshape(-1, 16)
    r0 = blocks.max(axis=1)
    r1 = blocks.min(axis=1)
    i = np.arange(8)
    # 8-value mode palette (r0 > r1); flat blocks (r0 == r1) decode exactly with index 0.
    pal = np.empty((blocks.shape[0], 8), dtype=np.int32)
    pal[:, 0] = r0
    pal[:, 1] = r1
    for k in range(1, 7):
        pal[:, k + 1] = ((7 - k) * r0 + k * r1 + 3) // 7
    err = np.abs(blocks[:, :, None] - pal[:, None, :])
    idx = err.argmin(axis=2).astype(np.uint64)
    idx[r0 == r1] = 0
    bits = np.zeros(blocks.shape[0], dtype=np.uint64)
    for p in range(16):
        bits |= idx[:, p] << np.uint64(3 * p)
    out = np.zeros((blocks.shape[0], 8), dtype=np.uint8)
    out[:, 0] = r0
    out[:, 1] = r1
    for b in range(6):
        out[:, 2 + b] = ((bits >> np.uint64(8 * b)) & np.uint64(0xFF)).astype(np.uint8)
    return out.tobytes()


def main(argv):
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    y = load_luminance(argv[1])
    base = box_resize(y, WIDTH, HEIGHT)
    toned = np.log1p(TONE_K * base) / np.log1p(TONE_K)
    levels = []
    cur = toned
    while True:
        levels.append(cur)
        h, w = cur.shape
        if min(h, w) // 2 < 4:
            break
        cur = cur.reshape(h // 2, 2, w // 2, 2).mean(axis=(1, 3))
    with open(argv[2], "wb") as out:
        out.write(b"FTNL")
        out.write(struct.pack("<4I", 1, WIDTH, HEIGHT, len(levels)))
        for lv in levels:
            out.write(bc4_encode(lv))
    lit = float((toned > 0.25).mean()) * 100.0
    print("%dx%d, %d mip levels, %.2f %% of texels above 0.25 after log tone" % (WIDTH, HEIGHT, len(levels), lit))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
