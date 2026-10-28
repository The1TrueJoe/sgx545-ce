# ukernel: openHC's own SGX545 microkernel

The open replacement for Imagination's USSE microkernel (the "EDM"). Written
from the behavioural spec in `docs/UKERNEL.md` — what the DDK microkernel
*does*, reproduced in our own code and layout — never translated from its
bytes.

| file | what |
|---|---|
| `ukernel.py` | the microkernel in USSE assembly (entries as labels) |
| `build.py` | assembles it with `tools/isa/asm.py` into the binary srvinit loads, plus a JSON manifest of entry byte-offsets |
| `test_emu.py` | drives each entry through the offline emulator (`tools/ukernel/`) and asserts docs/UKERNEL.md's behaviour; no GPU |
| `build.sh` | build + test |

    ukernel/build.sh                      # -> sgx545-ukernel-openhc.bin + .json, then tests

`srvinit` will grow a mode that loads this image and takes its entry offsets
from the manifest, instead of extracting the DDK's and using its fixed
constants (`docs/SRVINIT.md`). The build outputs are generated, not committed.

## State

First version: the **init handshake** (sets `ui32InitStatus =
PVRSRV_USSE_EDM_INIT_COMPLETE`, so the host's init poll succeeds) and the
host-interrupt tail (sets `SW_EVENT`), both passing in the emulator. Next:
the CCB host-kick dispatch and the TA/3D kick for one render context
(docs/UKERNEL.md §3, §6, §7), then transfers, cleanup, power and recovery.
Nothing runs on hardware until the open image reproduces a full frame offline
and the box + recovery method are chosen.
