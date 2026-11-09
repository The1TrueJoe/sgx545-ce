# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""P3 (destructive part): can a client PDS select code base 15, the kernel code heap? (UAPI.md 7.2)

The pixel task's DOUTU word 0 names code base 15 instead of 0, with the real program's offset.
Base 15 maps the microkernel's code heap, so the hardware runs kernel code (or an unmapped
address) as the client's pixel task. Expected to fault or wedge the GPU; the wedge is the
finding (a client should not be able to point the USSE at the kernel code base). Run alone,
last, with the Bench plug ready. If it renders normally instead, base 15 is reachable AND
benign, which is worse."""
from iso import T

R = ("R", 0, 0xf, 0x1000, 0x1000)
PIXHEAP = 0x37000000
UKCODE = 0x30000000
B27 = 0x08000000

TESTS = [
    # base 15, offset = (va - pixel heap)/8 (the normal offset): lands in the kernel code heap
    T("p3_base15", fsw0=(0xf, PIXHEAP, 0), fs=["mov o0, sa9  [skipinv end mask=1]"], fstemps=0),
    # base 15 with b27, same
    T("p3_base15_b27", fsw0=(0xf, PIXHEAP, B27), fs=["mov o0, sa9  [skipinv end mask=1]"], fstemps=0),
]
