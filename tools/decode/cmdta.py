"""Dump the TA command (SGXMKIF_CMDTA) each SGX_DOKICK points at, word by word,
tagging every word that is a device address (with its heap/buffer)."""
import struct
import sys

import layout
import memmap
import pvrlog

HEAPS = {0: "general", 1: "tadata", 2: "kcode", 3: "kdata", 4: "pixshader", 5: "vtxshader",
         6: "pdspixel", 7: "pdsvertex", 8: "sync", 9: "shared3dparams", 10: "3dparams",
         11: "genmapping", 12: "2d", 13: "cgbuffer", 14: "texture"}


def heapname(mm, va):
    for h in mm.heaps.values():
        if h.size and h.base <= va < h.base + h.size:
            return HEAPS.get(h.id & 0xFF, f"heap{h.id & 0xff}")
    return None


def tag(mm, mem, va):
    hn = heapname(mm, va)
    if hn is None:
        return ""
    b = mm.find(va)
    if b is None:
        return ""
    return f"<{hn} buf@{b.devva:#x}+{va - b.devva:#x} sz={b.size:#x}>"


def kicks(log):
    return [r for r in log.of(pvrlog.Bridge) if r.name == "SGX_DOKICK"]


def cmd_bytes(log, mm, k, kickno, length):
    arch = mm.arch
    base = layout.layout(arch)["PVRSRV_BRIDGE_IN_DOKICK_TAG"].path("sCCBKick")[0]
    hccb = layout.get(arch, "_SGX_CCB_KICK_", k.inp, "hCCBKernelMemInfo", base)
    off = layout.get(arch, "_SGX_CCB_KICK_", k.inp, "ui32CCBOffset", base)
    ccb = next(b for b in mm.bufs if b.kmeminfo == hccb)
    snaps = [s for s in log.of(pvrlog.Snap) if s.reason == "kick" and s.kick == kickno]
    mem = memmap.Memory(mm, snaps)
    return mem, ccb, off, mem.read(ccb.devva + off, length)


def main(path, length=0x2c0):
    log = pvrlog.load(path)
    mm = memmap.build(log)
    shared = layout.layout(mm.arch)["_SGXMKIF_CMDTA_SHARED_"].size
    for i, k in enumerate(kicks(log), 1):
        mem, ccb, off, d = cmd_bytes(log, mm, k, i, length)
        print(f"=== kick {i} (seq {k.seq}) CCB buf {ccb.devva:#x} size {ccb.size:#x} offset {off:#x}; "
              f"CMDTA_SHARED is {shared:#x} bytes")
        for o in range(0, len(d) - 3, 4):
            v = struct.unpack_from("<I", d, o)[0]
            mark = "|" if o == shared else " "
            print(f" {mark}{o:#05x}: {v:#010x} {tag(mm, mem, v)}")


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x2c0)
