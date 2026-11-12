# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 primary-attribute read, the USSE side of probe (c) (attempt 39):
phas.i, then ld.a.d r3, pa0, #0, then a loop. The driver's pds0 DOUTAs a known
value (0x0e850000) into PA0; if this ld faults at 0x0e850000, the task sees a
PDS-written primary attribute where USSE-written temporaries aren't visible,
which would say the SGX530 microkernel should carry its pointers in attributes.
Bring-up only."""
from .sgx530_usse import *  # noqa: F401,F403
USSE_PHAS = True
USSE_PA_READ = True
