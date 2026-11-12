# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 gate-1 decider that survives the §9 subset trap: xor r5, r5, r17 then
ld.a.d r3, r5, #0. Under §9, ctlblk (r17 = 0x0f003000) is a SUBSET of r5's leftover
(0x0fa93000), so `or r5, r5, r17` leaves r5's value unchanged whether or not the
write retires — the or-based aluwr/ldr5_ctl pair can't tell. `xor` is a
non-identity there: r5 ^ r17 = 0x0fa93000 ^ 0x0f003000 = 0x00a90000, a DIFFERENT,
unmapped address. So:
  - fault at 0x00a90000 => the xor changed r5 => a temporary write RETIRED (gate 1 YES);
  - fault at 0x0fa93000 => r5 untouched => it did NOT (gate 1 NO).
Two distinct predicted addresses; no reliance on o0 or cross-image leftovers.
Bring-up only; never ships. (Addresses assume r5 leftover 0x0fa93000 and r17/ctlblk
0x0f003000 as measured 2026-11-12; if either moves, recompute r5^r17.)"""
from .sgx530_aluwr import *  # noqa: F401,F403
USSE_ALU_OP = "xor"
