# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""P1 open half, the DoS question in isolation (UAPI.md 7.2 / G1, D9). Joseph asked for this
re-run after the first one was confounded by a concurrent dEQP chunk that had wedged the box.

Run this ONLY when the box is known-good and the GPU is idle: nothing else holding the lock,
WPE stopped, no dEQP. The suite proves the GPU is clean right before the store (`sanity_frame`
must render the green triangle with no HWR), then does the one store. Read together with a
before/after SGX-recovery count and `uptime` on the box:

  sanity clean, store `render done`, reg unchanged, 0 new HWRs, no reboot  -> store is NOT a DoS
  sanity clean, then the store wedges/reboots                              -> store IS a DoS
  sanity itself wedges                                                     -> box wasn't good; abort

The EA1 has no smart plug: if the store wedges and HW recovery doesn't bring it back, stop."""
from iso import T, out

DIRBASE = 0xc84
SENT = 0x0a5a5000       # page-aligned recognizable physical sentinel, within RAM
R = ("R", 0, 0xf, 0x1000, 0x1000)


TESTS = [
    # 1) prove the GPU renders clean with only us on it
    T("sanity_frame"),
    # 2) the store, alone
    T("p1_str_dirbase", bufs=[R], fsa=[(0, "R")], regs=[DIRBASE], dumps=[("R", 0, 0x4)], fs=[
        f"ldr r2, #{DIRBASE // 4} [drc=0]", "wdf [drc=0]", "st.a.d sa0, #0, r2  [skipinv]",
        f"limm r0, #{SENT:#x}  [skipinv]", f"str #{DIRBASE // 4}, r0",
        f"ldr r1, #{DIRBASE // 4} [drc=0]", "wdf [drc=0]", "st.a.d sa0, #1, r1  [skipinv]",
        out("r1")]),
]
