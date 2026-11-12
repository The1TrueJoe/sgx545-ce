# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""The SGX530 image whose event task is a minimal USSE probe, for stage 1's
twenty-fifth attempt (docs/porting/cores/sgx530-stage1.md, "After the
twenty-fourth attempt").

With USE_CODE_BASE >> 8 (attempt 24) the EDM task fetches from the kernel code
heap, runs, and faults at a b2 code address before the diagnostic ladder's first
EVENT_TIMER write. This build replaces the event task with the smallest program
that runs one st.a.d, one str (the EVENT_TIMER write), a second st.a.d, then a
self-loop, so the host read-backs name the first instruction class that derails.
Bring-up only: no golden, never ships.
"""
from .sgx530 import *  # noqa: F401,F403

USSE_PROBE = True
DIAG_TIMER = 0xa5a           # EVENT_TIMER value the str writes (b24 clear: no timer)
USSE_MARK_A = 0xbeef1        # ctlblk 0x28, written by st.a.d before the str
USSE_MARK_B = 0xc0de2        # ctlblk 0x18, written by st.a.d after the str
