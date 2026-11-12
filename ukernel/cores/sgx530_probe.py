# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""The SGX530 diagnostic build plus code-base-unit probes, for stage 1's fourth
attempt (docs/porting/cores/sgx530-stage1.md, "The fourth attempt").

The event entry is cores/sgx530_diag.py's ladder, unchanged. After the image,
one page per candidate unit of USE_CODE_BASE's address field holds a probe that
writes EVENT_TIMER = PROBE_TIMER + k (unit 2**k bytes) and ends. The host maps
probe k's page at 0x40000 << k, where code base 15 (field 0x40000, written for a
128 B unit) would start under unit 2**k. The 128 B case is the ladder itself
(EVENT_TIMER 0xa5a). Bring-up only: no golden, never ships.
"""
from .sgx530_diag import *  # noqa: F401,F403

PROBES = [0, 1, 2, 3, 4, 5, 6, 8, 9]   # log2 of the unit; 7 (128 B) is the ladder;
                                       # 10+ would start past the 28-bit space
PROBE_TIMER = 0xb00                    # EVENT_TIMER 0xb0k: unit 2**k bytes, b24 clear
