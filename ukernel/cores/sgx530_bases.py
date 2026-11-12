# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""The SGX530 diagnostic build plus a pad per USE code base, for stage 1's
tenth attempt (docs/porting/cores/sgx530-stage1.md, "After the ninth attempt").

The event entry is cores/sgx530_diag.py's ladder, unchanged (code base 15 at
offset 0). After the image, on 128 B boundaries, pad n writes EVENT_TIMER =
PAD_TIMER + n and ends. The host points USE_CODE_BASE_n at pad n's address, so
a DOUTU that starts its task through code base n, whatever its word 0 says,
names n. Bring-up only: no golden, never ships.
"""
from .sgx530_diag import *  # noqa: F401,F403

BASE_PADS = list(range(15))  # code bases 0-14; 15 is the ladder at offset 0
PAD_TIMER = 0xc00            # EVENT_TIMER 0xc0n: the task went through code base n
