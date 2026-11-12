# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""The SGX530 USSE probe with phas.i first, for stage 1's twenty-sixth attempt
(docs/porting/cores/sgx530-stage1.md, "After the twenty-fifth attempt").

Attempt 25 showed the probe (and the diagnostic ladder) derail before the first
instruction lands, with an image-dependent garbage fetch address. Both put work
before phas.i; the real microkernel entry has phas.i first, and the SGX530 lacks
USE_UNLIMITED_PHASES. This build is cores/sgx530_usse.py with phas.i emitted
first, to test whether that is the requirement. Bring-up only.
"""
from .sgx530_usse import *  # noqa: F401,F403

USSE_PHAS = True
