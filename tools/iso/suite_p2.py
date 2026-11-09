# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""P2: can a client's TA/3D task reach kernel memory through the MMU? (UAPI.md 7.2)

The task translates through the job's page directory (UKERNEL.md 6). If that PD maps the
kernel heaps, a client shader's ld/st reaches kernel structures. These read known kernel
addresses (SRVINIT.md allocations) into a client buffer with ld, and -- last, riskiest -- do a
self-contained write: read a kernel word, store a sentinel to it, read again. If the second
read shows the sentinel, the write landed in kernel memory. A kernel VA that is NOT mapped in
the client PD faults instead, which may trigger HWR; that is the P2-holds outcome and is itself
the finding. Also tries to allocate in the kernel heaps as an ordinary client."""
from iso import T, out

R = ("R", 0, 0xf, 0x1000, 0x1000)
KCCB = 0x32000000       # kernel CCB
TA3DCTL = 0x32003000    # TA3D control block (sa image; word 0x17 = PD phys)
HOSTCTL = 0x32003100    # host control block
UKCODE = 0x30000000     # microkernel code
TASIG = 0x3200a420      # TA signature buffer (RW scratch): least bad to scribble on


def ld_kernel(va, n):
    # R addr in sa0; load n dwords from the kernel VA and store to R
    body = [f"limm r1, #{va:#x}  [skipinv]"]
    for i in range(n):
        body += [f"ld.a.d r0, r1, #{i}  [skipinv drc=0]", "wdf [drc=0]", f"st.a.d sa0, #{i}, r0  [skipinv]"]
    return body + [out("r0")]


TESTS = [
    # can an ordinary client allocate in the kernel heaps at all? (heap 3 = kernel data, heap 4... no:
    # heap ids: kernel data = 3 here per SRVINIT, but tools/gen uses 3 for KDATA)
    T("p2_alloc_kdata", bufs=[("K", 3, 0xf, 0x1000, 0x1000)], dumps=[("K", 0, 1)], fs=None),
    # reads of kernel memory from a pixel task
    T("p2_ld_hostctl", bufs=[R], fsa=[(0, "R")], dumps=[("R", 0, 0x12)], fs=ld_kernel(HOSTCTL, 0x11)),
    T("p2_ld_ta3dctl", bufs=[R], fsa=[(0, "R")], dumps=[("R", 0, 0x19)], fs=ld_kernel(TA3DCTL, 0x18)),
    T("p2_ld_kccb", bufs=[R], fsa=[(0, "R")], dumps=[("R", 0, 0x9)], fs=ld_kernel(KCCB, 8)),
    T("p2_ld_ukcode", bufs=[R], fsa=[(0, "R")], dumps=[("R", 0, 0x9)], fs=ld_kernel(UKCODE, 8)),
    # read from a vertex task
    T("p2_vs_ld_ta3dctl", bufs=[R], vsa=[(0, "R")], dumps=[("R", 0, 0x19)],
      vs=[f"limm r1, #{TA3DCTL:#x}  [skipinv]"] +
         sum(([f"ld.a.d r0, r1, #{i}  [skipinv drc=0]", "wdf [drc=0]", f"st.a.d sa6, #{i}, r0  [skipinv]"]
              for i in range(0x18)), [])),
    # the write test, last: self-contained read/write/read of a kernel scratch word
    T("p2_wr_tasig", bufs=[R], fsa=[(0, "R")], dumps=[("R", 0, 0x4)], fs=[
        f"limm r1, #{TASIG:#x}  [skipinv]", "ld.a.d r0, r1, #0  [skipinv drc=0]", "wdf [drc=0]",
        "st.a.d sa0, #0, r0  [skipinv]", "limm r2, #0xdeadbeef  [skipinv]", "st.a.d r1, #0, r2  [skipinv]",
        "ld.a.d r3, r1, #0  [skipinv drc=0]", "wdf [drc=0]", "st.a.d sa0, #1, r3  [skipinv]", out("r3")]),
]
