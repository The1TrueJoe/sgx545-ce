# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 limm-writes-the-register test: limm r17, #0xa5a then str r17 to
EVENT_TIMER. EVENT_TIMER = 0xa5a => limm wrote r17; r17s leftover => it did not.
Attempt 32. Bring-up only."""
from .sgx530_usse import *  # noqa: F401,F403
USSE_PHAS = True
USSE_LIMMTEST = 17
