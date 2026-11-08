# EA1: the post-init event program storms on an unacknowledged DCU-invalidate; the host kick is still not serviced

First M1 hardware session on the **EA1** (`ce5300-board`, kernel
7.1.8, CE5300 core rev 1.0.14), after GPU testing moved off the EA3. `srvinit
-o` with the option-B microkernel. Two findings, both hardware-confirmed: a
real post-init bug (fixed this session), and the precise shape of the blocker
that remains.

## Option B init: re-confirmed on the EA1

Independently of the EA3, `srvinit -o` completes init: the advisory version
word appears at the TA/3D control block +0x58 (dump `T3C-50` word 2 =
`0x00010000`). So the r17-derived control-block addressing works here too.

## Getting the srvinit window on the EA1 (init-model note)

The EA1 is not a minimal rig: it auto-initialises SGX during boot. On a cold
boot `/proc/pvr/version` reads `System Version String: None`; the first GPU
client opens `/dev/dri/card0` and the kernel runs the full init-server
sequence (`SysFinalise`/`SGXInit` at ~t=52s), after which `srvinit`'s
`INITSRV_CONNECT` returns **129 = PVRSRV_ERROR_SRV_CONNECT_FAILED**
(`bridged_pvr_bridge.c` `PVRSRVInitSrvConnectBW`: fails when
`PVRSRV_INIT_SERVER_RAN` is already set). **Two** boot scripts run
`/opt/sgx-i386/run.sh sgxinit` — `S60sgx` *and* `S95ohc-display` (line 46) —
so both must be held off to leave the srvinit window open. With both held off,
`/proc/pvr/version` stays `None` through a full boot and `srvinit -o`'s
`INITSRV_CONNECT` succeeds. (No persistent change: the scripts are renamed
aside for the test and restored, then the box is rebooted to its normal stack.)

## Diagnostic sentinels (debug build, not committed)

Built with `UK_SENTINEL=1` (an env-gated branch, reverted before commit),
writing to free control-block words that appear in the kernel's SGX debug dump
as `T3C-E0..EC`:

| word | sentinel | what it records |
|---|---|---|
| +0xE0 | CTR | event-program entries (incremented at the top, before the init check) |
| +0xE4 | INITVAL | the `ui32InitStatus` seen at the top each entry (0 = init pass, 1 = re-entry) |
| +0xE8 | POST | `0xB0B0` when the post-init dispatch path is reached |
| +0xEC | KICK | `0xC0C0` when the `kick` handler body is reached |

## Run 1 (no fix): the event program is re-entered, but never dispatches the kick

`T3C-E0`: CTR `0x10`→`0x12`, INITVAL `1`, POST `0xB0B0`, **KICK `0`**.
`EUR_CR_EVENT_STATUS = 0` (`sw_event: no`), **`EUR_CR_EVENT_STATUS2 = 0x400`
(`dcu_invalid: yes`)**. Kernel CCB `WO:0x8 RO:0x7` (one command queued, waiting);
`SGXOSTimer detected SGX lockup (0x2 tasks)` → repeated HWR.

So — revising the EA3 "init exit wedges the core, never re-entered" reading —
the event program **is** re-entered after init and **does** reach the post-init
dispatch (POST set). The storm source is **`dcu_invalid` (EUR_CR_EVENT_STATUS2
bit 0x400)**: a data-cache-unit invalidate the kernel leaves pending after
`SysFinalise`. the open microkernel's dispatch only read `EUR_CR_EVENT_STATUS` (0x12C) and
dispatched four STATUS bits; it never read `STATUS2` (0x118) or acknowledged
`dcu_invalid`, so the event re-triggered and the SW_EVENT host kick was
starved. (The offline emulator models neither STATUS2 nor event re-entry, so
`test_emu`/`run_ours` — all 5 traces, 12/12 — still MATCH.)

## Fix (committed): acknowledge dcu_invalid in the dispatch

docs/UKERNEL.md §2 already says the dispatcher reads STATUS **and** STATUS2 and
clears what it sees. The dispatch now writes `DCU_INVALID` (0x400) to
`EUR_CR_EVENT_HOST_CLEAR2` (0x114) before the STATUS dispatch. Unconditional —
clearing a bit that is not set is a no-op and no handler reads it. Offline:
test_emu 5/5, run_ours 5/5 MATCH (no regression).

## Run 2 (with the fix): dcu_invalid cleared, but the kick is still not serviced

`EUR_CR_EVENT_STATUS2 = 0` / `dcu_invalid: no` — **the fix works**. But the run
still fails the same way: `T3C-E0` CTR `0x0C`→`0x12`, POST `0xB0B0`, **KICK `0`**;
`EUR_CR_EVENT_STATUS = 0` too; CCB `WO` one ahead of `RO`; `0x2 tasks` lockup →
HWR. `srvinit` ends `initsrv disconnect failed: 9`, rc=1.

With **both** EVENT_STATUS and STATUS2 now `0` at every lockup, the CTR climb is
**not** a within-microkernel storm — it is the kernel's HWR-and-retry loop:
each retry re-launches the event program once (CTR+1, reaches POST), finds no
pending event, ends; the GETMISCINFO command is never consumed; SGXOSTimer
times out; HWR; repeat.

## The remaining blocker, precisely localised

**The host GETMISCINFO kick never reaches our dispatch as SW_EVENT.** The event
program runs and reaches the dispatch (POST set), but `EUR_CR_EVENT_STATUS &
0x4000` is never true when it reads it (KICK = 0, `sw_event: no`), so it never
branches to `kick`. The kernel has queued the command (CCB `WO` > `RO`) and is
polling for the misc-info reply, which never comes.

So the next question is **how this EA1 kernel delivers the host kick to the
microkernel** and why it does not show up as SW_EVENT in EUR_CR_EVENT_STATUS:
- the kernel may kick by a path other than setting SW_EVENT in EVENT_STATUS
  (e.g. it writes the kernel CCB and expects the microkernel to poll it, or it
  uses a different event bit / a kick register), or
- srvinit's `-o` event-program / host-kick-table / SW_EVENT-enable wiring may
  differ in a way that means the kick does not set the bit our B_PDS0 reads.

Chase it offline against the kernel's `SGXGetMiscInfoUkernel`/kick path and
srvinit's `pds_ours`/`kick_ours`/host-enable setup, then confirm on HW. This is
microkernel/srvinit-side; the kernel-driver init model is understood.

## Box / method notes

- Normal p1 cold boot; `srvinit -o` the first SGX touch after holding off
  `S60sgx` + `S95ohc-display`; `/boot`, `S00kexec`, `S00aramboot` untouched; no
  test kernel, no kexec, no flash. The EA1 has no smart plug — each cold GPU was
  a plain `reboot` (reliable here across ~5 cycles this session), never a power
  cut; on a box hang the rule was stop + message, not retry.
- `flock`-style `/tmp/gpu.lock` owner marker held for the window; mesa-gallium
  idle throughout; box restored to its normal UI stack (both scripts re-enabled,
  rebooted, verified `/proc/pvr/version` = 1.0.14 + WPE webview up) and the
  staged binaries removed afterwards.
- Dumps: `docs/log/data/2026-11-02-2015-ea1-ob-sentinel.dmesg.log` (run 1),
  `docs/log/data/2026-11-02-2015-ea1-dcufix.dmesg.log` (run 2).
