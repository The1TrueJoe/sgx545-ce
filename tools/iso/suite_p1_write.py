# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""P1 open half, the safe pre-flight (UAPI.md 7.2 / G1, Joseph's D9). Run this first.

It confirms the rig renders on this box and that client memory works, re-confirms the P1 read
on the EA1, and establishes the discriminator for the write test: the host reads a register
before and after the frame, so after != before means the client's `str` reached the register.

USE_CODE_BASE_14 (0xa44) and _15 (0xa48) are unused code bases, so writing them changes nothing
a frame depends on: they are the non-fatal controls for "does any register write land". The last
test only reads BIF_BANK0 (0xc78) and BIF_DIR_LIST_BASE0 (0xc84) from the host and the shader, to
record the live directory base (needed for the destructive write test and doc gap G2). Nothing
here should wedge."""
from iso import T, out

CORE_ID = 0x1c
BASE14, BASE15 = 0xa44, 0xa48
BANK0, DIRBASE = 0xc78, 0xc84
R = ("R", 0, 0xf, 0x1000, 0x1000)       # READ|WRITE|CACHE_CONSISTENT|NO_SYNCOBJ
S14, S15 = 0x12340, 0x15150             # harmless code-base sentinels (never used by a frame)


def rd(off, slot):
    return [f"ldr r{slot}, #{off // 4} [drc=0]", "wdf [drc=0]", f"st.a.d sa0, #{slot}, r{slot}  [skipinv]"]


def wr(off, val):
    """write a sentinel, read it straight back; the host `reg` read is the real check"""
    return [f"limm r0, #{val:#x}  [skipinv]", f"str #{off // 4}, r0",
            f"ldr r1, #{off // 4} [drc=0]", "wdf [drc=0]", "st.a.d sa0, #0, r1  [skipinv]", out("r1")]


TESTS = [
    T("sanity_frame"),
    T("sanity_st", bufs=[R], fsa=[(0, "R")], dumps=[("R", 0, 0x9)], fs=[
        "limm r0, #0x11223344  [skipinv]", "limm r1, #0x55667788  [skipinv]",
        "st.a.d sa0, #0, r0  [skipinv]", "st.a.d sa0, #2, r1  [skipinv]",
        "ld.a.d r2, sa0, #2  [skipinv drc=0]", "wdf [drc=0]", out("r2")]),
    # P1 read still holds on this box
    T("p1w_ldr_core_id", bufs=[R], fsa=[(0, "R")], regs=[CORE_ID], dumps=[("R", 0, 0x2)],
      fs=rd(CORE_ID, 0) + [out("r0")]),
    # controls: does a write to an unused code-base register land? (host read-back = after)
    T("p1w_str_base14", bufs=[R], fsa=[(0, "R")], regs=[BASE14], dumps=[("R", 0, 0x2)], fs=wr(BASE14, S14)),
    T("p1w_str_base15", bufs=[R], fsa=[(0, "R")], regs=[BASE15], dumps=[("R", 0, 0x2)], fs=wr(BASE15, S15)),
    # read-only: the live BIF bank and directory base (host and shader), for the write test and G2
    T("p1w_bif_read", bufs=[R], fsa=[(0, "R")], regs=[BANK0, DIRBASE], dumps=[("R", 0, 0x3)],
      fs=rd(BANK0, 0) + rd(DIRBASE, 1) + [out("r1")]),
]
