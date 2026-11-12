# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 task-written-attribute test (attempt 41): or pa0, r5, r17 then ld pa0.
Does a task-written PRIMARY ATTRIBUTE retire? 0x0fd7d000 = yes. Bring-up only."""
from .sgx530_aluwr import *  # noqa: F401,F403
USSE_ALU_DSTOP = "pa0"
