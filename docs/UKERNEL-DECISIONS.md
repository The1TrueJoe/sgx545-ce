# Open microkernel: the decisions M1 asks to be recorded

From `AGENTS.md` §3.4/§3.5 and §5 M1. Recorded here so the DDK microkernel
stays a drop-in A/B reference and the firmware is submittable to
linux-firmware.

## License: MIT (proposed)

openHC's microkernel (`ukernel/`) is written from `docs/` — behaviour,
encodings and register meanings found by tracing and hardware tests
(`tools/isa/hwt_*`), never translated from Imagination's bytes or decompiled
code (the clean-room discipline in §3.6 and `ukernel/README.md`). It is
therefore ours to license permissively. **MIT** is proposed: it is the
simplest permissive license, is what linux-firmware and Mesa expect, and
places no obligation on redistributors (an ISO can ship the firmware blob
freely). The alternative, BSD-2/3-Clause, is equivalent for this purpose; MIT
is chosen for brevity. A `LICENSE` file goes in `ukernel/` and a `WHENCE`
entry names the built binary when it is submitted. *Joseph to confirm MIT vs
BSD.*

Nothing in `ukernel/` is copied or mechanically derived from `kernel/src/`
(GPL-2.0). The register and struct *knowledge* used by the microkernel comes
from `docs/`, which records how each fact was obtained; where a fact came from
static analysis of a blob it is confirmed by a hardware experiment before code
depends on it.

## Host interface: SGXMKIF-compatible, until the microkernel is complete

The microkernel keeps the **SGXMKIF** host interface (the kernel CCB of
`SGXMKIF_COMMAND`s, the control-block and host-control layouts, the
`SGX_BRIDGE_INIT_INFO` fields, the sync-object and status-value formats — all
in `kernel/src/.../sgx_mkif_km.h`) unchanged, so Imagination's DDK microkernel
remains a drop-in replacement for A/B testing throughout M1. srvinit loads
either one (`-f` the DDK's, `-o` ours) against the same kernel driver.

Only after the open microkernel is complete and validated on hardware does the
question of a simpler host interface for the new kernel driver (AGENTS.md open
question 4) get decided; changing it then is a deliberate firmware-ABI bump,
versioned (below).

## Versioned host interface: the kernel checks it at load

The microkernel reports its interface version so the kernel can refuse a
mismatch at load. SGXMKIF already carries two such checks, which ours honours:

- **`ui32ClientBuildOptions`** — the kernel compares it with its own
  `ui32KMBuildOptions` (srvinit passes `0x60218`); a mismatch is logged.
- **`sSGXStructSizes`** — the `SGX_MISCINFO_STRUCT_SIZES` the microkernel
  reports through `GETMISCINFO`, which the kernel checks against what srvinit
  declared; our get-misc-info handler must report the same sizes.

For ours we add one field of our own to the version surface: a small
**`openHC microkernel version`** word, written into a fixed control-block slot
at init and bumped whenever our host-visible behaviour changes. It is advisory
on the DDK driver (which does not read it) and lets an openHC-aware kernel log
exactly which build is running. The authoritative compatibility gate stays the
SGXMKIF build-options and struct-size checks, so the DDK stays a drop-in.

## Reproducible build, no DDK inputs

`ukernel/build.py` assembles the image from `ukernel/*.py` with
`tools/isa/asm.py` alone — no firmware fetch, no DDK library, no extraction.
The build is deterministic (same bytes every run). `srvinit -o` needs only the
built image and its `.pat` sidecar.
