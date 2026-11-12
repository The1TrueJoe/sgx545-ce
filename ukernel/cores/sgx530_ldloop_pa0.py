# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 gap-19 test T2 (sgx530-kernel, coordinator-approved): the pa0 load in a
tight loop. phas.i, then L: ld.a.d r3, pa0, #0; wdf; ba L. The load is issued
forever, so if the host switches the single directory list (list 0) out from under
the running task, the next fetch faults at PA0's page — a direct test of whether a
per-kick list switch isolates a running EDM task on a single-context core. Same
packaging as sgx530_pa0. Bring-up only: no golden, never ships."""
from .sgx530_usse import *  # noqa: F401,F403
USSE_PHAS = True
USSE_LDLOOP_PA0 = True
