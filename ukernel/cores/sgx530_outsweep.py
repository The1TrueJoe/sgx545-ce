# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 output-count sweep (attempt 42): how many output registers are
task-writable, and does st read an output as its data source?

Writes o0..o7 with `or oN, r5, r17` (= 0x0fd7d000 if the output took the write,
the value attempt 41 saw on o0), then stores each to a control-block scratch word
at 0x18 + 4n (the 0x18..0x37 scratch range, UKERNEL.md). The host reads those 8
scratch words after the run:

  0x18 <- o0   0x1c <- o1   0x20 <- o2   0x24 <- o3
  0x28 <- o4   0x2c <- o5   0x30 <- o6   0x34 <- o7

A word that reads 0x0fd7d000 means that output is writable and st-from-output
works; its own leftover means the output did not take the write. o0 is the
control (attempt 41 proved it writable via ld; here it is read via st, so a miss
on o0 isolates st-from-output). Self-loops, so emit-at-end is untested - that
needs the task to end and is a separate, recovery-careful probe. Bring-up only."""
from .sgx530_usse import *  # noqa: F401,F403
USSE_PHAS = True
USSE_OUTSWEEP = 8
