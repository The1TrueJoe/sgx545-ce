# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 aluwr with phas.i temps4 = 8 (= 32 temporaries), for attempt 36:
does the 530 take the tasks temps from phas.i, not the DOUTU? or r5,r5,r17 then
ld r5 -- fault 0x0fd7d000 means the write landed (temps allocated), 0x0c004000
means still none. Bring-up only."""
from .sgx530_aluwr import *  # noqa: F401,F403
USSE_PHAS_TEMPS4 = 8
