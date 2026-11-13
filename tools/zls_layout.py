#!/usr/bin/env python3
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
"""Read the ZLS buffers mesa/tests/sgx_zls_layout writes under SGX_DEBUG=zlsdump
(/tmp/sgx-zls-<n>-<w>x<h>-<format>-s<samples>.bin) and say where each pixel's
depth and stencil lie (docs/TEX.md section 7).

    python3 -I tools/zls_layout.py DUMP...

The test's depth plane is 0.05 + 0.9 * (Y - 0.5 + X / w) / h at window
position (X, Y), so each stored float gives back the position it was computed
at (the row exactly, at a pixel centre); its
stencil blocks are bx << 4 | by for 8x8 blocks. For every dump this prints
the first stored entries as positions, then how well some layouts fit:
TEX.md section 7's order (32x16 tiles, twiddled inside; at 4x one surface of
samples twice as wide and high), row-major over the padded width (rows
bottom-up and top-down), and 32x16 tiles row-major inside. A 1x depth fit is
exact: each word must decode to its pixel. At 4x a sample's offset in the
pixel shifts the plane, so the TEX.md fit checks that the word lies within
its pixel's range of the plane (its row exactly, its column to the range),
and the stencil fit checks the 8x8 block of every sample.
"""
import math
import os
import re
import struct
import sys

NAME = re.compile(r"sgx-zls-(\d+)-(\d+)x(\d+)-([A-Za-z0-9_]+)-s(\d+)\.bin$")


def align(v, a):
    return (v + a - 1) // a * a


def decode(d, w, h):
    """(X, Y) for a stored depth, or None for the clear (1.0) or garbage"""
    if not (0.03 < d < 0.98):
        return None
    v = (d - 0.05) / 0.9 * h
    y = math.floor(v)
    return ((v - y) * w, float(y))


def layouts(W, H, w, h):
    """candidate maps from entry index to pixel (x, y), GL's y up"""
    def rows_up(i):
        return i % W, i // W

    def rows_down(i):
        return i % W, h - 1 - i // W

    def tiles(i, flip):
        t, o = divmod(i, 32 * 16)
        tx, ty = t % (W // 32), t // (W // 32)
        x, y = tx * 32 + o % 32, ty * 16 + o // 32
        return x, (h - 1 - y) if flip else y

    return {"row-major, rows up": rows_up, "row-major, rows down": rows_down,
            "32x16 tiles, rows up": lambda i: tiles(i, False),
            "32x16 tiles, rows down": lambda i: tiles(i, True)}


def tex_index(w, h, S, x, y, s):
    """docs/TEX.md section 7: the word of sample s (sx = s & 1, sy = s >> 1)
    of pixel (x, y), rows top first"""
    X, Y, across = x, y, align(w, 32) // 32
    if S > 1:
        X, Y, across = 2 * x + (s & 1), 2 * y + (s >> 1), across * 2
    t = ((Y >> 4) * across + (X >> 5)) * 512
    ix, iy = X & 31, Y & 15
    if S > 1:
        xb = [ix >> 1 & 1, ix >> 2 & 1, ix >> 3 & 1, ix & 1]
    else:
        xb = [ix & 1, ix >> 1 & 1, ix >> 2 & 1, ix >> 3 & 1]
    return (t | (iy & 1) | xb[0] << 1 | (iy >> 1 & 1) << 2 | xb[1] << 3 |
            (iy >> 2 & 1) << 4 | xb[2] << 5 | (iy >> 3 & 1) << 6 | xb[3] << 7 |
            (ix >> 4 & 1) << 8)


def plane(x, y, w, h):
    """the test's depth at window position (x, y)"""
    return 0.05 + 0.9 * (y - 0.5 + x / w) / h


def tex_fit(depth, st, w, h, S):
    """(depth ok, depth total, stencil ok, stencil total) under TEX.md's order,
    for the drawn pixels"""
    dok = dtot = sok = stot = 0
    for y in range(h):
        for x in range(w):
            lo, hi = plane(x, y, w, h), plane(x + 1, y, w, h)
            for s in range(S):
                i = tex_index(w, h, S, x, y, s)
                d = depth[i]
                dtot += 1
                if S == 1:
                    p = decode(d, w, h)
                    dok += p is not None and math.floor(p[0]) == x and p[1] == y
                else:
                    # a sample at (X, Y) inside the pixel: the plane between
                    # its (x, y) corner and its (x + 1, y + 1) corner
                    dok += lo - 1e-6 <= d <= hi + 0.9 / h + 1e-6
                if st is not None:
                    stot += 1
                    sok += st[i] == ((x // 8) << 4 | (y // 8))
    return dok, dtot, sok, stot


def report(path):
    m = NAME.search(path)
    if not m:
        print(f"{path}: not a zlsdump name")
        return
    w, h, fmt, s = int(m[2]), int(m[3]), m[4].upper(), int(m[5])
    data = open(path, "rb").read()
    W, H = align(w, 32), align(h, 16)
    n = W * H * s
    print(f"== {os.path.basename(path)}: {w}x{h} {fmt} {s}x, padded {W}x{H}, "
          f"{len(data)} bytes ({n * 5} for depth + stencil)")
    if len(data) < n * 4:
        print("   too short for the depths")
        return
    depth = struct.unpack_from(f"<{n}f", data)
    pos = [decode(d, w, h) for d in depth]
    drawn = sum(p is not None for p in pos)
    print(f"   {drawn} of {n} depths decode to a position "
          f"({w * h * s} drawn samples expected)")
    for i in list(range(0, 8)) + list(range(30, 34)) + [W - 1, W, W + 1, 512, 513, 544]:
        if i < n:
            p = pos[i]
            print(f"   [{i:6d}] {depth[i]:.7f} " +
                  (f"x {p[0]:8.3f} y {p[1]:5.0f}" if p else "(clear or pad)"))
    st = data[n * 4:n * 5] if fmt.endswith("S8_UINT") and len(data) >= n * 5 else None
    dok, dtot, sok, stot = tex_fit(depth, st, w, h, s)
    print(f"   fit TEX.md order, depth   {dok}/{dtot}")
    if st is not None:
        print(f"   fit TEX.md order, stencil {sok}/{stot}")
    if s == 1:
        for name, f in layouts(W, H, w, h).items():
            ok = tot = 0
            for i, p in enumerate(pos):
                if p is None:
                    continue
                tot += 1
                x, y = f(i)
                ok += math.floor(p[0]) == x and p[1] == y
            print(f"   fit {name:24s} {ok}/{tot}")
    else:
        # the positions within a pixel the first entries hold
        frac = sorted({(round(p[0] % 1, 3), ) for p in pos[:4096] if p})
        print(f"   x offsets in the pixel among the first 4096: {frac[:16]}")
    if fmt.endswith("S8_UINT") and len(data) >= n * 5:
        st = data[n * 4:n * 5]
        print("   stencil, first 40: " + " ".join(f"{b:02x}" for b in st[:40]))
        if s == 1:
            for name, f in layouts(W, H, w, h).items():
                ok = tot = 0
                for i, b in enumerate(st):
                    x, y = f(i)
                    if 0 <= x < w and 0 <= y < h:
                        tot += 1
                        ok += b == ((x // 8) << 4 | (y // 8))
                print(f"   stencil fit {name:24s} {ok}/{tot}")


for p in sys.argv[1:]:
    report(p)
