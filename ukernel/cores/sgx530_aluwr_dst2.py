# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 low-register write test: or r2,r2,r17 then ld r2, to check whether a
low register is a writable temporary where r5 is not. Fault at the or result
(r2 leftover | r17 0x0fd79000) means it wrote; at r2 leftover means it did not.
Attempt 39. Bring-up only."""
from .sgx530_aluwr import *  # noqa: F401,F403
USSE_ALU_DST = 2
