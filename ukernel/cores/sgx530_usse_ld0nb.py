# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 ld-gate probe, offset 0, base from a known limm (r5 = kernel_data +
0x4000 = 0x0f004000) instead of the patched r17, for stage 1's attempt 29 (does
the base register matter?). If the fault stays at 0x0fd79000, the base operand is
not read from the register. Bring-up only."""
from .sgx530_usse import *  # noqa: F401,F403
USSE_PHAS = True
USSE_LDGATE = True
USSE_LDOFF = 0
USSE_LDBASE = 0x0f004000
