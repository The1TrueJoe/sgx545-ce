# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 orthogonal gate-1 confirmation: and r5, r5, r17 then ld.a.d r3, r5, #0.
Independent of sgx530_xorr5's specific address. Since r17 (ctlblk 0x0f003000) is a
subset of r5's leftover 0x0fa93000, r5 & r17 = 0x0f003000 = r17 — a MAPPED address:
  - RETIRED: r5 becomes 0x0f003000, the load reads [ctlblk+4] successfully => NO fault
    (the task runs its loop, or faults elsewhere, but not at 0x0fa93000);
  - NOT retired: r5 stays 0x0fa93000 (unmapped) => fault at 0x0fa93000.
Signalling "retired" by the ABSENCE of a fault (rather than a specific new address)
rules out any coincidence in the xorr5 result. Pairs with sgx530_xorr5. Bring-up
only; never ships. (Values assume r5 leftover 0x0fa93000, ctlblk 0x0f003000.)"""
from .sgx530_aluwr import *  # noqa: F401,F403
USSE_ALU_OP = "and"
