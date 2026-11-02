# Render teardown + in-flight bookkeeping: the DDK's full completion write-set

`render_done` now reproduces the DDK's complete render teardown, not just the
dependency sync. `run_ours.py`'s `[done]` check went from comparing only the
sync-heap writes ("ours 0, subset of the DDK's 2") to diffing **every** memory
write the DDK makes during its `3D end of render` + `DPM 3D memory free`
phases: `DDK 12 completion mem writes, ours 12`, no MISSING/DIFF/EXTRA, on all
three generated frames (320×240, 720×480, 1024×768). The TA/3D register match
(41 + 61) is untouched.

## What a completion does now

The render has drained, so the handler:

- **clears the destination sync objects.** The render details hold the render's
  `SGXMKIF_HWDEVICE_SYNC_LIST` destinations at words 0x34 and 0x90 (a colour
  and a depth target for a one-context frame); each one's ops-pending word is
  zeroed. These are the two sync-heap writes the `[done]` check was missing.
- **frees the render slot and RT.** render-details state (+0x2a4) → freed (2),
  active flag (+0x12c) and write-ops (+0x108) cleared; RT-data entry released
  (render back-pointer +0x4 → 0, state +0x34 → idle 0x10, completion word → 1).
- **empties the in-flight markers.** The context's in-flight render words
  (+0x28, +0x2c, +0x3c) and the control block's in-flight slot (+0x50) clear.
  `do_ta` sets the slot/RT markers at kick and `do_3d` sets the context
  markers when the 3D starts, so the teardown is the exact reverse.

All values and offsets were read from the DDK's own completion writes (the
`--why`/phase instrumentation over a frame), and are stable across the three
frame geometries — i.e. structure-relative or genuine constants, not
frame-specific.

## One context = one in-flight slot

The DDK keeps a doubly-linked render-in-flight list for concurrent contexts.
One render context runs at a time and completes before the next kick, so ours
collapses that to the single control-block slot at +0x50, cleared rather than
unlinked. It sits at the DDK's own field offset, so the DDK microkernel stays
a drop-in A/B reference (`docs/UKERNEL-DECISIONS.md`), and the cleanup/power
handlers read the same slot to tell whether a render is running — which is how
the teardown "completes their internals": the shared in-flight state now
exists for them to consult, and in the serial model it is always clear by the
time a power/cleanup command is processed.

## Validation rig rebuilt offline

The DDK firmware and traces were lost in the earlier `/private/tmp` cleanup.
Re-fetched the firmware with `srvinit/fetch-firmware.sh` (sha256 verified), and
— since the Linux `frame-fake` can't run on this arm64 Mac and no user-mode
qemu is available — built a **native arm64 `frame-fake`** by defining the five
`IMG_*` macros' OS branch (`-D__linux__`) and a one-line local `<linux/ioctl.h>`
shim (the ioctl numbers are faked anyway); arm64-macOS and x86_64-Linux are
both LP64, so the bridge struct layouts match. It generates the same traces
`run_ours.py`/`run.py` consume. This is a local tool build only — nothing in
it is committed, and the shim/`-D` live in the build command, not the tree.

## Checks

- `run_ours.py gen/{fake,v1,v2}.trace --ddk FW` — MATCH, `[done] 12/12`.
- `test_emu.py` — init, recovery, raise-irq, and a new no-trace
  `render teardown` assertion (sync objects cleared, render/RT/context slot
  freed) all OK.
- `test_commands.py FW` — get-misc-info/power/cleanup/ctxsuspend all OK.

Transfer (the 3D-only render) stays dispatched-but-stubbed: its `TRANSFERCMD`
isn't populated in any frame `frame-fake` produces, so it can't be
register-validated yet (asked tex-rt-present for a transfer capture).
