# CE5300 board: option B fixes the init stall (hardware-confirmed); the next blocker is the host kick

Second M1 hardware session on the CE5300 board (cold boot, sgx545ce driver, `srvinit
-o`). The option-B microkernel — no `mov sa`, the control-block base carried in
a reserved temp (r17) and every pointer derived from `*(r17+n)` — **completes
init on hardware**. The init stall from the 0630 session is gone.

## Option B init: confirmed

`srvinit -o uk-ob.bin` (6608-byte image, r17-derived pointers) on a clean p1
cold boot. The kernel's SGX debug dump after the run shows init ran to the end:

| evidence | value | meaning |
|---|---|---|
| Host control HC-0 | `0x00000001` | `ui32InitStatus` = INIT_COMPLETE (was `0x0` in the 0630 stall) |
| TA/3D control T3C-50 word 2 (ctlblk+0x58) | `0x00010000` | the advisory open-microkernel version word `init` writes last |
| HC-10/HC-20 | populated | host-control block filled by the microkernel |

So in the bare-`DOUTU` event context, r17 is allocated and usable, and the
handlers' `ld.a.d r17,#n` pointer derivation works on hardware. The whole
premise of option B holds. (Reproduced across two cold boots; identical.)

## The new blocker: the host kick is never serviced

Init completes, then the kernel kicks `GETMISCINFO` for its DDK-version compat
check and the run fails there:

```
SGXOSTimer() detected SGX lockup (0x2 tasks)
HWRecoveryResetSGX: SGX Hardware Recovery triggered
...
SGX Kernel CCB WO:0x1 RO:0x0
SGXGetMiscInfoUkernel: Timeout occurred waiting for misc info.
(FAIL) SGXInit: Unable to validate device DDK version
```

`srvinit`'s `INITSRV_DISCONNECT` returns 9 and it exits rc=1.

The decisive datum is **Kernel CCB `WO:0x1 RO:0x0`**: the host queued one command
(write offset 1) and the microkernel's read offset never advanced, so `kick_adv`
never ran — the command was never processed. The core locks up ~210 ms after
`SysFinalise` with `EUR_CR_EVENT_STATUS2` `dcu_invalid` set, in both runs,
identically.

### Localized with sentinels

Three sentinels were placed on the post-init path (debug build, not committed),
writing markers to free control-block words T3C-C0/C4/C8:

| sentinel | where | fired? |
|---|---|---|
| A | event entry, post-init path (after the `(p0) ba init` check) | **no** |
| B | the `EUR_CR_EVENT_STATUS` value the event program reads | **no** |
| C | the `kick` handler | **no** |

All three stayed `0x00000000`. So after the first init task ends, the microkernel
**never reaches the post-init dispatch section at all**. The event program is not
re-entered to service the SW_EVENT host kick (or is looping back into `init`).
The blocker is upstream of the kick handler body — not in `k_getmiscinfo`.

This is the same class of blind spot as the sa bug: the offline emulator runs one
task to completion and models neither event re-entry nor the core's lockup, so it
cannot see this. `test_emu`, `test_commands` and `run_ours` (all five traces,
12/12 completion writes) still MATCH the DDK.

## Change made this session

- **`kick` now acknowledges SW_EVENT** (`str` 0x4000 to `EUR_CR_EVENT_HOST_CLEAR`)
  as its first action, the acknowledge docs/UKERNEL.md §2 already specifies ("the
  dispatcher writes the matching bit back to acknowledge it before handling it").
  It was missing. Offline-validated (no regression). It is **necessary but not
  sufficient** for this run: the HW blocker is upstream (the event program is not
  re-entered), so the acknowledge is not exercised yet. The matching acknowledge
  in `ta_finished` / `render_done` is still to add, validated against the DDK
  render trace when frames run.

## srvinit event setup is ruled out

The init-script setup is **identical for `-o` and `-f`** (srvinit.c `build()`,
the common block after the `if (s->ours)` branch): the event PDS program address
`0xa68 = B_PDS0`, control `0xa6c = 1`, and the SW_EVENT host-enable
`EUR_CR_EVENT_HOST_ENABLE = 0x4000`. Only the PDS program *contents* (our bare
DOUTU vs the DDK's) and the host-kick table differ. So for `-o` the event program
is armed and SW_EVENT is enabled exactly as for the working DDK path — the
re-trigger is set up. The blocker is therefore in the **microkernel's own
behavior after init**, not srvinit registration: the open microkernel's init task ends but the
core does not return to the event-wait state to re-run B_PDS0 on the next
SW_EVENT (it locks up with 2 tasks pending, `dcu_invalid`). This implicates the
init task's exit / task-phase handling (the `phas.i`/`nop [end]` wrapper, or
something the DDK's init task does at exit that the open microkernel omits).

## Next step (for the next CE5300 board slot)

Add a sentinel at the very top of the event entry (before the init check): does
B_PDS0 re-run at all on the next SW_EVENT? If it never fires, init's exit left the
core wedged (the 2-task lockup) and the fix is in the init task's phase/exit
sequence — compare the DDK init task's exit (its disasm/trace) against the open microkernel's
`phas()`/`end()`. If it fires but branches back into `init`, the post-init
InitStatus read is wrong in the re-entered context. Either way the work is in the
microkernel, not srvinit.

## Box / method notes

- Normal p1 cold boot (sgx545ce present), `srvinit` in userspace; no test kernel
  RAM-booted, no kexec, `/boot` / `S00kexec` / `S00aramboot` untouched. Bench
  power-cycle between every attempt for a clean cold GPU; `flock /tmp/gpu.lock`,
  `sync` in the run script. Box handed back to the mesa-gallium session on a clean
  cold boot afterwards.
- `SGX_DEVINITPART2` logs one `invalid param size ... out 16/8`; the 16-byte retry
  succeeds (handled in `srvinit`), not the blocker.
