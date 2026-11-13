#!/usr/bin/env python3
"""Verify a frame's PDS data layout against a core's DS0/DS1 interleave, offline.

For every PDS program a frame references through the VDM control stream (state
programs and vertex fetch), this interprets the program under a chosen interleave
(pds.py's interpreter, with the matching DS0/DS1 layout) and checks that its DOUTU
launches at a real USSE code address and every DMA it actually takes reads a
non-zero source. A program laid out for the wrong core reads its launch/DMA words
from zero or wrong positions -- the task launches at code base 0, or a DMA fetches
address 0 -- the SGX530 gate-2 failure (docs/porting/cores/sgx530-vendor-setup.md
section 2). Running the interpreter (not a static scan) means the whole data
segment is exercised -- the DMA sources ds0[0..3] as well as the launch ds0[4..5]
-- and the conditional DMAs (the state program's `tst; bra; doutd`) are honored,
so a skipped optional DMA is never mistaken for a broken one.

    --interleave 2word   the SGX545 layout: DS0/DS1 interleaved two words at a
                         time (docs/PDS.md "Program layout").
    --interleave 8word   the SGX530 layout: eight words at a time. The 530 lacks
                         SGX_FEATURE_PDS_DATA_INTERLEAVE_2DWORDS, so sgxgen must
                         pack its data segments this way for the 530.

A frame built for a core passes under that core's interleave (every launch
selects a real vertex/emit task, 0x3c0-class) and fails under the other. Run it
on a frame-fake trace after the sgxgen 8-word-packing change:

    python3 tools/ukernel/pds_interleave_check.py frame64.trace --interleave 8word

must be N/N PASS, with the printed launch addresses the same 0x3c0-class tasks the
2word reading of a 545 frame gives. That is the offline form of the ds0[4] ->
0x0000070a check -- an SGX530 PDS-layout go/no-go that needs no board slot. Exit
status is 0 on PASS, 1 on FAIL.

Scope: this exercises the whole data segment (launch + taken DMAs) under the
interleave; the printed launch address is the definitive eyeball -- a correct
repack resolves every VDM-launched program to its 0x3c0-class task, not the pixel
heap (0x37) or base 0. It does not check USSE-side execution (shader/task
semantics) or structures outside the PDS data segment.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fromtrace  # noqa: E402
import pds  # noqa: E402
from pds import use_addr  # noqa: E402


def ds_layout_2word(words, data_words):
    """pds.py's native layout: DS0/DS1 interleaved two words at a time."""
    ds = {0: {}, 1: {}}
    for m in range(min(data_words, len(words))):
        row, col = m >> 2, m & 1
        (ds[0] if (m & 3) < 2 else ds[1])[2 * row + col] = words[m]
    return ds


def ds_layout_8word(words, data_words):
    """the SGX530 layout: DS0/DS1 interleaved eight words at a time (64-byte
    blocks), per docs/porting/cores/sgx530-vendor-setup.md section 2."""
    ds = {0: {}, 1: {}}
    for m in range(min(data_words, len(words))):
        block, within = m >> 4, m & 15
        if within < 8:
            ds[0][block * 8 + within] = words[m]
        else:
            ds[1][block * 8 + (within - 8)] = words[m]
    return ds


LAYOUT = {"2word": ds_layout_2word, "8word": ds_layout_8word}


def run_program(seg, rows, mode):
    """interpret one PDS program under `mode`'s interleave; return
    (doutu_word0, [executed DOUTD source addresses]) or raise."""
    pds.ds_layout = LAYOUT[mode]                 # PDS.__init__ reads this global

    class _NoMem:
        def rd(self, a, pc=None):
            return 0

    p = pds.PDS(struct.pack(f"<{len(seg)}I", *seg), rows * 4, _NoMem()).run()
    dma = [e[1] for e in p.log if e[0] == "doutd"]   # ("doutd", src, n, bank, reg)
    return (p.doutu.w0 if p.doutu else None), dma


def programs(stream):
    """(kind, addr, rows) for each state program / vertex PDS in a VDM stream."""
    out, i = [], 0
    while i < len(stream):
        x = stream[i]
        if (x >> 28) in (4, 6) and x:
            out.append(("state", (x & 0x0FFFFFFF) << 4, stream[i + 1] >> 26))
            i += 2
        elif x & 0x80000000 and x & 0x00C00000:
            dw = stream[i:i + 6]
            if len(dw) >= 6:
                out.append(("vtx", (dw[4] & 0x0FFFFFFF) << 4, dw[5] & 0x7F))
            i += 6
        else:
            i += 1
    return out


def check(trace, mode):
    fails = total = 0
    for k in (k for k in fromtrace.kicks(trace) if k.kind == "kick"):
        cmd = k.ccb_va + k.ccb_off - 0x58
        vdm = k.memory.words(cmd + 0x250, 1)[0]
        stream = k.memory.words(vdm, 64) or []
        print(f"\nkick {k.n}: VDM stream @ {vdm:#x}")
        seen = set()
        for kind, addr, rows in programs(stream):
            if not addr or addr in seen:
                continue
            seen.add(addr)
            seg = k.memory.words(addr, (rows + 8) * 4)
            if seg is None:
                print(f"  {kind:5s} @ {addr:#x}: not in trace")
                continue
            total += 1
            try:
                w0, dma = run_program(seg, rows, mode)
            except Exception as e:                   # won't even interpret under this interleave
                fails += 1
                print(f"  {kind:5s} @ {addr:#x} rows={rows}: interpret FAILED ({e})")
                continue
            launch = use_addr(w0) if w0 else None
            bad_dma = [a for a in dma if not a]       # a DMA that executed reads address 0
            ok = w0 and launch and not bad_dma
            fails += not ok
            dmas = " ".join(f"{a:#x}" for a in dma) or "none"
            print(f"  {kind:5s} @ {addr:#x} rows={rows}: "
                  f"launch {('code '+hex(launch)) if launch else 'BASE 0 (null)'}; "
                  f"DMAs taken: {dmas}  [{'OK ' if ok else 'FAIL'}]")
    ok = fails == 0
    print(f"\n{'=' * 60}\n{mode}: {total - fails}/{total} programs interpret with a real "
          f"launch and no null DMA; {fails} broken.  {'PASS' if ok else 'FAIL'}")
    return ok


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("trace", help="a frame-fake or DDK pvrtrace")
    ap.add_argument("--interleave", choices=["2word", "8word"], required=True,
                    help="2word = SGX545 layout, 8word = SGX530 layout")
    a = ap.parse_args()
    sys.exit(0 if check(a.trace, a.interleave) else 1)
