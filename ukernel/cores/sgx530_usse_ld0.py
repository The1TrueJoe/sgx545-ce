# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 ld-gate probe, offset 0, for stage 1's attempt 29 (does the offset
matter?). If ld.a.d r3, r17, #0 faults at 0x0fd79000 like #42, the address does
not depend on the offset. Bring-up only."""
from .sgx530_usse import *  # noqa: F401,F403
USSE_PHAS = True
USSE_LDGATE = True
USSE_LDOFF = 0
