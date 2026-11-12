# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 st-to-memory control (attempt 46): st.a.d pa0, #0, r5 - store a TEMP's
value, not an output. The sibling sgx530_stout_o0 stored o0 and the version word
read 0 (attempt 45, with the arena mapped no-EDMPROTECT so the store no longer
faults). This isolates why: r5's leftover is 0x0c004000 (temps read back their
stable leftover even though writes to them don't retire). If the version word
reads 0x0c004000, st reaches memory from a temp and the o0 failure is st-reading-
an-output specifically; if it reads 0, st-to-memory / the flush is the problem for
any source. Pair with the same PA0 = ctlblk+0x60 DOUTA and the no-EDMPROTECT arena.
Self-loops; st; idf; wdf. Bring-up only."""
from .sgx530_usse import *  # noqa: F401,F403
USSE_PHAS = True
USSE_STOUT = "r5"
