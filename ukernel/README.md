# ukernel: openHC's own SGX545 microkernel

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

- **init** — clean boot sets `ui32InitStatus`; **recovery** re-init (the host
  control block's HWR bit set) first discards the surviving kernel CCB (read
  offset := write offset) and empties the render-in-flight list, so no stale,
  possibly-dead work is replayed (docs/UKERNEL.md §5a — the fix for the
  double-lockup wedge the replayer hit);
- **host-kick dispatch** — reads the kernel CCB command, dispatches on its
  type (our host-kick table is the identity);
- **TA kick** — derives the TA command, render details, PB descriptor and RT
  entry from the context and the client CCB, points the BIF at the context's
  page directory, and programs the parameter-buffer, TE/macrotile/screen and
  RT registers (41 struct-sourced registers) before starting the TA;
- **3D start** — programs the ISP/PBE 3D state from the render details (61
  struct-sourced registers) before starting the 3D;
- **completion** — the host-interrupt tail (`SW_EVENT`).

`run_ours.py` confirms ours programs **every** struct-sourced core register
with the same value as the DDK, per phase, for four frames: the reference DDK
capture and three `frame-fake` scenes (default, a 320×200 clear+colour, and a
depth test).

Not yet reproduced (the next increments, each needs its own validation): the
completion-event handshakes (cache-invalidate / DPM-load *waits* — ours pulses
the request without waiting yet), the TA status-value and 3D sync-object list
walks (docs/UKERNEL.md §5), transfers, cleanup, power, and the multi-render
scheduler the DDK keeps (ours is one context). Nothing runs on hardware until
a full frame's completion path is reproduced offline and the box + recovery
method are chosen.
