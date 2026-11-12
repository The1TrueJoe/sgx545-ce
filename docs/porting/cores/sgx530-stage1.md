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

**On a failed start, log the heartbeat too**, from the CPU mapping, before the arenas are
freed. It is memory, not a register. The event entry's first memory accesses, in order:
- the PDS fetches the entry program (0x18 bytes at the event-program address);
- its DOUTU starts the USSE at code base 15, segment 0;
- instruction 3 loads the heartbeat (control block +0xa8);
- instruction 6 stores it +1. This is the event's first write. `INIT_STATUS` follows at instruction 50.

So a heartbeat of 0 means nothing reached instruction 6: the PDS fetch, the DOUTU, or the
USSE's first fetch or load stalled. A heartbeat of 1 means the USSE ran, and the stall is
in init.

## What each result means

**Pass:** the base reads back what was written; init 1; version 0x00010000; wait 0 and
timeouts 0; the heartbeat ≥ 1 and higher on the second read. That confirms (C) the four
event-program offsets, the event program entering the microkernel, `EVENT_KICK` starting
the init event, and the timer event reaching the event program. It is also coarse
evidence for the 18 instruction forms the path runs (sgx530.md, "What stage 1 runs").

| Result | Likely cause | What to do |
|---|---|---|
| `doesn't read back; not starting the microkernel` (-EIO) | 0xa68 isn't the program address on this core, or it holds fewer bits | Stop. Nothing was kicked and the events were never enabled. Record both values and the pre-write line; the offsets stay G. |
| The base reads back, init stays 0 (the probe fails after 500 ms) | The event program didn't run or didn't reach the microkernel: the enable register's layout, the kick, the entry program's data layout, or `USE_CODE_BASE_15`. Or the microkernel's first instruction, `phas.i`, isn't valid on this core: the SGX530 lacks `USE_UNLIMITED_PHASES` (ISA.md, "Other cores: the SGX530") | Stop. Record the logged `EVENT_STATUS`, the BIF fault registers, `BIF_MEM_REQ_STAT` and the heartbeat. A BIF fault points at the PD or the heap layout. No fault and no event bit points at the event program. Reads outstanding with no fault point below the MMU: the table walk's own reads or the interconnect. The first attempt (log 2026-11-11-1532) gave this: 2 reads, no fault; the heartbeat wasn't logged. |
| Init 1, version not 0x00010000 | The ctlblk patch, or a different image | Check `--check` and the file's sha256 against the packaged one. |
| Init 1, timeouts > 0 | A status wait gave up. Nothing on the init and timer paths waits, so this is unexpected | Record `wait` (the last id) and look it up in `sgx530.json`'s `waits` map (register, bits, set or clear). |
| Init 1, heartbeat stuck at 1 | Init ran but no timer event reached the event program: `EVENT_TIMER`'s layout, the enable value's b29, or the period from `clock_hz` | Read `EVENT_TIMER` (0xacc, named) once: bit 24 should be set, with a nonzero period. Stage 1 isn't passed until the heartbeat moves. |
| The heartbeat moves very fast | The timer period is short for this clock | Not a failure. Record the rate for the `clock_hz` entry (gap 14). |
| Interrupt 21's count climbs fast | An event the host enabled keeps firing | Unload the module. Only SW_EVENT is enabled, and init raises none, so this is unexpected. |

## The diagnostic entry (the third attempt)

The second attempt (log 2026-11-11-1946) got this far:
- `USE_CLKS` came on and memory reads completed;
- there was no fault;
- the heartbeat, the version word and `INIT_STATUS` stayed 0.

So something in the USE's clock domain ran, but the image's first store (instruction 6)
never landed. `cores/sgx530_diag.py` builds the same image with markers in front of
`phas.i`. Each one depends on fewer forms than the next:

| Step | Instructions | Leaves |
|---|---|---|
| 1 | `limm r0`, `str` to `EVENT_TIMER` (K) | `EVENT_TIMER` = 0xa5a (b24, enable, clear: no timer) |
| 2 | the patched `limm r17`/`r14`, `limm r0`, `st.a.d` | control block word 0x28 (the log's "wait") = 0xd1a60001 |
| 3 | `ldr` `CORE_ID`, `st.a.d` | word 0x18 (the log's "version") = 0x01120000 |
| 4 | `ld.a.d`, `imae`, `st.a.d` on the heartbeat | heartbeat 1 |
| 5 | `phas.i`, then the normal entry (its own heartbeat +1, init) | heartbeat 2; init writes version 0x00010000 and `INIT_STATUS` 1 |

Every event runs the markers again, so while both halves run, the heartbeat moves by 2 per
event. Init leaves word 0x28 alone, and no wait on the init or timer path can time out.

```bash
python3 ukernel/build.py sgx530_diag sgx530_diag.bin sgx530_diag.json
python3 drm/tools/mkukfw.py sgx530_diag.bin -o sgx530_ukernel.bin --manifest sgx530_diag.json --core-rev 0x00010205
python3 drm/tools/mkukfw.py --check sgx530_ukernel.bin
```

Expected:
- `--check` prints image 0x4338, crc 0x1fb64e42, features 0x4, entry program 0 at 0x0,
  and patches 0x10 <- ta3dctl and 0x18 <- pdtable.
- The image is 17208 B, sha256 a2ee6a6d…, the same bytes through asm.py and
  `sgx-asm -c sgx545`.
- The packaged file is 845463a3…. The event program moves to 0x02004340.

The file name stays `sgx530_ukernel.bin`, so the driver needs no change. The one extra
read is `EVENT_TIMER` (0x0acc, named) on the failed start, next to the status words.
Without it, step 1 goes unread, and an all-zero result can't tell row 1 from row 2.

| `EVENT_TIMER` | wait | version | heartbeat | init | What it means |
|---|---|---|---|---|---|
| 0 | 0 | 0 | 0 | 0 | None of our code ran: the entry program's data under the SGX530's layout, the DOUTU against `USE_CODE_BASE_15`, or `limm`/`str` encode differently. Or the SGX530 needs `phas.i` first (U). Next: the entry side. |
| 0xa5a | 0 | 0 | 0 | 0 | Our code runs and register stores work; the memory store doesn't land. Suspects: `st.a.d [moe]`, or the patched address. |
| 0xa5a | 0xd1a60001 | 0 | 0 | 0 | Memory stores work, so the address patch is right. `ldr` or the `wdf` after it stops. |
| 0xa5a | 0xd1a60001 | 0x01120000 | 0 | 0 | `ld.a.d` or `imae`. |
| 0xa5a | 0xd1a60001 | 0x01120000 | 1 | 0 | Everything before `phas.i` works; `phas.i` stops or derails the task (ISA.md's highest risk). Next: a per-core flag that drops it. |
| any | 0xd1a60001 | 0x01120000 | 2 | 0 | `phas.i` runs, and the stall is in init, past the entry. |
| period \| b24 | 0xd1a60001 | 0x00010000 | ≥ 2, +2 per event | 1 | Init passes and the timer runs. The normal image's only difference is `phas.i` at instruction 0. |

Other values: record them. An `EVENT_TIMER` other than 0, 0xa5a or init's value means a
field is narrower than assumed, which is still evidence that step 1 ran.

**What the addresses depend on.** hwt_window showed on the SGX545 that `ba` and `phas.i`
targets count from the code base the task runs from, not from the heap.
- The entry runs from `USE_CODE_BASE_15`. The DOUTU uses segment 0 and offset 0, and
  `USE_CODE_BASE_15` points at the kernel code heap's start, where the image is loaded at
  offset 0. So the image's indices are the base's indices, and build.py adds no load
  offset.
- `phas.i #0, #0` names no next phase, so it has no target.
- Steps 1-4 have no branch. The normal entry's first `ba` comes after the markers.
- The image is 17 KiB: one window.
- The markers go to the control block at 0x02403000 (words 0x18, 0x28 and 0x2a, at 0x02403060,
  0x024030a0 and 0x024030a8). That is page 3 of the kernel data arena, mapped
  read-write and EDM-protected in the firmware directory (PDE 9, valid only).
- What these don't cover is the unit of `USE_CODE_BASE`'s 24-bit address field on the
  SGX530 (gap 9). The 32-bit SGX535 and SGX545 have 25-bit fields in units of 128 B. A
  28-bit core with a 24-bit field would fit a 16 B unit, and then base 15 would point at
  0x00400000. Nothing is mapped there at start, so I'd expect a USSE fetch there to fault.
  The second attempt saw no fault, but row 1 can't rule it out.

**`sgx530_diag_nophas`** is the same build without `phas.i`: after step 4 the normal
entry falls through. It is the follow-up if the diagnostic entry stops at heartbeat 1.
- Expected: image 0x4330 (17200 B, sha256 c42a740e…) and crc 0x5bd6b2f7.
- The same patches, and the packaged file is 8d1a634f….
- A pass gives the same results as the last row above.

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
