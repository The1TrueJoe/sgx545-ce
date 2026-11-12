# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 output-distinctness probe o10 (margin-1 check). Attempt 42 showed
eleven writable outputs against a peak of 10, so o10 distinct = one register of
margin. Writes o0..o10 eleven distinct leftovers then reads o10: it must read its
own source's leftover (r17 = 0x0fd79000); any other page = aliasing. Bring-up
only."""
from .sgx530_usse import *  # noqa: F401,F403
USSE_PHAS = True
USSE_ODIST = 10
USSE_ODIST_N = 11
