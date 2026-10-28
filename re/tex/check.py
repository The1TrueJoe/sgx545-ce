"""Check tex.py's layout against every texel of PROBE_TEXPAT captures.

    python3 check.py CAPTURE.bin --size W H [--fmt F] [--levels N] [--cube]

For each texel the probe uploaded, predicts its byte offset with tex.py and
compares with where the pattern actually sits in the capture's texture
buffer. Prints the number of texels that match, and the first mismatches.
"""
import argparse
import sys

import rawmap
import tex


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cap")
    ap.add_argument("--fmt", default="rgba8888")
    ap.add_argument("--size", nargs=2, type=int, required=True)
    ap.add_argument("--levels", type=int, default=1)
    ap.add_argument("--cube", action="store_true")
    ap.add_argument("--face-stride", type=lambda x: int(x, 0), default=None)
    a = ap.parse_args()
    va, data, _, _ = rawmap.texture(a.cap)
    w, h = a.size
    bpp = tex.BPP[a.fmt]
    got = rawmap.decode(data, a.fmt, w, h)
    chain = tex.level_offset(w, h, bpp, a.levels)
    fstride = a.face_stride if a.face_stride is not None else chain
    ok = bad = 0
    shown = 0
    for f in range(6 if a.cube else 1):
        for lv in range(a.levels):
            lw, lh = tex.level_size(w, h, lv)
            for y in range(lh):
                for x in range(lw):
                    o = f * fstride + tex.texel_offset(x, y, w, h, bpp, lv)
                    if a.fmt in ("rgba8888", "rgb888"):
                        want = (lv, f, x, y)
                    elif bpp == 2:
                        want = (y * lw + x) & 0xFFFF
                    else:
                        want = (y * lw + x) & 0xFF
                    if got.get(o) == want:
                        ok += 1
                    else:
                        bad += 1
                        if shown < 8:
                            shown += 1
                            print(f"  face {f} level {lv} ({x},{y}): predicted +{o:#x}, there {got.get(o)}")
    print(f"{a.cap}: buffer {va:#x} {len(data):#x} bytes, chain {chain:#x}: {ok} texels match, {bad} differ")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
