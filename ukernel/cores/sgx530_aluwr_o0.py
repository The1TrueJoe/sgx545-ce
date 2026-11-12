# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 output-write test (attempt 41): or o0, r5, r17 then ld.a.d r3, o0, #0.
Does a task-written OUTPUT register retire where a temporary doesnt? Fault at
0x0fd7d000 (r5 0x0c004000 | r17 0x0fd79000) = the output write landed, so the
microkernel could use outputs for scratch. Any other page = o0 leftover, didnt.
Bring-up only."""
from .sgx530_aluwr import *  # noqa: F401,F403
USSE_ALU_DSTOP = "o0"
