# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 pairing/latency test: aluwr with 2 nop(s) between or r5,r5,r17 and
ld r5. The 530 lacks USE_NO_INSTRUCTION_PAIRING (the 545 has it), so it may pair
the write with the dependent read, making ld see stale r5. If nops make ld fault
at 0x0fd7d000 (the or result) instead of 0x0c004000 (leftover), that is the
cause. Attempt 38. Bring-up only."""
from .sgx530_aluwr import *  # noqa: F401,F403
USSE_ALU_NOPS = 2
