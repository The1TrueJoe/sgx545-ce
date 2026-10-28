"""Read a PROBE_TEXPAT texture back out of a capture: which texel sits at each offset.

    python3 rawmap.py CAPTURE.bin --fmt rgba8888 --size W H [--levels n] [--cube] [--show N]

The texture buffer is the texture-heap (0xf0000000) allocation with the
largest size, read at the kick-1 snapshot. Each texel of the pattern names
itself (re/probe/probe.c, PROBE_TEXPAT):
  rgba8888 / rgb888: bytes x, y, level | face<<4, 0x80 | x>>8 | (y>>8)<<2
  16-bit formats: y*w + x;  l8 / a8: (y*w + x) & 0xff
Prints the decoded (level, face, x, y) at the first offsets and, per level and
face, the lowest and highest byte offset, and returns the map for check.py.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "decode"))
import memmap  # noqa: E402
import pvrlog  # noqa: E402

BPP = {"rgba8888": 4, "rgb888": 4, "rgb565": 2, "rgba4444": 2, "rgba5551": 2, "la88": 2, "l8": 1, "a8": 1}
TEX_HEAP = 0xF0000000


def texture(path, kick=1):
    """(device address, bytes) of the largest texture-heap buffer at a kick snapshot."""
    log = pvrlog.load(path)
    mm = memmap.build(log)
    bufs = [b for b in mm.bufs if b.origin == "alloc" and b.devva >= TEX_HEAP and b.cpu is not None]
    b = max(bufs, key=lambda b: (b.size, b.seq))	# the latest of equal sizes (1x1 vs the 4-byte driver words)
    mem = memmap.Memory(mm, [s for s in log.of(pvrlog.Snap) if s.reason == "kick" and s.kick == kick])
    return b.devva, mem.read(b.devva, b.size), log, mm


def decode(data, fmt, w, h):
    """offset -> (level, face, x, y) for an 8888 pattern; offset -> linear index for the others."""
    bpp = BPP[fmt]
    out = {}
    for o in range(0, len(data) - bpp + 1, bpp):
        t = data[o:o + bpp]
        if fmt in ("rgba8888", "rgb888"):
            if fmt == "rgb888":    # expanded to 4 bytes, alpha 0xff
                t = bytes(t[:3]) + b"\x80"
            if not t[3] & 0x80:
                continue
            x = t[0] | (t[3] & 3) << 8
            y = t[1] | (t[3] >> 2 & 3) << 8
            out[o] = (t[2] & 15, t[2] >> 4, x, y)
        elif bpp == 2:
            out[o] = t[0] | t[1] << 8
        else:
            out[o] = t[0]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cap")
    ap.add_argument("--fmt", default="rgba8888")
    ap.add_argument("--size", nargs=2, type=int, required=True)
    ap.add_argument("--show", type=int, default=64)
    a = ap.parse_args()
    va, data, _, _ = texture(a.cap)
    w, h = a.size
    m = decode(data, a.fmt, w, h)
    print(f"texture buffer {va:#x}, {len(data):#x} bytes, {len(m)} decoded texels")
    for o in sorted(m)[:a.show]:
        print(f"  +{o:#06x}: {m[o]}")
    if a.fmt in ("rgba8888", "rgb888"):
        by = {}
        for o, (lv, f, x, y) in m.items():
            by.setdefault((f, lv), []).append(o)
        for k in sorted(by):
            print(f"face {k[0]} level {k[1]}: {len(by[k])} texels at +{min(by[k]):#x}..+{max(by[k]):#x}")


if __name__ == "__main__":
    main()
