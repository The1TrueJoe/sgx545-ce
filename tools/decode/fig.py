"""Turn EA3 frames into PNGs for the build log.

    python3 fig.py png  <frame> <out.png> [--size W H] [--scale N]
    python3 fig.py grid <out.png> <cols> <frame>... [--size W H] [--scale N] [--gap PX]

A frame is either a glReadPixels capture (.rgba: RGBA, bottom row first, W*H*4
bytes) or a render-target dump (anything else: BGRA, top row first, pitch rounded
up, as cmpfb.py reads it). The size defaults to 720x480. --scale N keeps every
Nth pixel, for thumbnails; grid tiles frames left to right on a dark background.
Alpha is forced to opaque, so memory the GPU never wrote shows as black.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cmpfb import convert, png  # noqa: E402


def load(path, w, h):
    d = open(path, "rb").read()
    if path.endswith(".rgba"):
        if len(d) < w * h * 4:
            raise ValueError(f"{path}: {len(d)} bytes is short of {w}x{h}")
        img = bytearray(d[:w * h * 4])
    else:
        img = bytearray(convert(d, w, h))
    # the GPU leaves alpha 0 in places (untouched memory, a 1-c shader); a viewer
    # would show those pixels as transparent, so figures are always opaque
    img[3::4] = b"\xff" * (w * h)
    return bytes(img)


def scale(img, w, h, n):
    if n == 1:
        return img, w, h
    sw, sh = w // n, h // n
    out = bytearray()
    for y in range(sh):
        row = img[(y * n) * w * 4:(y * n + 1) * w * 4]
        for x in range(sw):
            out += row[x * n * 4:x * n * 4 + 4]
    return bytes(out), sw, sh


def grid(frames, cols, gap, bg=b"\x16\x18\x1c\xff"):
    w, h = frames[0][1], frames[0][2]
    rows = (len(frames) + cols - 1) // cols
    gw, gh = cols * w + (cols + 1) * gap, rows * h + (rows + 1) * gap
    out = bytearray(bg * (gw * gh))
    for i, (img, fw, fh) in enumerate(frames):
        # images are bottom row first, like the png writer expects
        ox = gap + (i % cols) * (w + gap)
        oy = gh - gap - h - (i // cols) * (h + gap)
        for y in range(fh):
            o = ((oy + y) * gw + ox) * 4
            out[o:o + fw * 4] = img[y * fw * 4:(y + 1) * fw * 4]
    return bytes(out), gw, gh


def opt(a, name, n, default):
    if name in a:
        i = a.index(name)
        v = [int(x) for x in a[i + 1:i + 1 + n]]
        del a[i:i + 1 + n]
        return v if n > 1 else v[0]
    return default


def main(a):
    w, h = opt(a, "--size", 2, (720, 480))
    n = opt(a, "--scale", 1, 1)
    gap = opt(a, "--gap", 1, 8)
    if a[0] == "png":
        img, sw, sh = scale(load(a[1], w, h), w, h, n)
        png(a[2], img, sw, sh)
    elif a[0] == "grid":
        frames = [scale(load(p, w, h), w, h, n) for p in a[3:]]
        img, gw, gh = grid(frames, int(a[2]), gap)
        png(a[1], img, gw, gh)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
