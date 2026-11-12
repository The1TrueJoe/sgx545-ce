# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 output-writability probe o2 (attempt 42): or o2, r5, r17 then
ld.a.d r3, o2, #0. Fault at 0x0fd7d000 (r5 0x0c004000 | r17 leftover 0x0fd79000)
= o2 took the write; any other page = o2 kept its leftover, not writable. The
fault page is the readout; no mapped base register is needed. Bring-up only."""
from .sgx530_aluwr import *  # noqa: F401,F403
USSE_ALU_DSTOP = "o2"
