# Command set: get-misc-info (DDK-exact), power, cleanup, context suspend

The kick handler now dispatches on the command type (our host-kick table is
the identity, so a command's ServiceAddress is its type) and handles more of
the SGXMKIF command set. Offline, against the DDK (`ukernel/test_commands.py`).

## get-misc-info — matches the DDK word for word

This is the one that failed the EA3 bring-up: `SGXFinalise` calls
get-misc-info to validate the microkernel's version, and ours never ran it, so
`SGXInit: Unable to validate device DDK version`. The handler now writes the
core identity (`EUR_CR_CORE_ID`/`CORE_REVISION`), the SGXMKIF version/feature
words and `ui32ClientBuildOptions`, and — when the request asks for them — the
`SGX_MISCINFO_STRUCT_SIZES`, then sets the READY flag. Driven through the event
entry with a type-6 command, all 21 misc-buffer words equal the DDK's
(`misc[4] = 0xd2aaa = 862890`, the 1.7.862890 build). So the kernel's
version check will pass with ours, as the SGXMKIF-compat decision requires.

## power, cleanup, context suspend — the host contract

These satisfy the host-visible SGXMKIF contract the driver polls:

- **power** reports completion in `ui32PowerStatus` (POWEROFF → bit 3, IDLE →
  bit 2), which `sgxpower.c` waits for;
- **cleanup** sets `PVRSRV_USSE_EDM_CLEANUPCMD_COMPLETE` in
  `ui32CleanupStatus`, which `sgxutils.c` waits for;
- **context suspend/resume** sets/clears the context's suspend flag.

The DDK additionally drives its power-management state machine and unlinks
cleaned-up resources from the microkernel's render lists only when safe; that
list surgery is the multi-render-bookkeeping item still to come, so these
handlers are the host contract now and grow their internals with it. Transfer
(a 3D-only render) is dispatched but stubbed, pending its handler.

`ukernel/test_commands.py DDK_FW` checks all four; `run_ours.py` (the TA/3D
frame) and `test_emu.py` (init + event dispatch) are unchanged and green.
