# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""SGX530 EDM-path microkernel (host-owned memory), for the driver's edm_init mode.

Same as the sgx530 microkernel, but with the completion handler reduced for the
530's dropped stores (docs/porting/cores/sgx530-edm-path.md):
- `pixel_done` raises ONE SW_EVENT per render instead of running the WAIT_3D/WAIT_FREE
  rendezvous (both markers are `st.a.d`, which drop on the 530, so the rendezvous
  can't work and the handler would otherwise exit early on SC_INFLIGHT=0 and complete
  nothing). The host's SW_EVENT IRQ then signals the oldest job's dma_fence and
  advances the CCB read offset.
- `free_done` is unchanged: on the 530 it naturally acks 0x1 and ends
  (SC_INFLIGHT=0 → render_done_noop), so there is exactly one SW_EVENT per render.

Everything else is identical to the sgx530 image: the init body's dropped stores are
harmless no-ops (the host zero-inits the control block and sets ui32InitStatus), and
the kick path (do_ta/ta_finished/do_3d) is command-driven and works as-is. The host
owns the CCB read offset (advances it on the SW_EVENT IRQ), the firmware structures,
and completion. Launched by the driver's §9 bare-DOUTU event program at offset 0 with
r17=ctlblk patched, exactly like the plain sgx530 image."""
from .sgx530 import *  # noqa: F401,F403
EDM_HOST_MEMORY = True
