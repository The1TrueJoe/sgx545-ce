# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 low-register write test: or r0,r0,r17 then ld r0, to check whether a
low register is a writable temporary where r5 is not. Fault at the or result
(r0 leftover | r17 0x0fd79000) means it wrote; at r0 leftover means it did not.
Attempt 39. Bring-up only."""
from .sgx530_aluwr import *  # noqa: F401,F403
USSE_ALU_DST = 0
