# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 st-from-output probe: or o0, r5, r17 then st.a.d pa0, #0, o0.

limm cannot put a mapped address in a temporary on the 530, so there is no temp
base to store through. PA0 can: a standalone DOUTA (driver-set, persists) holds a
mapped control-block scratch address, and this task stores o0's value to [pa0].
The host reads that scratch word: 0x0fd7d000 => st reads an output as its data
source (and o0 took the write); unchanged => it does not. Pair with the driver's
DOUTA that sets PA0 = the scratch address. Self-loops; no fault expected (pa0 is
mapped). Bring-up only."""
from .sgx530_usse import *  # noqa: F401,F403
USSE_PHAS = True
USSE_STOUT = "o0"
