# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""P3 (information part): the DOUTU word-0 b27 flag. (UAPI.md 7.2, PDS.md G4)

b27 is set on the microkernel's own DOUTU entries, meaning unknown. These run an ordinary
base-0 pixel program with b27 set, doing the same register read P1 does, to see whether b27
changes what the task is allowed. Low risk (base 0, the client's own code). The destructive
part -- selecting code base 15, the kernel code heap -- is suite_p3_wedge.py, run alone."""
from iso import T, out

R = ("R", 0, 0xf, 0x1000, 0x1000)
CORE_ID = 0x1c
B27 = 0x08000000
PIXHEAP = 0x37000000


def fs_ldr(off):
    return [f"ldr r0, #{off // 4} [drc=0]", "wdf [drc=0]", "st.a.d sa0, #0, r0  [skipinv]", out("r0")]


TESTS = [
    # b27 set, base 0: does the register read that P1 tries behave any differently?
    T("p3_b27_ldr_core_id", bufs=[R], fsa=[(0, "R")], regs=[CORE_ID], dumps=[("R", 0, 1)],
      fsw0=(0, PIXHEAP, B27), fs=fs_ldr(CORE_ID)),
    # b27 set, base 0, a plain draw: does the frame still render? (b27 harmless on a normal task?)
    T("p3_b27_plain", fsw0=(0, PIXHEAP, B27), fs=["mov o0, sa9  [skipinv end mask=1]"], fstemps=0),
]
