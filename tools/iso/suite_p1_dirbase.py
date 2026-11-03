# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""P1 open half, the critical test (UAPI.md 7.2 / G1, Joseph's D9, 2026-11-02). Destructive;
run alone, after suite_p1_write has passed, with the box's GPU lock held.

The question: does a client USSE `str` to an EFFECTIVE register take effect, not just a read?
BIF_DIR_LIST_BASE0 (0xc84) is the page-directory base the BIF requestors translate through. The
client shader reads it, writes a page-aligned sentinel, and reads it back; but the authority is
the host, which reads 0xc84 before and after the frame over the PCI BAR. `sgx_flush` has a 3 s
timeout, so the host after-read runs even when the GPU wedges, and the CPU/BAR path is
independent of GPU translation.

  after == SENT  -> the write LANDED: a client can repoint translation, so the new driver must
                    forbid `str` to effective registers (kernel shader validation covers stores).
  after == before-> the write did NOT land: narrower validation (reads only) may suffice.

If the write lands and is live, the next GPU access (the shader's own next fetch / the PBE store)
mistranslates and the GPU wedges; HW recovery resets the core and restores the register. The
sentinel is inside RAM and page-aligned so a directory walk faults cleanly rather than bus-hangs.
One test. The EA1 has no smart plug: if this wedges and HW recovery does not bring it back, stop."""
from iso import T, out

DIRBASE = 0xc84
SENT = 0x0a5a5000       # page-aligned recognizable physical sentinel, within RAM (checked before the run)
R = ("R", 0, 0xf, 0x1000, 0x1000)


TESTS = [
    T("p1_str_dirbase", bufs=[R], fsa=[(0, "R")], regs=[DIRBASE], dumps=[("R", 0, 0x4)], fs=[
        # read the live base first (shader copy; host `reg` read is authority)
        f"ldr r2, #{DIRBASE // 4} [drc=0]", "wdf [drc=0]", "st.a.d sa0, #0, r2  [skipinv]",
        # write the sentinel
        f"limm r0, #{SENT:#x}  [skipinv]", f"str #{DIRBASE // 4}, r0",
        # read it back (may mistranslate if the write is live; host read-back is the real datum)
        f"ldr r1, #{DIRBASE // 4} [drc=0]", "wdf [drc=0]", "st.a.d sa0, #1, r1  [skipinv]",
        out("r1")]),
]
