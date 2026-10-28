"""Parse a USP binary (the DDK compiler's pre-patch shader container).

Layout learned so far (DDK 1.7, version 0x23):
  +0x00  magic 0x55535020 ("USP ")
  +0x04  version
  +0x08  total size in bytes
  +0x10  0 fragment, 1 vertex
  +0x7c  high half: constant-table entries, low half: instruction count
  +0x90  constant table, 8 bytes per entry, (component index, index << 16 | 1)
         in the simple cases: one entry per uniform component the shader reads
  then   instructions, 64-bit each as (low word, high word); a pair of
         0xffffffff words follows the last one
"""
import glob
import os
import struct
import sys

MAGIC = 0x55535020


def load(path):
    d = open(path, "rb").read()
    magic, ver, size = struct.unpack_from("<III", d, 0)
    if magic != MAGIC:
        raise ValueError(f"{path}: not a USP binary")
    d = d[:size]
    w = struct.unpack_from("<I", d, 0x7C)[0]
    nconst, count = w >> 16, w & 0xFFFF
    consts = [struct.unpack_from("<II", d, 0x90 + 8 * i) for i in range(nconst)]
    base = 0x90 + 8 * nconst
    insts = [struct.unpack_from("<II", d, base + 8 * i) for i in range(count)]
    return {
        "name": os.path.basename(path).split("-", 1)[-1].rsplit(".", 1)[0],
        "ver": ver,
        "size": size,
        "kind": "vert" if struct.unpack_from("<I", d, 0x10)[0] == 1 else "frag",
        "consts": consts,
        "insts": insts,  # (lo, hi)
        "raw": d,
    }


def corpus(dirname):
    return [load(p) for p in sorted(glob.glob(os.path.join(dirname, "*.usp")))]


if __name__ == "__main__":
    for s in corpus(sys.argv[1]):
        print(f"{s['name']} ({s['kind']}, {len(s['insts'])} insts)")
        for lo, hi in s["insts"]:
            print(f"    {hi:08x} {lo:08x}   {hi:032b} {lo:032b}")
