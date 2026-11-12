# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 §9-valid store test (coordinator priority 1): does a task store COMMIT to
EDMPROTECT memory under the vendor launch? The stage-1 stout probe stored to [pa0],
but §9's launch seeds pa0, so its fault was on a bad pointer, not a real "store
dropped." Here the store base is r17, loader-patched to ctlblk — a mapped
EDMPROTECT address §9 does NOT touch. It stores r5 (whose reads give the known
leftover 0x0fa93000) to [ctlblk + 4*(off+1)] (st.a.d offset is in dwords with a +1,
ISA.md). The host then reads that dword:
  - 0x0fa93000  => the store COMMITTED to EDMPROTECT memory (the task is the EDM
                   requestor under §9's word1 = 0x04000000);
  - 0 / unchanged => silently dropped (as in stage 1, but now under a real launch);
  - BIF store fault 0x4004 => reached the MMU, EDMPROTECT rejected it (not the EDM
                   requestor — word1 did not grant it).

off defaults to a scratch dword well past the 0x128-byte control block's live
fields; THE DRIVER (sgx530-kernel) must confirm ctlblk + 4*(off+1) is a mapped,
EDMPROTECT, readable scratch word in the ta3dctl buffer's page (and zero it first).
If a different offset is safer, say so and the image rebuilds. Bring-up only."""
from .sgx530_usse import *  # noqa: F401,F403
USSE_PHAS = True
USSE_STOR_OFF = 0x7f  # max the 7-bit field allows: ctlblk + 4*0x80 = ctlblk + 0x200 (byte 512),
                      # past the 0x128-byte control block, within its first page
