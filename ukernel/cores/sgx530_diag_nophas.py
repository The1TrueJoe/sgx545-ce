# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""The SGX530 diagnostic build without phas.i: the same markers, then the
normal entry falls through with no phase instruction. The follow-up if the
diagnostic entry stops at heartbeat 1 (docs/porting/cores/sgx530-stage1.md,
"The diagnostic entry"). Bring-up only: no golden, never ships.
"""
from .sgx530_diag import *  # noqa: F401,F403

DIAG_PHAS = False
