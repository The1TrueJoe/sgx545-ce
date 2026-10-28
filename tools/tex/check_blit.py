"""Predict every byte blitest writes and compare with its dumps.

    python3 check_blit.py <dir with blitest's *.bin>

Every case has an exact prediction, measured on the EA3: up (tex.py's twiddle
of the source), down (the source again), mipn (texel 2x+1, 2y+1, as the DDK's
glGenerateMipmap), mipl (LINEAR at half size: a two-step rounded average),
up2x (pixel replication), npot (the source rows), to565 (each channel
truncated, no dither).
"""
import os
import struct
import sys

import tex


def pattern(x, y):
    return bytes([(x * 7 + y * 3) & 255, (x * x + 5 * y) & 255, (x * y) & 255, (255 - 2 * x - y) & 255])


def lin_image(w, h, pitch=None):
    pitch = pitch or w
    out = bytearray(pitch * h * 4)
    for y in range(h):
        for x in range(w):
            out[4 * (y * pitch + x):4 * (y * pitch + x) + 4] = pattern(x, y)
    return bytes(out)


def twiddled_texel(data, x, y, w, h):
    o = tex.twiddle(x, y, w, h) * 4
    return data[o:o + 4]


def count(a, b, n, size):
    return sum(a[i:i + size] != b[i:i + size] for i in range(0, n * size, size))


def main():
    d = sys.argv[1]
    rd = lambda n: open(os.path.join(d, n + ".bin"), "rb").read()
    src = lin_image(64, 64)
    assert rd("src") == src, "source dump differs from the pattern"
    res = []
    up = rd("up")
    res.append(("up", count(up, tex.encode_level(src, 64, 64, 4), 4096, 4), 4096))
    res.append(("down", count(rd("down"), src, 4096, 4), 4096))
    mipn = rd("mipn")
    want = bytearray(32 * 32 * 4)
    for y in range(32):
        for x in range(32):
            o = tex.twiddle(x, y, 32, 32) * 4
            want[o:o + 4] = pattern(2 * x + 1, 2 * y + 1)
    res.append(("mipn (2x+1, 2y+1)", count(mipn, want, 1024, 4), 1024))
    mipl = rd("mipl")
    want = bytearray(32 * 32 * 4)
    for y in range(32):
        for x in range(32):
            a, b, c, e = (pattern(2 * x + i, 2 * y + j) for j in (0, 1) for i in (0, 1))
            o = tex.twiddle(x, y, 32, 32) * 4
            # LINEAR at half size: a two-step rounded average (pairs, then the pair averages)
            want[o:o + 4] = bytes((((a[k] + b[k] + 1) >> 1) + ((c[k] + e[k] + 1) >> 1) + 1) >> 1 for k in range(4))
    res.append(("mipl (two-step rounded average)", count(mipl, want, 1024, 4), 1024))
    big = rd("up2x")
    want = b"".join(pattern(x >> 1, y >> 1) for y in range(128) for x in range(128))
    res.append(("up2x (pixel replication)", count(big, want, 128 * 128, 4), 128 * 128))
    npot = rd("npot")
    res.append(("npot", sum(npot[4 * (y * 104 + x):4 * (y * 104 + x) + 4] != pattern(x, y)
                            for y in range(30) for x in range(100)), 3000))
    o565 = rd("to565")
    n = 0
    for y in range(64):
        for x in range(64):
            r, g, b, a = pattern(x, y)
            n += struct.unpack_from("<H", o565, 2 * (y * 64 + x))[0] != ((r >> 3) << 11 | (g >> 2) << 5 | b >> 3)
    res.append(("to565 (truncation)", n, 4096))
    bad = 0
    for name, diff, n in res:
        if n is None:
            print(f"  {name}")
        else:
            print(f"  {name}: {n - diff} of {n} match, {diff} differ")
            bad += diff
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
