# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 non-limm ALU-write test: or r5, r5, r17, then ld.a.d r3, r5, #0.
r5's leftover 0x0c004000 | r17's 0x0fd79000 = 0x0fd7d000. A fault there means the
ALU wrote r5; a fault at 0x0c004000 means it did not. No limm. If the ALU writes
but limm (attempt 32) does not, limm is specifically broken. Bring-up only."""
from .sgx530_usse import *  # noqa: F401,F403
USSE_PHAS = True
USSE_ALUWRITE = True
