"""Hexdump device memory at a kick: hexva.py <log> <kick#> <va> [len]  (words, address-tagged)."""
import struct
import sys

import cmdta
import memmap
import pvrlog


def main(path, kick, va, n):
    log = pvrlog.load(path)
    mm = memmap.build(log)
    snaps = [s for s in log.of(pvrlog.Snap) if s.reason == "kick" and s.kick == kick]
    mem = memmap.Memory(mm, snaps)
    d = mem.read(va, n)
    if d is None:
        sys.exit(f"{va:#x}: not in any captured buffer")
    last = None
    for o in range(0, len(d) - 3, 16):
        row = d[o:o + 16]
        if row == last and row == bytes(16):
            continue
        last = row
        ws = struct.unpack_from(f"<{len(row) // 4}I", row)
        tags = " ".join(t for t in (cmdta.tag(mm, mem, w) for w in ws) if t)
        print(f"{va + o:#010x}: " + " ".join(f"{w:08x}" for w in ws) + (f"   {tags}" if tags else ""))


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]), int(sys.argv[3], 0), int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x100)
