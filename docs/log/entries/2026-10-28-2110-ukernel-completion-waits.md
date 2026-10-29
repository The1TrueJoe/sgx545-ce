# The open microkernel waits for the cores

Follow-up to `2026-10-28-2040-ukernel-render-done.md`. The open microkernel
now waits for the completion events that gate a kick, instead of pulsing the
request and racing on. This is the last correctness piece before hardware: on
real silicon, kicking the TA before the parameter-buffer free list is loaded,
or kicking either core before its caches are invalidated, faults.

## What it does (ukernel/ukernel.py)

Two helpers, matching the DDK's sequences (docs/UKERNEL.md §6):

- `invalidate(pds_inv, done_mask)`: pulse the PDS, coefficient-store, MADD and
  USE cache-invalidate requests, then spin on `0x138` until the side's done
  bits are set (`0x27` TA, `0x4b` 3D) and acknowledge them, and spin on
  `EVENT_STATUS` for the MADD-cache done bit. Run after pointing the BIF at the
  context's page directory (TA) and before the 3D kick.
- `dpm_load()`: write the DPM state and control load, spin on `EVENT_STATUS`
  for `0x240`, acknowledge, then clear the OTPM (`0x24c = 9`) and wait for its
  two done bits. Run after the parameter-buffer registers and before the TA
  kick, so the free list is ready.

Each wait is a real spin loop — `ldr` the event register, mask, `test` and a
predicated branch back — that the emulator's register model terminates by
presenting the done bits a working GPU would (`tools/ukernel/sgxsim.py`,
`install_hw_model`).

## Validation

The microkernel runs the whole frame through the emulator with the waits in
place and still matches the DDK: 41 TA + 61 3D struct-sourced registers, the 7
TA status-value writes, and the completion sync write, on the reference
capture and the three `frame-fake` scenes. `ukernel/test_emu.py` stays green.

## State: a single frame is complete offline

Bring-up, the TA kick (with cache-invalidate and DPM-load waits), the TA
status writes, the 3D kick (with its invalidate wait), and completion (the
destination sync update and host interrupt) are all reproduced and
register/sync-identical to the DDK for a single replayed frame. What remains
is the `srvinit --ours` loader (so the box starts our image), and — for
multi-frame/Minecraft, not a single frame — the fuller 3D teardown. Then the
EA3, step by step. No hardware has been touched.
