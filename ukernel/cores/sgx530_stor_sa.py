# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 store with a SECONDARY-ATTRIBUTE base (coordinator lead a): does a store
COMMIT when its address is held in a secondary attribute, the vendor's route?
vendor-setup §5/§6: the vendor microkernel's stores use addresses held in SA,
filled by the control-block DOUTD DMA in the init task's second PDS program. Our
stor5 used a temp base (r17) and dropped; the stage-1 stout used pa0 (primary) and
dropped. Here the base is sa{N}: `st.a.d sa{N}, #0, r5`.

THE DRIVER (sgx530-kernel) fills sa{N} via a DOUTD with a mapped EDMPROTECT scratch
address (as the vendor's secondary-update program does), zeroes that address, runs
this under the full §9 launch (word1 = 0x04000000 / bit26 on), then reads it back:
  - 0x0fa93000  => the SA-held store COMMITTED — the 530's store path needs an
                   SA-held address (temp/pa bases don't work);
  - 0 / unchanged => SA isn't the key either; the store path is deeper;
  - BIF 0x4004 fault => reached the MMU, EDMPROTECT rejected it.

Bring-up only. The SA index is a parameter; the driver picks one it can fill and
confirms the scratch address and its EDMPROTECT mapping."""
from .sgx530_usse import *  # noqa: F401,F403
USSE_PHAS = True
USSE_STOR_SA = 16  # mid-range of the vendor's sa0-32; the driver fills it via DOUTD
