"""Diff the GPU memory two captures handed the GPU at the same kick.

  diff.py A.bin B.bin [kick] [--all]

Buffers are matched by device address (allocation is deterministic for the
same program, so the same buffer lands at the same address). Prints every
32-bit word that differs, grouped per buffer, with A and B values side by
side and float readings when they look like floats.
"""
import struct
import sys

import cmdta
import memmap
import pvrlog


def mem_at(path, kick):
    log = pvrlog.load(path)
    mm = memmap.build(log)
    snaps = [s for s in log.of(pvrlog.Snap) if s.reason == "kick" and s.kick == kick]
    return log, mm, memmap.Memory(mm, snaps)


def fl(v):
    f = struct.unpack("<f", struct.pack("<I", v))[0]
    if v and 1e-6 < abs(f) < 1e6:
        return f"{f:.6g}"
    return ""


def main(a, b, kick=1, show_all=False, limit=64):
    la, ma, A = mem_at(a, kick)
    lb, mb, B = mem_at(b, kick)
    bregs = {va: (d, buf) for va, d, buf in B.regions}
    for va, da, buf in A.regions:
        if va not in bregs:
            print(f"{va:#010x} only in A ({buf.size:#x})")
            continue
        db, _ = bregs.pop(va)
        if da == db:
            continue
        n = min(len(da), len(db)) // 4
        wa = struct.unpack_from(f"<{n}I", da)
        wb = struct.unpack_from(f"<{n}I", db)
        diffs = [i for i in range(n) if wa[i] != wb[i]]
        print(f"== {va:#010x} {cmdta.heapname(ma, va)} size {buf.size:#x}: {len(diffs)} words differ")
        for i in diffs[: (None if show_all else limit)]:
            print(f"   +{i * 4:#06x}: {wa[i]:#010x} {fl(wa[i]):>10} | {wb[i]:#010x} {fl(wb[i]):>10}")
        if not show_all and len(diffs) > limit:
            print(f"   ... {len(diffs) - limit} more")
    for va, (d, buf) in bregs.items():
        print(f"{va:#010x} only in B ({buf.size:#x})")


if __name__ == "__main__":
    args = [x for x in sys.argv[1:] if not x.startswith("--")]
    main(args[0], args[1], int(args[2]) if len(args) > 2 else 1, "--all" in sys.argv)
