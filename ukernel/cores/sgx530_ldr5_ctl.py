# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 raw-r5 control for the §9 gate-1 decode: same image as sgx530_aluwr but
the `or r5, r5, r17` is replaced by a `nop`, so `ld.a.d r3, r5, #0` reads r5's raw
leftover under §9's launch. aluwr (or) vs this (nop) is a within-r5 comparison:
different fault addresses => the or changed r5 => a temporary write retired; the
same address => r5 was untouched => it did not. This avoids comparing r5 to o0,
which the §9 run showed is inconsistent with the simple leftover model (aluwr's
0x0fa93000 is not a subset of aluwr_o0's 0x0f373000). Bring-up only; never ships."""
from .sgx530_aluwr import *  # noqa: F401,F403
USSE_ALU_NOWRITE = True
