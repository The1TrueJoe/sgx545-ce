# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 ld.a.d which-register probe: preload r0..r22 = (N+1)<<16, then
ld.a.d r3, r5, #0. The fault page (M+1)<<16 names register M that the 530 read
when operand r5 was named. Attempt 31. Bring-up only."""
from .sgx530_usse import *  # noqa: F401,F403
USSE_PHAS = True
USSE_LDGATE = True
USSE_LDOFF = 0
USSE_LDPRELOAD = True
USSE_LDREG = 5
