# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 limm+str confirmation with a DIFFERENT constant, to rule out residual.
limmwr_r5 wrote 0xa5a and EVENT_TIMER read 0xa5a — but EVENT_TIMER is sticky across
runs (the xorr5 run, which never writes it, also showed 0xa5a), so a single 0xa5a
read is ambiguous between "str wrote it" and "residual from an earlier 0xa5a run".
A fresh constant cannot be residual: this writes 0x5c3 (b24 clear, no timer enable).
Read EVENT_TIMER (0xacc): **0x5c3** => limm AND str definitively commit under §9
(the architecture-decider confirmed); 0xa5a or r5's leftover => re-examine. The init
clears EVENT_TIMER to 0 (SRVINIT part 1), so a fresh insmod starts clean. Bring-up
only; never ships."""
from .sgx530_usse import *  # noqa: F401,F403
USSE_PHAS = True
USSE_LIMMTEST = 5
DIAG_TIMER = 0x5c3  # a fresh constant distinct from 0xa5a; b24 clear so no timer starts
