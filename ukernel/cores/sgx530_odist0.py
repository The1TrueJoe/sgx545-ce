# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 output-distinctness probe o0: are the outputs distinct physical
registers or aliases? Attempt 42 wrote the same value (0x0fd7d000) to every
output, so it cannot tell. This writes o0..o9 ten distinct values (rN's own
leftover) then reads o0. It must read its own source's leftover; any other page
means a later write clobbered it, i.e. the outputs alias. Bring-up only."""
from .sgx530_usse import *  # noqa: F401,F403
USSE_PHAS = True
USSE_ODIST = 0
