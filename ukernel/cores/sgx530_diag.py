# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""The SGX530 image with a diagnostic event entry, for stage 1's third attempt.

The same microkernel as cores/sgx530.py, but the event entry first writes
markers the host can read after a failed start, and only then runs phas.i and
the normal entry (docs/porting/cores/sgx530-stage1.md, "The diagnostic
entry"). A bring-up build only: it has no golden and never ships.
"""
from .sgx530 import *  # noqa: F401,F403

DIAG_TIMER = 0xa5a           # 1: EVENT_TIMER (K) = this, b24 (enable) clear
DIAG_ENTRY = 0xd1a60001      # 2: control block word 0x28 (the log's "wait") = this;
                             # 3: CORE_ID to word 0x18 (version); 4: heartbeat +1
                             # (ld/add/st); then phas.i and the normal entry's own +1:
                             # 2 per event while both halves run
