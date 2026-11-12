# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 ld.a.d source-operand sweep: ld.a.d r3, r6, #0 (register value
unused). The fault page is the address the 530 derives from naming register
6. Attempt 31. Bring-up only."""
from .sgx530_usse import *  # noqa: F401,F403
USSE_PHAS = True
USSE_LDGATE = True
USSE_LDOFF = 0
USSE_LDREG = 6
