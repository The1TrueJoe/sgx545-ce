# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 pa-retire decider under §9, the xor counterpart of sgx530_xorr5. The
or-based sgx530_aluwr_pa0w is degenerate for the same reason sgx530_aluwr is
(r17 = ctlblk 0x0f003000 is a subset of the destination's bits), so it cannot
show a pa retire. `xor pa0, r5, r17` writes r5 ^ r17 = 0x00a90000, then
`ld.a.d r3, pa0, #0`:
  - fault at 0x00a90000 => the xor landed in pa0 => a primary-attribute write RETIRED;
  - fault elsewhere (pa0's own leftover) => it did NOT.
Confirms whether the §9 launch gives the task a writable `pa` bank as well as a
writable temporary file (sgx530_xorr5 already showed temps retire). The vendor
task writes PA2 (vendor-setup §3), so pa retire matters. Bring-up only; never
ships. (0x00a90000 assumes r5 leftover 0x0fa93000 and r17 0x0f003000, 2026-11-12.)"""
from .sgx530_aluwr import *  # noqa: F401,F403
USSE_ALU_DSTOP = "pa0"
USSE_ALU_OP = "xor"
