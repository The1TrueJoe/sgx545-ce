# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 load-retire check (coordinator): does a load land its value in a temp?
The EDM-with-host-memory path needs the microkernel to LOAD the CCB and control
block into retiring temporaries. Temps retire for ALU writes (sgx530_xorr5); this
confirms the same for a loaded value, so "loads retire" is C, not assumed.

r17 is loader-patched to ctlblk. The DRIVER pre-writes a distinctive UNMAPPED
sentinel (e.g. 0x0dddd000) at [ctlblk + 0x200] (the word stor5 confirmed mapped),
then:
  ld.a.d r5, r17, #0x7f   -> r5 = the sentinel, IF loads retire
  ld.a.d r3, r5, #0       -> faults at r5
Read BIF_FAULT:
  - the sentinel (0x0dddd000)  => the loaded value landed in r5 — loads retire (C);
  - 0x0fa93000 (r5's leftover) => the load did not retire.
The first ld reads an EDMPROTECT page, which reads fine (reads worked all through
stage 1, and the task is the EDM requestor under §9). Bring-up only; never ships."""
from .sgx530_usse import *  # noqa: F401,F403
USSE_PHAS = True
USSE_LDRETIRE = 0x7f  # ctlblk + 4*0x80 = ctlblk + 0x200, the mapped scratch word stor5 used
