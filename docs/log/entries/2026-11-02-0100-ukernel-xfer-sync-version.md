# Transfer destination sync + advisory version word (offline gap work)

Two of the gaps listed in UKERNEL.md §10, closed offline while waiting for the
EA3 hardware slot.

## Transfer destination sync

A transfer's client waits on a destination sync object; until now `xfer_done`
only advanced the CCB and raised the host IRQ, so the client saw the queue move
but never the blit's completion. The completion was read from a real transfer
capture (a GLES texture upload and the EGL swap): the destination sync's
write-ops-complete **address** is in the shared command at word 0x168 and the
**value** at word 0x16c (`SGXMKIF_TRANSFERCMD_SHARED`); completion writes the
value to that address. The handler now does that, and also clears the transfer
context's in-flight fields (+0x14, +0xb0) as the completion does.

Still open: a transfer also updates two sync-heap objects whose addresses come
from the kick's sync-handle list (not the shared command); those are not
reproduced yet. Flagged in §10.

## Advisory openHC version word

`init` now writes `0x00010000` (1.0) to control-block `+0x58`, per
UKERNEL-DECISIONS.md: advisory (the DDK driver ignores it), for an openHC-aware
kernel to log which build is running. The authoritative compatibility gate
stays the SGXMKIF build-options and struct-size checks.

## Checks

- `test_emu.py`: `init` now also asserts the version word; `transfer
  completion` now asserts the destination sync is signalled and the ctx
  in-flight fields clear. All 5 checks OK.
- `test_commands.py` OK; `run_ours.py` fake/v1/v2/xfer/xfer-rect still MATCH
  (the kick-phase registers are unchanged).
