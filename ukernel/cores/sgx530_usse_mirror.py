# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""The SGX530 USSE probe that mirrors the real entry's preamble, for stage 1's
twenty-seventh attempt (docs/porting/cores/sgx530-stage1.md, "After the
twenty-sixth attempt").

Attempt 26: the real image (phas.i first) reaches a b1 data fault at 0x0fd79000
on its first load, while the bare phas probe still derails on a code fetch. The
difference is the real entry patches both r17 (ctlblk) and r14 (pdtable). This
build mirrors the real preamble exactly — phas.i, both patches, the heartbeat
load ld.a.d r3, r17, #HEARTBEAT [moe] — then stores what the load read to ctlblk
0x28 and writes EVENT_TIMER, then self-loops. It isolates the first load: a fault
at 0x0fd79000 pins it to that access; a marker means the load works and the real
fault is later. Bring-up only.
"""
from .sgx530_usse import *  # noqa: F401,F403

USSE_PHAS = True
USSE_MIRROR = True
