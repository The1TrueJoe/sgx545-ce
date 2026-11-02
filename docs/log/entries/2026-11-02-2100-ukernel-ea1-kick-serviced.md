# EA1: the host kick is serviced — srvinit completes (rc=0), the DDK-version check passes

Hardware confirmation of the host-kick work on the EA1 (`srvinit -o`, cold boot,
both `S60sgx` and `S95ohc-display` held off). Two fixes landed this slot; the
primary post-init blocker is **resolved on hardware**.

## Result

```
srvinit: SGX545 microkernel started (ukernel at 0x30000000, pd 0x01d69000)
rc=0
```

`srvinit` reaches the success path for the first time — `INITSRV_DISCONNECT`
returns 0 (it was `failed: 9` in every earlier run). That call is where the
kernel resets the core, starts the microkernel, and runs the `GETMISCINFO`
DDK-version validate, so rc=0 means **the host kick was serviced and the version
check passed**. The kernel's SGX debug dump shows **Kernel CCB `WO == RO`** at
every sample (…0x19/0x19, 0x1A/0x1A, … 0xA6/0xA6) — the microkernel is
**consuming** host commands and advancing the read offset, where before the fix
`RO` lagged `WO` forever.

## The two fixes

1. **Host-kick detection** (committed earlier, `71b150f`): the host kicks via the
   `EUR_CR_EVENT_KICK` doorbell (no status bit), detected by the kernel CCB
   write ≠ read offset — not `SW_EVENT`. Dispatch falls through to `kick`.
2. **Control-block layout** (this entry): `SA_KCCB` was `16` (word 16, +0x40) but
   the kernel CCB base lives at word **0x16** (+0x58) — where `srvinit` writes it
   (`srvinit.c` `t[0x16] = va(B_CCB)`) and the DDK model keeps it
   (`tools/ukernel/sgxsim.py`, +0x58). And `SC_VERSION` was `0x16`, so `init`
   overwrote that very base with the advisory version word. So the kick read the
   command from a garbage base (+0x40, unset), got a bogus type, branched to
   `do_ta`, kicked a bogus TA, and the core wedged (`0x2 tasks`). Fix: `SA_KCCB =
   0x16`; `SC_VERSION = 0x18` (+0x60, a free slot).

### How the sentinels pinned it

A debug build (env-gated, reverted before commit) wrote markers to +0xDC..+0xEC:
after fix #1, `KICK` = `0xC0C0` (kick handler reached), the `WO`/`RO` the
microkernel read matched the kernel's (`7`/`6`), and the past-empty marker
`0xDDDD` was set — so the kick *was* entered and found a command, but the core
still wedged. That isolated the fault to *after* the empty-check: the command
read, i.e. the kernel-CCB-base slot — fix #2.

### Why offline missed both

`test_emu`, `test_commands`, `run_ours` drove the kick with `SW_EVENT` set *and*
`WO≠RO`, and placed the kernel CCB base at word 16 — mirroring both bugs, so they
passed. All three now model the hardware: kick by `WO≠RO` only (no `SW_EVENT`),
CCB base at word 0x16. Green: `test_emu` 5/5, `test_commands` get-misc-info
21/21 (+ power/cleanup/ctxsuspend), `run_ours` 5/5 MATCH.

## Residual (next): SGXOSTimer "0x2 tasks" HWR on the idle microkernel

After `srvinit` exits, with no GPU client following (the display stack was held
off), a periodic `SGXOSTimer() detected SGX lockup (0x2 tasks)` + HWR recurs
(~every 0.5 s). Host commands are still consumed each cycle (`WO == RO` keeps
advancing, to 0xA6+), so this is **not** the kick blocker — it is a post-init
idle-stability issue: the SGXOSTimer's lockup check (it samples the TA/3D
**signature** registers `B_TASIG`/`B_3DSIG` that `srvinit` seeds, and the task
count) fires on an idle microkernel that is not updating whatever heartbeat it
watches, or is leaving two tasks un-retired. Chase next: what the SGXOSTimer
reads to decide "0x2 tasks", and whether the microkernel must maintain the
signature registers / a power-idle handshake when no work is queued. Likely
interacts with the `k_power` / active-power-management path.

## Box / method

Cold boots by plain `reboot` (EA1 has no plug), both init scripts held off for
the srvinit window and restored after; `/boot`, `S00kexec` untouched; no flash.
Box handed back on its normal install (both scripts re-enabled, `/proc/pvr` =
1.0.14, WPE webview up), staged binaries removed, `/tmp/gpu.lock` released,
mesa-gallium told it's free. Evidence:
`docs/log/data/2026-11-02-2100-ea1-kick-serviced.dmesg.log`.
