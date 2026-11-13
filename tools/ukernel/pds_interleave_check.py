#!/usr/bin/env python3
"""Verify a frame's PDS data layout against a core's DS0/DS1 interleave, offline.

For every PDS program a frame references through the VDM control stream (state
programs and vertex fetch), this resolves the program's DOUTU launch word
(word0) under a chosen interleave and prints the USSE code address it selects.
A program laid out for the wrong core reads its launch source from a zero or
wrong word and launches at code base 0 (no task) -- the SGX530 gate-2 failure
(docs/porting/cores/sgx530-vendor-setup.md section 2).

    --interleave 2word   the SGX545 layout: DS0/DS1 interleaved two words at a
                         time (docs/PDS.md "Program layout").
    --interleave 8word   the SGX530 layout: eight words at a time. The 530 lacks
                         SGX_FEATURE_PDS_DATA_INTERLEAVE_2DWORDS, so sgxgen must
                         pack its data segments this way for the 530.

A frame built for a core passes under that core's interleave (every launch word
selects a real vertex/emit task, 0x3c0-class) and fails under the other. Run it
on a frame-fake trace after the sgxgen 8-word-packing change:

    python3 tools/ukernel/pds_interleave_check.py frame64.trace --interleave 8word

must report every launch word non-null (and the printed code addresses the same
0x3c0-class tasks the 2word reading of a 545 frame gives). That is the offline
form of the ds0[4] -> 0x0000070a check -- an SGX530 PDS-layout go/no-go that
needs no board slot. Exit status is 0 on PASS, 1 on FAIL.

Scope: this catches null / code-base-0 launches, the primary wrong-interleave
signature. The printed code address is the definitive eyeball: a correct repack
resolves every VDM-launched program to its 0x3c0-class task, not the pixel heap
(0x37) or base 0.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fromtrace  # noqa: E402
from pds import HALT, movs_sources, use_addr  # noqa: E402


def mem_off(bank, idx, mode):
    """memory-word offset of DS{bank} word `idx` under the interleave."""
    if mode == "2word":
        r, c = idx >> 1, idx & 1
        return r * 4 + (c if bank == 0 else 2 + c)
    return (idx >> 3) * 16 + (idx & 7) + (0 if bank == 0 else 8)  # 8word


def find_doutu(code):
    """the first MOVS (op 0/2) whose destination is DOUTU (5)."""
    for w in code:
        if w == HALT:
            break
        if (w >> 28) in (0, 2) and (w & 0x1F) == 5:
            return w
    return None


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
            seg = k.memory.words(addr, (rows + 4) * 4)
            if seg is None:
                print(f"  {kind:5s} @ {addr:#x}: not in trace")
                continue
            du = find_doutu(seg[rows * 4:])
            if du is None:
                print(f"  {kind:5s} @ {addr:#x} rows={rows}: no DOUTU")
                continue
            bank, idx = movs_sources(du)[0]          # launch word0 source
            off = mem_off(bank, idx, mode)
            w0 = seg[off] if off < len(seg) else 0
            total += 1
            ok = w0 != 0
            fails += not ok
            sel = f"-> code {use_addr(w0):#x}" if w0 else "-> code base 0 (null launch)"
            print(f"  {kind:5s} @ {addr:#x} rows={rows}: DOUTU w0 = ds{bank}[{idx}] "
                  f"= mem[{off}] = {w0:#010x} {sel}  [{'OK ' if ok else 'FAIL'}]")
    ok = fails == 0
    print(f"\n{'=' * 60}\n{mode}: {total - fails}/{total} launch words resolve to a "
          f"program; {fails} null.  {'PASS' if ok else 'FAIL'}")
    return ok


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("trace", help="a frame-fake or DDK pvrtrace")
    ap.add_argument("--interleave", choices=["2word", "8word"], required=True,
                    help="2word = SGX545 layout, 8word = SGX530 layout")
    a = ap.parse_args()
    sys.exit(0 if check(a.trace, a.interleave) else 1)
