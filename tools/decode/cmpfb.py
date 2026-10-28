"""Compare a dumped render target with a glReadPixels capture.

    python3 cmpfb.py <dump.bin> <ref.rgba> [width height] [--png out.png]

The dump is BGRA8888, top row first, with a pitch rounded up to 32 pixels;
glReadPixels gives RGBA bottom row first. Prints the differing pixel count
and, with --png, writes the converted dump.
"""
import struct
import sys
import zlib


def convert(bb, w, h):
    pitch = len(bb) // h
    out = bytearray()
    for y in range(h - 1, -1, -1):
        r = bb[y * pitch:y * pitch + w * 4]
        row = bytearray(len(r))
        row[0::4], row[1::4], row[2::4], row[3::4] = r[2::4], r[1::4], r[0::4], r[3::4]
        out += row
    return bytes(out)


def png(path, img, w, h):
    raw = b"".join(b"\0" + img[y * w * 4:(y + 1) * w * 4] for y in range(h - 1, -1, -1))
    ch = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                ch(b"IDAT", zlib.compress(raw, 9)) + ch(b"IEND", b""))


def main(a):
    out = None
    if "--png" in a:
        i = a.index("--png")
        out = a[i + 1]
        del a[i:i + 2]
    w, h = (int(a[2]), int(a[3])) if len(a) > 3 else (720, 480)
    img = convert(open(a[0], "rb").read(), w, h)
    ref = open(a[1], "rb").read()
    bad = [i // 4 for i in range(0, w * h * 4, 4) if img[i:i + 4] != ref[i:i + 4]]
    print(f"cmpfb: {len(bad)} of {w * h} pixels differ")
    for p in bad[:8]:
        x, y = p % w, p // w
        print(f"  ({x},{y}) got {img[4 * p:4 * p + 4].hex()} want {ref[4 * p:4 * p + 4].hex()}")
    if out:
        png(out, img, w, h)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
