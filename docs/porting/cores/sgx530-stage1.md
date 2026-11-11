# SGX530 stage 1: the first microkernel start, runbook

Stage 1 of the SGX530 bring-up covers stages 1 and 2 of [microkernel.md](../microkernel.md)
(the processor boots our code; init without the vendor srvinit) on our OMAP3630 test
board: powervr_sgx loads the open microkernel, kicks init and checks that it answers. It is one attempt, run
by the kernel session through the board owner. This page says what goes on the board,
what to read back, what each result means and when to stop. The facts behind it are in
[sgx530.md](sgx530.md) ("The open microkernel's SGX530 build") and the host side is in
[single-context.md](../single-context.md).

## Before the attempt

- **The event-program registers.** powervr_sgx uses the SGX545's offsets (0xa58 enable,
  0xa68 program address, 0xa6c/0xa70 data). They are a guess (G): no GPL header names
  them for any core. Joseph decides whether anything confirms them before stage 1, and
  nothing here depends on that answer. Either way the driver's guard stays on: it reads the
  four before writing them, and it refuses to kick if the program address doesn't read
  back.
- **The board owner's OK** for the module load, at a time they choose. The board has no
  bench power plug, so a wedged GPU means a manual power cycle. Plan for one attempt.
- **The board's kernel.** Build the module for the kernel the board runs at the time
  (7.1.8 now, 7.3.3 after the move). It needs the board's config and compiler; see
  kernel.md and `drm/tools/modver-check.py`.

## What gets loaded

1. **The module**, from the head of the kernel session's branch: the SGX530 part entry,
   the guarded event-program start, and init-script part 2 limited to named registers.
   Load it with `allow_unisolated=1`. Without that parameter the probe stops after the ID
   read, before any write.
2. **The firmware**, as `powervr_sgx/sgx530_ukernel.bin` wherever the board's firmware
   loader looks (the board owner decides where):

   ```bash
   python3 ukernel/build.py sgx530 sgx530.bin sgx530.json
   python3 drm/tools/mkukfw.py sgx530.bin -o sgx530_ukernel.bin --manifest sgx530.json --core-rev 0x00010205
   python3 drm/tools/mkukfw.py --check sgx530_ukernel.bin
   ```

   `--check` must print features 0x4, image 0x42c0 crc 0x77f96757, core rev 0x10205,
   entry program 0 at 0x0, and patches 0x8 <- ta3dctl and 0x10 <- pdtable. The image
   matches `ukernel/golden/sgx530.sha256` (2d79b7f2…, 17088 B). It assembles to the same
   bytes through asm.py and through `sgx-asm -c sgx545`; the manifest records `"isa":
   "sgx545"`.

## What the start touches

**The host writes**, after the clocks are on: `CLKGATECTL`, `SOFT_RESET`, `BIF_CTRL`,
`DIR_LIST_BASE0`, `EVENT_HOST_CLEAR`/`CLEAR2`, `EVENT_HOST_ENABLE` (0x4000, SW_EVENT) and
`ENABLE2` (0), `USE_CODE_BASE` 0xa0c-0xa48, `PDS_EXEC_BASE`, `EVENT_KICKER`,
`EVENT_TIMER`, `EVENT_KICK`, and the four guessed event-program registers. All are named
in the SGX530's GPL header (K) except the last four.

**The microkernel touches** only these on init, the timer event, get-misc-info and the
idle PD restore: `CORE_ID` 0x10, `CORE_REVISION` 0x14, `EVENT_STATUS` 0x12c,
`EVENT_HOST_CLEAR` 0x134, `EVENT_TIMER` 0xacc, `BIF_CTRL` 0xc00 and `DIR_LIST_BASE0`
0xc84. It never writes `EVENT_HOST_ENABLE`/`ENABLE2`. `ukernel/test_sgx530.py` ("stage 1")
fails if that changes. Init itself writes only `EVENT_TIMER` (the period and enable bit
24). It raises no host interrupt, so stage 1 passes by polling even if the interrupt
doesn't reach the CPU.

## What to read back

The driver logs these on the first start:

| Line | What |
|---|---|
| `event PDS registers before any write: enable … base … data … …` | The four guessed registers after the clocks are on and before any write: their reset values, or whatever an unbacked address returns. |
| `event PDS base 0x0a68: wrote …, read …` | The program-address read-back after init-script part 2. |
| `microkernel: init … version … wait … timeouts … heartbeat …` | After a start: `INIT_STATUS` (host control word 0), and control-block words 0x18, 0x28, 0x29 and 0x2a. |

Then read the status words **a second time, at least a second later** (debugfs dump or a
second status line). The heartbeat is the one value that has to change between reads.
Also note interrupt 21's count in `/proc/interrupts` before and after the load.

## What each result means

**Pass:** the base reads back what was written; init 1; version 0x00010000; wait 0 and
timeouts 0; the heartbeat ≥ 1 and higher on the second read. That confirms (C) the four
event-program offsets, the event program entering the microkernel, `EVENT_KICK` starting
the init event, and the timer event reaching the event program. It is also coarse
evidence for the 18 instruction forms the path runs (sgx530.md, "What stage 1 runs").

| Result | Likely cause | What to do |
|---|---|---|
| `doesn't read back; not starting the microkernel` (-EIO) | 0xa68 isn't the program address on this core, or it holds fewer bits | Stop. Nothing was kicked and the events were never enabled. Record both values and the pre-write line; the offsets stay G. |
| The base reads back, init stays 0 (the probe fails after 500 ms) | The event program didn't run or didn't reach the microkernel: the enable register's layout, the kick, the entry program's data layout, or `USE_CODE_BASE_15` | Stop. Record the logged `EVENT_STATUS` and BIF fault registers. A BIF fault points at the PD or the heap layout; no fault and no event bit points at the event program. |
| Init 1, version not 0x00010000 | The ctlblk patch, or a different image | Check `--check` and the file's sha256 against the packaged one. |
| Init 1, timeouts > 0 | A status wait gave up. Nothing on the init and timer paths waits, so this is unexpected | Record `wait` (the last id) and look it up in `sgx530.json`'s `waits` map (register, bits, set or clear). |
| Init 1, heartbeat stuck at 1 | Init ran but no timer event reached the event program: `EVENT_TIMER`'s layout, the enable value's b29, or the period from `clock_hz` | Read `EVENT_TIMER` (0xacc, named) once: bit 24 should be set, with a nonzero period. Stage 1 isn't passed until the heartbeat moves. |
| The heartbeat moves very fast | The timer period is short for this clock | Not a failure. Record the rate for the `clock_hz` entry (gap 14). |
| Interrupt 21's count climbs fast | An event the host enabled keeps firing | Unload the module. Only SW_EVENT is enabled, and init raises none, so this is unexpected. |

## Abort conditions

Stop, unload the module if the board still answers, and hand back to the board owner if:

- the probe returns any error, including -EIO from the guard or a timeout;
- the kernel logs an oops, a BIF fault or a hung task, or the board stops answering;
- interrupt 21 storms.

Don't change a guessed offset and try again in the same session. A new guess is a new
attempt, agreed with the board owner. Don't sweep or read other unnamed registers to look
for the right ones.

## Afterwards

- Record the results in `docs/log/entries/` and mark what they confirm as C in sgx530.md:
  the event-program offsets and the timer path, and gap 1's 0x18/0x1c if read.
- If it passed: microkernel.md stage 3, the host kick and the CCB (get-misc-info, which
  also raises the first host interrupt; gap 3), then the first TA (stage 4). Instruction-level ISA tests come after the first frame, and so does the move to
  `sgx-asm -c 530`.
