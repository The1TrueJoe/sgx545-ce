# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 output-distinctness probe o8 (see sgx530_odist0.py). Writes o0..o9 ten
distinct leftovers, then reads o8: it must read its own source's leftover, else a
later write clobbered it == the outputs alias. Bring-up only."""
from .sgx530_usse import *  # noqa: F401,F403
USSE_PHAS = True
USSE_ODIST = 8
