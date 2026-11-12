# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 limm field probe: limm r5 = bit 22 (0x400000), then ld.a.d r3, r5, #0.
The fault page is the value the 530 actually loaded, reading where its limm
value-field places bit 22. Attempt 30 sweep. Bring-up only."""
from .sgx530_usse import *  # noqa: F401,F403
USSE_PHAS = True
USSE_LDGATE = True
USSE_LDOFF = 0
USSE_LDBASE = 0x400000
