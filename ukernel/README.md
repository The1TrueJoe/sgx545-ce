# ukernel: an open SGX545 microkernel

The open replacement for Imagination's USSE microkernel (the "EDM"). Written
from the behavioural spec in `docs/UKERNEL.md` — what the DDK microkernel
*does*, reproduced in our own code and layout — never translated from its
bytes.

| file | what |
|---|---|
| `ukernel.py` | the microkernel in USSE assembly (entries as labels; register-programming tables = docs/UKERNEL.md §7) |
| `build.py` | assembles it with `tools/isa/asm.py` into the binary srvinit loads, plus a JSON manifest of entry byte-offsets |
| `test_emu.py` | drives each entry through the offline emulator (`tools/ukernel/`) and asserts docs/UKERNEL.md's behaviour; no GPU, no trace |
| `run_ours.py` | runs ours through a whole frame and **diffs the core registers it programs against the DDK's**, per phase |
| `build.sh` | build + test |

    ukernel/build.sh                                    # build + the no-dependency tests
    python3 ukernel/run_ours.py TRACE --ddk DDK_FW      # frame-level match vs the DDK

`TRACE` is a pvrtrace log (a DDK capture, or a `tools/gen` `frame-fake -t`
run, which builds on any machine). `srvinit` will grow a mode that loads this
image and takes its entry offsets from the manifest, with the host-kick table
set to the identity (so a command's `ui32ServiceAddress` is its type);
`docs/SRVINIT.md` covers the DDK build's fixed constants ours replaces. The
build outputs are generated, not committed.

## State

Implemented and validated offline (no hardware):

- **event entry** — the one task the bare-DOUTU event program starts. It loads
  the control-block base from a loader-patched limm, populates the four `sa`
  the handlers use (self / host-control / CCB-control / kernel-CCB), and
  dispatches on `ui32InitStatus` and `EUR_CR_EVENT_STATUS` (host kick → kick,
  TA finished → ta_finished, 3D end → render_done). Every handler is a branch
  target of this one task;
- **init** — reached from the event entry (sa populated). Clean boot sets
  `ui32InitStatus`; **recovery** re-init (the host control block's HWR bit set)
  first discards the surviving kernel CCB (read offset := write offset) and
  empties the render-in-flight list (docs/UKERNEL.md §5a — the double-lockup fix);
- **host-kick dispatch** — reads the kernel CCB command, dispatches on its
  type (our host-kick table is the identity);
- **TA kick** — derives the TA command, render details, PB descriptor and RT
  entry from the context and the client CCB, points the BIF at the context's
  page directory, invalidates the caches and **waits** for the done bits,
  programs the parameter-buffer registers, loads the DPM state/control and
  **waits** for the free list, programs the TE/macrotile/screen and RT
  registers (41 struct-sourced), then starts the TA;
- **TA finished** — walks the TA command's status-value list (count at
  command+0x5c, `{address, value}` pairs from +0x118) and writes each value to
  its sync-heap address (the counters the host and next client wait on);
- **3D start** — programs the ISP/PBE 3D state from the render details (61
  struct-sourced registers), invalidates the 3D-side caches and **waits**,
  then starts the 3D;
- **render done** — the TA3D destination sync update (`*complete = pending+1`
  for the sync the host/replay polls on) and the write-ops reset, then the
  host-interrupt tail (`SW_EVENT`).

`run_ours.py` confirms ours programs **every** struct-sourced core register
with the same value as the DDK, per phase, for four frames (the reference DDK
capture and three `frame-fake` scenes: default, a 320×200 clear+colour, and a
depth test), and that the TA status-value writes to the sync heap match the
DDK's (7 on the reference capture).

Not yet reproduced (the next increments, each needs its own validation): the rest
of the render teardown the DDK does
at 3D-end (its other sync-category list walks, the parameter-buffer free-list
return, the render-in-flight list and context refcount — these need the
control-block bookkeeping a multi-render scheduler keeps; ours does the one
destination-sync write a single replayed frame is polled on), transfers,
cleanup, power, and the `srvinit --ours` loader. Nothing runs on hardware until
a full frame's completion path is reproduced offline and the box + recovery
method are chosen.

## Building

One command, no external dependencies — only CPython (3.7 or newer; dict
insertion order, which the manifest relies on, is guaranteed from 3.7):

    ukernel/build.sh                 # assemble + run the no-GPU emulator tests

or just the assemble step:

    python3 ukernel/build.py [OUT.bin] [MANIFEST.json]

The build is **reproducible**: the assembler is pure Python with no timestamps,
host paths or randomness, so the same source produces byte-identical `.bin` and
`.json` on any machine and any supported Python. `UK_BUILD_ID` sets the manifest
`build_id` (default `dev`); set it to the commit for a release. The outputs
(`<core>_ukernel.bin`, its `.bin.pat`, and the `.json` manifest) are
generated, not committed.

## Per-core images

The microkernel programs the SGX core registers for one specific SGX5xx variant
(the register-programming tables are docs/UKERNEL.md §7). Each core is a config
module, `cores/<core>.py`, selected by `build.py <core>` (default `sgx545`, the GPU
in the Intel Atom CE5300 and Cedarview); its `CORE_NAME` names the output binary
and the manifest `core` field, so images never collide. Where a core needs other
handler logic, `ukernel.py` has a per-core feature flag (`MULTI_MEM_CONTEXTS`,
`FIX_HW_BRN_28889`, `WAIT_BOUND`, `HEARTBEAT`, `INIT_DPM_CLEAR`), never a fork; the SGX545 defaults
emit the code the SGX545 always had.
`ISA_CORE` names whose instruction encodings `sgx-asm -c` uses. It is 545 for every
core until sgx.xml describes another, and the manifest's `isa` says so when it differs.

- `sgx545`: the shipping core; golden `golden/sgx545.sha256`.
- `sgx530`: our OMAP3630 test board, being brought up
  (docs/porting/cores/sgx530.md, "The open microkernel's SGX530 build"): one
  memory context with the host's PD table (a second patch, `pdtable`), bounded
  waits and a heartbeat word. Checked offline by `test_sgx530.py`; golden
  `golden/sgx530.sha256`.

## Feature bits

The manifest's `features` word (and the image header a packaging tool writes
from it) declares the capabilities the loader must assume. Bits:

| bit | name | meaning |
|---|---|---|
| 0 (0x1) | `CLEANUP_V2` | the DDK's two-phase resource cleanup. **This image does NOT set it** — this microkernel uses a simpler defer-and-hold cleanup, so the loader must not expect the v2 protocol. |
| 1 (0x2) | `SECURE` | the background-object PDS block is written by the host (CPU) at submit, not by the microkernel's event-driven module, so the loader can isolate each client address space from the firmware's own heaps instead of mapping them into every context. Set by default (`UK_FEATURES`, default 2). |
| 2 (0x4) | `SINGLE_CONTEXT` | (number proposed, with the kernel side) one memory context: the EDM runs on list 0, the job's PD comes from the host's read-only PD table, the firmware PD is restored before any completion, init on `EVENT_KICK`. The SGX530 image's default (and it does not set `SECURE`). |

A loader keys its behaviour on these bits, not on the build id, so an image can
state exactly which protocol it implements. The upstream DRM driver loads only
images that declare `SECURE` (bit 1) unless a compat config is set, so a
feature-0 image is a dev/test-only build; `UK_FEATURES` defaults to the core's
`FEATURES` (2 for the SGX545), so the default build is the upstream-loadable one.

## Manifest fields

`build.py` writes a JSON manifest beside the binary:

| field | meaning |
|---|---|
| `core` | the SGX core this image targets (see Per-core images) |
| `version` | microkernel interface version, `major.minor` |
| `features` | the feature-bit word (see Feature bits) |
| `build_id` | a free-form build identifier (`UK_BUILD_ID`, default `dev`) |
| `bytes`, `instructions` | image size and instruction count |
| `entries` | byte offset of each entry/label the loader points its PDS programs and host-kick table at |
| `patches` | byte offset of each `limm` site the loader fills at load (e.g. `ctlblk`, the control-block base) |

## Licence

MIT (`LICENSE`); every source file carries an `SPDX-License-Identifier: MIT`
header.
