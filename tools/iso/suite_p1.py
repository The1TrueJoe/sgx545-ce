# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""P1: can an ordinary client's USSE task read and write EUR_CR_* registers? (UAPI.md 7.2)

R is a client buffer in the general heap; programs get its address in sa0 (fragment) or
sa6 (vertex) and store what they saw there. USE_CODE_BASE_14 is the write target: nothing
uses code base 14, so writing it changes nothing a frame depends on."""
from iso import T, out

CORE_ID, CORE_REV, TIMER = 0x1c, 0x20, 0x144
BASE14 = 0xa44
R = ("R", 0, 0xf, 0x1000, 0x1000)       # READ|WRITE|CACHE_CONSISTENT|NO_SYNCOBJ
S = 0x12340                             # a harmless code base value (never used)


def fs_ldr(off):
    return [f"ldr r0, #{off // 4} [drc=0]", "wdf [drc=0]", "st.a.d sa0, #0, r0  [skipinv]", out("r0")]


TESTS = [
    # the rig itself: the default frame, then a program storing to client memory
    T("sanity_frame"),
    T("sanity_limm", fs=["limm r0, #0x12345678  [skipinv]", out("r0")]),
    T("sanity_st", bufs=[R], fsa=[(0, "R")], dumps=[("R", 0, 8)], fs=[
        "limm r0, #0x11223344  [skipinv]", "limm r1, #0x55667788  [skipinv]",
        "st.a.d sa0, #0, r0  [skipinv]", "st.a.d sa0, #2, r1  [skipinv]",
        "ld.a.d r2, sa0, #2  [skipinv drc=0]", "wdf [drc=0]", out("r2")]),
    # P1: reads from a pixel task
    T("p1_fs_ldr_core_id", bufs=[R], fsa=[(0, "R")], regs=[CORE_ID], dumps=[("R", 0, 1)], fs=fs_ldr(CORE_ID)),
    T("p1_fs_ldr_core_rev", bufs=[R], fsa=[(0, "R")], regs=[CORE_REV], dumps=[("R", 0, 1)], fs=fs_ldr(CORE_REV)),
    T("p1_fs_ldr_timer", bufs=[R], fsa=[(0, "R")], regs=[TIMER], dumps=[("R", 0, 2)], fs=[
        f"ldr r0, #{TIMER // 4} [drc=0]", "wdf [drc=0]", "st.a.d sa0, #0, r0  [skipinv]",
        f"ldr r1, #{TIMER // 4} [drc=0]", "wdf [drc=0]", "st.a.d sa0, #1, r1  [skipinv]", out("r1")]),
    # P1: a write from a pixel task, read back by the task and by the host
    T("p1_fs_str_base14", bufs=[R], fsa=[(0, "R")], regs=[BASE14], dumps=[("R", 0, 1)], fs=[
        f"limm r0, #{S:#x}  [skipinv]", f"str #{BASE14 // 4}, r0", f"ldr r1, #{BASE14 // 4} [drc=0]", "wdf [drc=0]",
        "st.a.d sa0, #0, r1  [skipinv]", out("r1")]),
    T("p1_fs_str_base14_restore", regs=[BASE14], fs=["limm r0, #0  [skipinv]", f"str #{BASE14 // 4}, r0", out("r0")]),
    # P1: the same from a vertex task
    T("p1_vs_ldr_core_id", bufs=[R], vsa=[(0, "R")], regs=[CORE_ID], dumps=[("R", 0, 1)], vs=[
        f"ldr r0, #{CORE_ID // 4} [drc=0]", "wdf [drc=0]", "st.a.d sa6, #0, r0  [skipinv]"]),
    T("p1_vs_str_base14", bufs=[R], vsa=[(0, "R")], regs=[BASE14], dumps=[("R", 0, 1)], vs=[
        f"limm r0, #{S + 0x80:#x}  [skipinv]", f"str #{BASE14 // 4}, r0", f"ldr r1, #{BASE14 // 4} [drc=0]", "wdf [drc=0]",
        "st.a.d sa6, #0, r1  [skipinv]"]),
    T("p1_vs_str_base14_restore", regs=[BASE14], vs=["limm r0, #0  [skipinv]", f"str #{BASE14 // 4}, r0"]),
]
