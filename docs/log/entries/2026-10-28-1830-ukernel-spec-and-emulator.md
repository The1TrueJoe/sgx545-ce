# The microkernel, read and reproduced offline

The last Imagination binary in the stack was the 53 KB USSE microkernel that
`srvinit` uploads. This is the start of replacing it: the whole thing
disassembled and understood, an offline emulator that runs it against a frame,
a behavioural spec, and the first piece of an open replacement.

## Decoder

`tools/isa` gained the two flow-control forms the shader corpus never used —
`setl` / `savl` (special-class ops 3 and 4), which set and save the link
register. The microkernel brackets every nested call with them and jumps
through tables with `setl` + `lapc`. With these the entire microkernel
decodes with no unknown word and no unexplained bit.

## Emulator

`tools/ukernel/` is new. `emu.py` executes the microkernel's instruction
subset (moves, limm, bitwise, test, the integer multiply-adds, device
loads/stores, ldr/str to the register file, branches/calls/setl/savl, emit,
program end) against a sparse device-memory model and the core register file;
it stops on any instruction it has no rule for, so a run that completes is
real behaviour. `sgxsim.py` lays kernel data out as `srvinit` does, models the
five entry PDS programs and the registers the microkernel polls, and plays the
host side. `fromtrace.py` loads device memory, HW contexts and kick parameters
from a pvrtrace log — a DDK capture or a `tools/gen` `frame-fake` run (which
now builds natively on macOS). `run.py FW TRACE` runs a whole frame — init,
each TA kick, TA-finished, 3D end of render, the transfer and completion —
and prints every register access, store and task; `--why` tags each register
write with the exact structure word it came from.

The DDK microkernel runs the reference capture and a generated frame to
completion: init sets `ui32InitStatus`, the TA kicks program the TE/PB
registers from the TA command, the 3D start programs the ISP/PBE registers
from the render details, and the TA status and 3D sync words land where the
capture has them. Where each register comes from is in
`data/2026-10-28-ukernel/ddk-ref-frame-registers.txt`.

Rules learned on the way: repeated `ld`/`st` without post-increment address
`off+1 .. off+n` (from the microkernel's own writes); the invalidate-done bit
groups in `0x138` are 0x27 (TA) and 0x4b (3D); the microkernel raises the host
interrupt by writing `SW_EVENT` into `EVENT_STATUS` itself.

## Spec

`docs/UKERNEL.md`: the event dispatcher, the eleven CCB host-kick handlers
(TA, transfer, power, context-suspend, cleanup, misc-info, process-queues,
HWPerf, flush), the init handshake, the completion/sync path, the TA and 3D
register sequences with per-register provenance, and open questions. In our
own words, from behaviour — no listings, no bytes.

## Open microkernel

`ukernel/` is openHC's own microkernel, in USSE assembly, built by `asm.py`
into the binary srvinit will load plus a manifest of entry offsets. The first
version is the init handshake (sets `ui32InitStatus =
PVRSRV_USSE_EDM_INIT_COMPLETE`) and the host-interrupt tail; both pass in the
emulator (`ukernel/test_emu.py`). The CCB kick dispatch and the one-context
TA/3D kick are next, then transfers, cleanup, power and recovery — each
validated offline before any hardware run.

No hardware was touched.
