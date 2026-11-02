# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""P4: does a client task see the microkernel's live state in the secondary attributes? (UAPI.md 7.2)

The microkernel keeps its state in sa0-sa63, which the hardware does not clear between tasks
(UKERNEL.md 1). Our fragment's secondary program DMAs 11 dwords, so it overwrites sa0-sa10;
sa11-sa63 are whatever the last task (a microkernel entry, between kicks) left. This reads
them into a client buffer. If they hold kernel pointers -- the TA3D control block address, the
page directory's physical address (UKERNEL.md sa0x17), CCB addresses -- P4 fails: a client can
read kernel addresses. The least risky of the four (no register or kernel-memory access)."""
from iso import T, out

R = ("R", 0, 0xf, 0x1000, 0x1000)


def read_sa(first, count):
    # R's address is in sa0 (fsa); store sa<first..first+count-1> to R[0..]
    body = [f"st.a.d sa0, #{i}, sa{first + i}  [skipinv]" for i in range(count)]
    return body + [out(f"sa{first}")]


TESTS = [
    # sa0-sa10 are the DMA'd uniform block (known); read them back as a control
    T("p4_sa0_10", bufs=[R], fsa=[(0, "R")], dumps=[("R", 0, 0xc)], fs=read_sa(0, 11)),
    # the leftovers: sa11..sa42, then sa43..sa63
    T("p4_sa11_42", bufs=[R], fsa=[(0, "R")], dumps=[("R", 0, 0x21)], fs=read_sa(11, 32)),
    T("p4_sa43_63", bufs=[R], fsa=[(0, "R")], dumps=[("R", 0, 0x16)], fs=read_sa(43, 21)),
    # the same from a vertex task (its secondary DMAs 10 dwords -> sa0-sa9; R addr in sa6)
    T("p4_vs_sa10_41", bufs=[R], vsa=[(0, "R")], dumps=[("R", 0, 0x21)],
      vs=[f"st.a.d sa6, #{i}, sa{10 + i}  [skipinv]" for i in range(32)]),
]
