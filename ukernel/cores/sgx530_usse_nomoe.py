# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""The SGX530 mirror probe without [moe], for stage 1's twenty-eighth attempt
(docs/porting/cores/sgx530-stage1.md, "After the twenty-seventh attempt").

Attempt 27 pinned the real image's fault to the heartbeat load ld.a.d r3, r17,
#42 [moe]; the effective address (0x0fd79000) isn't r17 + a clean scaled offset.
The SGX545 image uses [moe] 409 times and never sets MOE up, relying on the
default MOE state (increment 1, hwt_p1). If the SGX530's default MOE/address
state differs, every [moe] access is corrupt. This build is the attempt-27
mirror with [moe] dropped: if the load now lands, [moe] is the cause. Bring-up
only.
"""
from .sgx530_usse import *  # noqa: F401,F403

USSE_PHAS = True
USSE_MIRROR = True
USSE_MOE = False
