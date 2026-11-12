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
  them for any core. The board owner decides whether anything confirms them before stage 1, and
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

On a guessed core, powervr_sgx also logs `<when>: clock status … event status … BIF req …
int …`, with the clocks on, before the kick, and every 100 ms after it until init
completes. On the OMAP36xx it adds `<when>: OCP revision … sysconfig … debug config …
status …` (the OCP wrapper, [sgx530.md](sgx530.md)). Expected there: revision
0x40000000, sysconfig 0x28 and debug config bit 31 set (ti-sysc's quirk). In the status
word, MWait (b16) or MStandby (b17) set while `BIF req` stays nonzero would point at the
initiator's requests not completing on the interconnect (G: from the field names only). The driver holds a runtime-PM
reference on a single-context core, so `power/control` no longer needs setting by hand.

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
| The base reads back, init stays 0 (the probe fails after 500 ms) | The event program didn't run or didn't reach the microkernel: the enable register's layout, the kick, the entry program's data layout, or `USE_CODE_BASE_15`. Or the microkernel's first instruction, `phas.i`, isn't valid on this core: the SGX530 lacks `USE_UNLIMITED_PHASES` (ISA.md, "Other cores: the SGX530") | Stop. Record the logged `EVENT_STATUS`, the BIF fault registers, `BIF_MEM_REQ_STAT` and the heartbeat. A BIF fault points at the PD or the heap layout. No fault and no event bit points at the event program. Reads outstanding with no fault point below the MMU: the table walk's own reads or the interconnect. The first attempt (log 2026-11-11-1532) gave this: 2 reads, no fault; the heartbeat wasn't logged. The second (log 2026-11-11-1946) narrowed it. The OCP wrapper was as mainline sets it, with its initiator not stalled. `USE_CLKS` came on after the kick, reads came and went (2, 0, 2), and the heartbeat stayed 0. So the USE (or the PDS) runs, but not up to the microkernel's instruction 6. The third (log 2026-11-11-2053), with the diagnostic entry, stayed all 0, `EVENT_TIMER` included. None of the microkernel's code runs, not even its first `limm` + `str`. |
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
| 0 | 0 | 0 | 0 | 0 | None of our code ran: the entry program's data under the SGX530's layout, the DOUTU against `USE_CODE_BASE_15`, or `limm`/`str` encode differently. Or the SGX530 needs `phas.i` first (U). Read the BIF registers to tell them apart (gap 9). A BIF page fault at 0x00400000 (`BIF_FAULT` b12-27) means `USE_CODE_BASE` counts in 16 B units on the SGX530, and base 15 = 0x40000 points there. A fault at 0x04000000 would mean 256 B units. No fault leaves the unit at 128 B and points at the PDS side: the entry program or the DOUTU. Next: the entry side. |
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

## After the third attempt

Attempt 3 (log 2026-11-11-2053) landed in the all-zero row with no BIF fault: `EVENT_TIMER`
stayed 0 at every sample. So the USSE ran none of the image. `USE_CLKS` stayed on,
`BIF_MEM_REQ_STAT` read 2 throughout, and nothing faulted.

**Why not a sled of markers at other offsets.** At start, the firmware directory maps
only the code arena (0x02000000-0x0200dfff), the PD table (0x0200f000) and the data arena
(0x02400000-0x0240dfff). Every misreading I can construct lands outside them:
- a code-base unit of 16, 32, 64 or 256 B puts base 15 at 0x00400000, 0x00800000,
  0x01000000 or 0x04000000;
- a narrower base-index field sends the task through a pixel or vertex code base, none
  programmed into a mapped page;
- bit 27 read as part of the offset puts the task megabytes past the arena.

Each would fault, and three attempts saw no fault. So copies of step 1 elsewhere only
help if the SGX530's fault reporting itself doesn't work. Nothing so far shows that.

**What fits instead: the task is never scheduled.** The DOUTU's word 0 has `0x8000000` in
the field PDS.md reads as the temporary count in fours (bits 24-31): 8, or 32
temporaries. That reading is a guess even on the SGX545. The image uses r0-r17 and r23
(r23 only in the TA kick's link save), so 24 temporaries. Stage 1 needs only r0-r17. The
manifest now records `"temps": 24` (build.py: the image's highest rN plus 1). The SGX530's
per-task limit is U. If it can't give one task 32, the task waits forever: no
instruction, no fault, the clocks on. That is attempt 3.

**Attempt 4, one variable:** the same `sgx530_diag` image, with the entry DOUTU's bits
24-31 = 6 (24 temporaries, the manifest's `temps`) instead of 8. Everything else stays.
The runbook's ladder table reads the same.

If it lands in the all-zero row again, the next variables are, in order:
1. the event data registers 0xa6c/0xa70 (1 and 6, a guess on the SGX545 too: probably
   the data size and something else);
2. DOUTU word 1 (2, the microkernel templates' value);
3. the MOVS selects under a one-word interleave.

## The fourth attempt: probes for the code-base unit

This attempt is meant to answer as much as one board slot can. `cores/sgx530_probe.py` is
`sgx530_diag`'s ladder, unchanged at offset 0, followed by nine probes past the image, one
per 4 KiB page. Each probe writes its own `EVENT_TIMER` value (b24 clear) and ends.

The entry DOUTU's offset is 0, so its own unit can't matter. The probes test the other
unknown, `USE_CODE_BASE`'s unit. The host writes field 0x40000, the kernel code heap at
0x02000000 in 128 B units. Under a unit of 2^k bytes, base 15 would start at
0x40000 << k. With the module parameter `code_base_probes=1` (off by default, and only
for an `event_pds_guessed` core's first start), powervr_sgx maps probe k's code-arena
page there, read-only like the code arena:
- it maps them before the reset, whose `INVALDC` covers them;
- it logs `code-base unit probes mapped`;
- it unmaps them right after the init wait.

Without the parameter, nothing extra is mapped, and the probe image runs as the plain ladder.
The 0xd000 page holds probe 9 and the PDS programs together.

| k | Unit | Map at | Code-arena offset | `EVENT_TIMER` |
|---|---|---|---|---|
| 0 | 1 B | 0x00040000 | 0x5000 | 0xb00 |
| 1 | 2 B | 0x00080000 | 0x6000 | 0xb01 |
| 2 | 4 B | 0x00100000 | 0x7000 | 0xb02 |
| 3 | 8 B | 0x00200000 | 0x8000 | 0xb03 |
| 4 | 16 B | 0x00400000 | 0x9000 | 0xb04 |
| 5 | 32 B | 0x00800000 | 0xa000 | 0xb05 |
| 6 | 64 B | 0x01000000 | 0xb000 | 0xb06 |
| 7 | 128 B | 0x02000000 (the image itself) | 0x0 | 0xa5a: the ladder |
| 8 | 256 B | 0x04000000 | 0xc000 | 0xb08 |
| 9 | 512 B | 0x08000000 | 0xd000 | 0xb09 |

A unit of 1 KiB or more would start past the 28-bit space.

Build it:
- `python3 ukernel/build.py sgx530_probe …`, then mkukfw as before.
- The image is 53272 B (0xd018), sha256 1356a4c5…, the same through asm.py and
  `sgx-asm`.
- `--check` should print image 0xd018, crc 0xd6bbd623, features 0x4, entry program 0 at
  0x0, and patches 0x10/0x18.
- The packaged file is fa39d633…. The event program moves to 0x0200d020, still inside
  the code arena: the PDS programs end at 0xd220, under 0xe000.

The 24-temporary entry DOUTU (bits 24-31 = 6; "After the third attempt") is in the same
driver build, kernel a8a66544, for every SGX530 start. That gives two ways to run the
fourth attempt (the coordinator picks):
- **Temporaries only:** the `sgx530_diag` image (845463a3…), no parameter. Read it
  with the ladder table above.
- **Temporaries and probes:** the `sgx530_probe` image (fa39d633…) with
  `code_base_probes=1`. Check the log line. Without it, a 0 below says nothing about the
  unit.

Reading the combined run:

| `EVENT_TIMER` after the kick | What it means |
|---|---|
| 0xa5a, then the ladder as in its table | The base unit is 128 B. The task now starts, so the temporary count was the block. |
| 0xb0k | The base unit is 2^k B. The temporary count may have mattered as well; the next attempt keeps 24 and fixes the unit. |
| 0 | Neither the unit (if the alias pages are mapped as described) nor the temporary count. Next: 0xa6c/0xa70. |

A BIF fault now says the same as before; the probe pages make no new fault
possible.

## After the fourth attempt

Attempt 4 (log 2026-11-11-2112) ran the probe image with `code_base_probes=1` and the
24-temporary DOUTU. `EVENT_TIMER` stayed 0, so neither the ladder nor any probe ran.
- The temporary count alone wasn't the block.
- The probes fire only if some USSE task starts, so they don't decide the code-base unit.
  They show that no task starts at an address any probe covers. The unit stays U (gap 9),
  but it is no longer a lead.

**What the SGX530's own DDK does differently: the heap layout (K).** The GPL
`sgxconfig.h` of the SGX530 DDK 1.4 kernel places, in its 28-bit space:

| Heap | Base | Size |
|---|---|---|
| PDS pixel | 0x0D800000 | 8 MiB |
| PDS vertex | 0x0E000000 | 8 MiB |
| reserved code | 0x0E800000 | 512 KiB |
| kernel code | 0x0EC00000 | 512 KiB |
| kernel data | 0x0F000000 | 4 MiB |
| pixel shader | 0x0F400000 | 5 MiB |
| vertex shader | 0x0FC00000 | 2 MiB |

The general, 3D parameter, TA data and sync heaps sit below 0x0D800000.

Both DDKs put the kernel code heap, which holds the event PDS program, above the PDS pixel
heap that `PDS_EXEC_BASE` names: on the SGX545 directly above it, on the SGX530 above both
PDS heaps, all within 24 MiB of the exec base. Ours, single-context.md section 5, is the
only layout that puts kernel code below `PDS_EXEC_BASE` (0x02000000 against 0x02c00000).
- If the SGX530's PDS reaches only addresses above its exec base, or within a window from
  it, the event program is out of its reach on our layout.
- That predicts a misdirected fetch. A fault is expected but not certain: a PDS fetch
  outside its window may wrap or be dropped rather than translated.

**Attempt 5, one variable:**
- Move the PDS, kernel and shader heaps to the DDK's bases above. `PDS_EXEC_BASE` becomes
  0x0D800000 (its K field is b20-27), and the kernel code heap 0x0EC00000.
- `USE_CODE_BASE_15` = 0x0EC00000 >> 7 = 0x1d8000, inside its 24-bit field.
- The image needs no change. It holds no heap address: its only addresses are the two
  loader patches, `ctlblk` (r17) and `pdtable` (r14). Everything else comes through the
  control block the host fills: host control, CCB control, the kernel CCB, and command
  data. Branch targets count from code base 15, whatever it points at.
  - Every other immediate is a register mask or an event bit: `EVENT_STATUS`/`HOST_CLEAR`
    masks such as 0x20842000, `EVENT_TIMER`'s enable, the MADD invalidate 0x10000000, the
    field-keep masks 0xf83fffff and 0x07ffffff, and the bounded waits' bits.
  - The manifest holds only offsets.
  - Only the probe table is layout-specific. It maps at the code-base field << k, so with
    kernel code at 0x0EC00000 (field 0x1d8000) its addresses change, and k ≥ 8 would
    start past 28 bits.
- Run it with the `sgx530_diag` image and the 24-temporary DOUTU.

**Free reads to add**, named in the SGX530's K header, at every sample:
- `PDS_PC_BASE` (0x0b2c, a 30-bit address; only the SGX530 has it);
- `PDS` (0x0abc, `DOUT_TIMEOUT_DISABLE` b6).

The SGX545's vendor init script writes 0xe to 0x0abc. Our SGX530 init doesn't write it.
Their values before and after the kick are new facts whatever attempt 5 shows.

If attempt 5 is still all zero, 0xa6c/0xa70 is next, then DOUTU word 1, then the MOVS
selects.

## After the sixth attempt: one slot, several variables

Attempt 6 (log 2026-11-11-2154) wrote 0xa68 as an offset from `PDS_EXEC_BASE`
(0x01404340, read back whole). It ran with the SGX530 DDK's heap layout and the
24-temporary DOUTU, and still gave all zero: `EVENT_TIMER` 0, `PDS_PC_BASE` and `PDS` 0,
`USE_CLKS` on, reads 2/0/1/2/2, no fault.

Every remaining suspect is a host-side setting. The image can't split them, because none
of its code runs until a task starts. One board slot can still test several if the driver
**sweeps them in one load**. For each variant:
1. hold the cores in reset and run the normal start sequence;
2. apply that one change;
3. kick, and poll `EVENT_TIMER` for about 100 ms;
4. log the variant and what it read.

The sweep stops at the first variant whose `EVENT_TIMER` leaves 0, and carries on with the
normal init poll from there. Each variant is one change from attempt 6's baseline:

| # | Change | Why |
|---|---|---|
| 0 | none (attempt 6) | the control |
| 1 | 0xa6c/0xa70 = 2, 6 | if 0xa6c counts data rows and an SGX530 row is 2 dwords (no 2-dword interleave), the entry's 4 data words are 2 rows |
| 2 | 0xa6c/0xa70 = 4, 6 | 0xa6c in dwords |
| 3 | 0xa6c/0xa70 = 1, 2 | 0xa70 as the code's dwords (MOVS + HALT), not the whole program's |
| 4 | 0xa6c/0xa70 = 0, 0 | the reset values: the PDS's defaults, if it has any |
| 5 | DOUTU word 1 = 0 | 2 comes from the SGX545 microkernel templates; vertex and state tasks have 0 |
| 6 | the entry's data in the SGX545's 2-dword interleave | the other layout, if both MOVS readings were wrong |
| 7 | `USE_CODE_BASE_15` DM = 2 | DM 3 for the EDM is the SGX545 vendor's value. The field is 2 bits on every core, and no GPL source numbers the data masters |
| 8 | `PDS` (0x0abc) b6, `DOUT_TIMEOUT_DISABLE` (K) | if a DOUT the USSE doesn't take times out and the PDS reruns the program, that fits reads that come and go with nothing started |
| 9 | 0xa58 = 0x7fffffff | the enable value is the SGX545's (G) and leaves out b14 and others. It is unclear whether the kick needs an enabled bit at all. b31 (master interrupt) stays out. |

All of it uses registers the start already writes or the SGX530 header names. Each
variant costs a soft reset and about 100 ms.

The image stays `sgx530_diag`: the first marker to land, `EVENT_TIMER` 0xa5a, names
the variant. If none lands, the result still rules out each single change. The next
step would then be pairs, starting with 1 or 2 combined with 5.

## After the seventh attempt: the kicker word

None of attempt 7's ten variants started a task (log 2026-11-11-2238). Each started clean:
`CLKGATESTATUS` was back to 0x00010000 after every reset.

**`PDS_PC_BASE` is probably a status register.** The GPL DDK 1.7 kernel (in-repo
`sgxinit.c:1039`) never writes it. It reads it only in its hang dump, next to
`EVENT_STATUS` and the BIF status registers. Read as the PDS's current program (G), its
0 at every sample means the PDS never started a program. The kick never reached it.

**The likely cause (K).** The same `sgxinit.c` (lines 373-384) handles a core without
`SGX_FEATURE_MULTI_EVENT_KICK`, which includes the SGX530. Its init kick sets the
event-kicker word to `(kicker + 1) & 0xff`, with a write barrier and the PDump comment
"First increment of the SGX event kicker value", and only then writes `EVENT_KICK`. The
CCB doorbell does the same. Our start zeroed the kicker and kicked without incrementing
it.

That fits all seven attempts, if the hardware dispatches a kick only when the word at
`EVENT_KICKER` has changed:
- the USE domain wakes;
- the BIF reads a mapped word, with no fault;
- the PDS never runs.

The SGX545 starts on `EVENT_KICK2`, which doesn't go through the kicker, so it never
showed.

**Attempt 8:**
- the init kick increments the kicker word first;
- attempt 6's settings (TI layout, 0xa68 as an offset from `PDS_EXEC_BASE`, 24
  temporaries);
- the `sgx530_diag` image, unchanged.

If that is still all zero, the next test comes from TI's DDK 1.4 `SGXInitialise`. It has
no init kick: after part 2 it starts `EVENT_TIMER` (enable plus the period), so init runs
on the first timer event (0x2204b00f enables b29).

## After the eighth attempt: the PDS reaches the DOUTU

Attempt 8 (log 2026-11-11-2300) added the kicker increment, and the PDS started.
- **`PDS_PC_BASE` is the PDS's program counter** (C). Writes don't stick. It reads dwords
  relative to `PDS_EXEC_BASE`.
- **Baseline: 0x005010d4.** ×4 = 0x01404350, the event program's offset + 0x10: its first
  code word, the MOVS to DOUTU. The PDS sits there.
- **Variant 1 (0xa6c = 2): 0x005010d8**, the HALT after it.
- **Variants 5, 6 and 7 (DOUTU word 1 = 0, the 2-dword interleave, DM 2): 0x0000f75c**,
  which doesn't point into our program.
- The ladder never ran: no 0xa5a, no marker, heartbeat 0.

So the kick reaches the PDS, and the PDS runs our entry program. In the baseline it stops
at the DOUT that hands the task to the USSE, as if the USSE doesn't take it.
- A DOUTU the USSE doesn't accept fits a PC stuck on the MOVS.
- A PC at the HALT (variant 1) suggests the MOVS went out. The task still didn't run our
  code, or ran it elsewhere.
- The three variants that move the PC to the same unexplained value all change something
  the USSE sees.

**Attempt 9:**
- **Image:** `sgx530_probe` with `code_base_probes=1`. Now that a task may start, a
  probe landing would show where it started. The kernel maps only the probes that fit
  the TI layout (1-64 B units).
- **Logging:** `PDS_PC_BASE` sampled about every 10 ms in each variant, to tell a PC that
  moves from one that sits.
- **Variants,** from attempt 8's baseline:

| # | Change | What it tests |
|---|---|---|
| 0 | none | the control |
| 1 | 0xa6c = 2 | the PC went on to the HALT |
| 5, 6, 7 | as before | the PC left our program |
| 1+5, 1+6, 1+7 | together | the HALT reading with each USSE-side change |
| 5+7, 1+5+7 | together | the two changes nearest the task's start |
| T4 | DOUTU temporaries field = 1 (4 temps) | a task that asks for little. Step 1 needs only r0 |
| 1+T4 | together | |
| T0 | temporaries field 0 | the SGX545's vertex and state programs use 0 |

A probe marker (`EVENT_TIMER` 0xb0k) in any variant means the task started at another
code-base unit. 0xa5a means it started at our image.

## After the ninth attempt: which code base does the task use?

Attempt 9 (log 2026-11-11-2324) ran 13 variants with the probes mapped. No ladder, no
probe. Within every variant, `PDS_PC_BASE` cycled through:
- our event program's MOVS and HALT;
- 0xf35c (0xf75c with `sgx530_diag`);
- 0.

So the PDS keeps re-running our program and never starts a task.

0xf35c and 0xf75c aren't in anything we build. As dwords from `PDS_EXEC_BASE` they
are 0x0d83cd70 and 0x0d83dd70, in the PDS pixel heap, where nothing is mapped; our image
starts at dword 0x500000. Their 0x1000 shift between images is unexplained.

**Code-base pads.** If the DOUTU's code-base index is read differently on the SGX530
(field position or width), the task goes through a base other than 15. `cores/sgx530_bases.py`
is the ladder at offset 0 plus a pad per code base 0-14 past the image, on 128 B
boundaries. Pad n writes `EVENT_TIMER` = 0xc00 + n and ends.
- Offsets: pad n at 0x4380 + 0x80·n (pad 14 at 0x4a80).
- The image is 19096 B (0x4a98), sha256 7b51cdce…, the same through asm.py and `sgx-asm`.
- `--check`: crc 0x465944e3, features 0x4, the same entry and patches. The packaged file is f7de7b95….

For the sweep, the host points `USE_CODE_BASE_n` (n 0-14) at kernel code + pad n,
>> 7, in two passes:
- every DM 3, as base 15;
- the usual DMs: 1 for 0-9, 0 for 10-14.

Base 15 stays as it is. 0xc0n means the task went through base n. 0xa5a means base 15
after all.

**Calibrate the fault path (K registers only).** Every "no fault, so not that address"
inference so far assumes the SGX530 reports BIF faults as we read them, and it never has.
As the last step of the sweep:
1. point `EVENT_KICKER` at an unmapped address, such as 0x0e800000, the empty reserved
   code heap;
2. increment the kicker word and kick;
3. sample `BIF_INT_STAT`, `BIF_FAULT` and `BIF_MEM_REQ_STAT`.

A fault at 0x0e800000 confirms the path. None means the earlier "would fault" arguments
don't hold, and the probes and pads are the only evidence of where a task went.

**Order and recovery.** The board has no remote power switch, so a wedge costs a manual
power cycle. The calibration therefore runs last, after every pad pass is logged, and
recovers like this:
1. Write `EVENT_KICKER` back to the kicker buffer and `EVENT_KICK` nothing more.
2. Run the driver's recovery reset, `pvrsgx_reset(recovery = true)`. With list 0 on the
   scratch directory, it maps the scratch page at each faulting address, so the held
   request completes, then resets the BIF and the cores again (up to 32 rounds). That path is
   proven on the SGX545's hang recovery but not yet on the SGX530.
3. Read `BIF_INT_STAT` once more. It must be 0 before the normal failed-probe teardown.
4. If it isn't 0 after the 32 rounds: log it, hold the cores and the BIF in reset
   (`SOFT_RESET` = BIF | cores), and unload without any further register access.
   That leaves nothing for a pending request to complete into. Expect to need the
   board owner for a power cycle only if the board stops answering.

If the kernel side can't do steps 2-4 for this start, it should skip the calibration.
The pad passes don't need it.

## After the tenth attempt: the MOVS itself

Attempt 10 (log 2026-11-11-2339) ran 18 variants with pads on code bases 0-14, under the
usual DMs and under DM 3. No pad and no ladder ran.

**The SGX530 reports BIF faults (C).** The calibration pointed `EVENT_KICKER` at
0x0e800000 and kicked:
- `BIF_INT_STAT` read 0x4004 and `BIF_FAULT` 0x0e800000 at every sample; `MEM_REQ_STAT` 0.
- Recovery left no fault pending, with no scratch-page rounds needed.
- So the kick reads the kicker word, and every earlier "no fault" means no access went
  unmapped.

Across attempts 4, 9 and 10, no USSE task starts, through any code base, unit or DM.
The PDS runs our event program, passes its MOVS, reaches its HALT, and loops.

**What's left is the MOVS.** It passes, yet no task starts. Two of its fields would do that
silently if the SGX530 reads them differently:
- **dest** (bits 0-4), 5 = DOUTU on the SGX545. Another number on the SGX530 would send the
  three words to another unit.
- **cc** (bits 24-26), 7 = always on the SGX545. If it means something else, the MOVS is
  skipped. The SGX545's vertex-fetch DOUTU uses cc 3.

**Attempt 11:** one load, the `sgx530_bases` image with `code_base_pads=1` (so a task through
any base still lands on a pad), each variant one change to PDS0's MOVS:

| Variant | Change |
|---|---|
| control | dest 5, cc 7 |
| dest | 4, 6, 7, 1, 0, 2 |
| cc | 3, 0 |
| all-slots | the row's four data words all = the DOUTU word, all four selects 0: DOUTU (w0, w0, w0) under any interleave |

- dest 3 is DOUTD, a DMA from the address in word 0. Leave it out: with our word it would
  fault at 0x0600000f or so.
- A pad or the ladder landing names the variant.
- If a variant's PC stops cycling, or settles on a new value, log that too.

## After the eleventh attempt: before the first instruction

Attempt 11 (log 2026-11-11-2351) changed the MOVS's dest and cc, and tried the all-slots
row. Nothing started. Under every change, cc 0 included, the PC sat on the event
program's first instruction and never reached the HALT. A skipped MOVS would have reached
it. So the PDS likely stalls before it executes the first instruction. It may be waiting
on something it needs first, probably its data segment or a data-store allocation, which
0xa6c/0xa70 describe.

The third PC value, 0xf75c, came back with the same image that gave 0xf33c before. So it
doesn't track our image: probably another PDS program or state, not ours.

**Attempt 12:** one load, the `sgx530_bases` image with the pads, the kicker fix. A grid
over the two event data registers, since every earlier 0xa6c/0xa70 change except 2/6
ran without the kicker fix:
- 0xa6c ∈ {0, 1, 2, 4};
- 0xa70 ∈ {0, 1, 2, 4, 6, 8}.
- That is 24 cells, including the baseline 1/6. Each logs the PC trace.

Then two enable values with the kicker fix: 0xa58 = 0x7fffffff, and 0xa58 = 0.

A cell whose PC reaches the HALT, or leaves the first instruction, narrows it down even
if no task starts.

## After the twelfth attempt: 0xa6c is the code start

Attempt 12 (log 2026-11-12-0002) found what 0xa6c does: it is where the PDS starts
executing, in dwords from the event program's start (C).
- 0 starts the PDS on the program's first word, 1 on the next, 4 on the MOVS.
- 0xa70 changed nothing in that test.
- The 1 used since attempt 1 started the PDS on data word 1. That explains attempts 1-11.
- On the SGX545, 1 plausibly counts one 16-byte row (G).

The SGX530's start now writes 0xa6c = 4, the program's 4-dword data segment. The PDS then
sits on the MOVS, and still no task starts.

**Attempt 13** reruns the earlier variants from that start, with no image change. Its
first useful read is cc 0:
- the PC moves to the HALT: the PDS executes the MOVS and waits on its DOUT;
- the PC stays on the MOVS: it is stuck before executing it.

The MOVS-side variants are crossed with the two likeliest readings of 0xa70: 4, the data
segment in dwords, and 6, the whole program.

## After the thirteenth attempt: does the PDS execute at all?

Attempt 13 (log 2026-11-12-0022) ran 34 variants from the 0xa6c = 4 start. The PC showed
the MOVS's address and never the HALT's, even with cc 0, which should skip the MOVS. So the
PDS fetches the first code word and doesn't execute it. Either the SGX530's PDS encoding
differs from the SGX545's (PDS.md), or the PDS waits on something before it executes
any instruction.

**Attempt 14:** host-built PDS0 programs. The data segment stays (4 dwords, 0xa6c = 4),
and every encoding is the SGX545's, hardware-confirmed there. PDS.md has BRA as
`9 cc target` (the target an instruction index from the first code word) and HALT as
`af000000`.

| Program (code words) | If the PDS executes | If it doesn't |
|---|---|---|
| A: `97000001 97000001` (bra #1; bra #1, a self-loop on word 1) | the PC sits on code word 1 | the PC stays on word 0 |
| B: `af000000` (halt) | the PC leaves word 0 or goes idle | as now |
| C: `97000003 af000000 af000000 af000000` (bra #3 over two HALTs) | the PC is seen at word 3 | word 0 |
| D: A with 0xa6c = 0 and the loop at words 0-1 (no data segment) | word 1 | word 0 |

The self-loop is the clearest test. A PDS that executes ends up looping on word 1, so the 10 ms
samples catch it there.

Also read 0xa68's low four bits back after writing them (0x1, 0x4, 0xf). The SGX545
writes the address & ~0xf. If the bits stick on the SGX530, they're a field: candidates are
a data size or a row count. Then rerun A with them set.

## Correction: 0xa6c counts 16-byte rows

The sections after the twelfth and thirteenth attempts read 0xa6c in dwords. It counts
16-byte rows.
- `PDS_PC_BASE` is in dwords: 0x5012a8 × 4 is exactly PDS0's start.
- 0xa6c = 0, 1, 2 and 4 gave 0x5012a8, …ac, …b0 and …b8. That is 4 dwords, 16 bytes,
  per unit.

So:
- 0xa6c = 1, the SGX545's value, starts the PDS at byte 0x10: our MOVS, after the one-row
  data segment. It was right from attempt 1 (C).
- 0xa6c = 4 starts it at byte 0x40, past PDS0's 0x18 bytes, inside PDS1's slot. Attempt
  13's results don't apply to our program. Neither does the attempt-14 plan above,
  which put its programs after the data segment with 0xa6c = 4.
- Attempts 8-11 read with 1 = row 1: the PC cycling …ac / …b0 fits the PDS fetching
  our MOVS-and-HALT row, then the next row. It probably did execute the MOVS and the HALT
  and send the DOUT, and no task started. That points back at the DOUTU's contents and the
  USSE side.

**Attempt 14, corrected.** 0xa6c = 1, code from row 1. The PC may resolve only rows, so
each landmark gets its own row:
- **A:** row 1 = `bra #8` and three HALTs; row 2 = four HALTs; row 3 = `bra #8` (a
  self-loop) and three HALTs. If the PDS executes, the PC sits at row 3 (…b4), a value
  never seen before. If it doesn't, it stays at …ac.
- **C:** row 1 = `bra #4` and three HALTs; row 2 = four HALTs. If it executes, the PC shows
  …b0 and then goes idle.
- **D:** A from byte 0 with 0xa6c = 0.

The test program needs 0x40 bytes and overlaps PDS1's slot, which the SGX530's event
registers don't use. The normal start goes back to 0xa6c = 1.

## After the fourteenth attempt: is 0xa70 the program's length?

Attempt 14 (log 2026-11-12-0038) ran the row-aligned programs with 0xa6c = 1 and 0xa70 = 6.
- No PC reached a branch target. Program A's self-loop on row 3 never held the PDS: the
  PC showed row 1 (…ac), 0xf33c and 0.
- 0xa68's low four bits read back 0.
- Every `PDS_PC_BASE` value so far is row-aligned, so it may be the row fetched rather
  than a per-instruction PC.

**Reading (G): 0xa70 is the program's length, and 6 cut A short.** On the SGX545, 6 is
exactly PDS0's size in dwords: one 4-dword data row plus MOVS and HALT. If the PDS fetches
only 0xa70 dwords:
- the MOVS program (attempts 8-11) was whole;
- program A (4 rows, 16 dwords) was cut off inside row 1, before either branch;
- attempt 12's 0xa70 grid only ran the MOVS program, which can't show execution.

**Attempt 15:** program A, 0xa6c = 1, and 0xa70 ∈ {1, 2, 3, 4, 6, 8, 12, 16, 20, 32, 64}.
The list covers dwords and rows.
- A PC held on row 3 (…b4) for some value means the PDS executes the SGX545's BRA. The
  smallest such value also gives 0xa70's unit (C).
- Then the same values with program C, and with D (0xa6c = 0, code from byte 0).
- If no value holds the loop, 0xa70 isn't the length. Then the PDS ISA itself is the
  question, with a test of whether row 1's content changes the trace at all: zeros, HALTs,
  or the self-loop at row 1.

## After the fifteenth attempt: detect execution by a fault

Attempt 15 (log 2026-11-12-0054) ran programs A, C and D under eleven 0xa70 values. The PC
only ever showed the start row, 0 and an unexplained value: 0xf7dc this time, from the same
image that gave 0xf33c before. The run didn't read 0xa70 back, so whether each value latched
is U.

**`PDS_PC_BASE` may not be a running PC.** Its name, and every value so far being a
program's start row, fit "the start address of the program last dispatched" (G). Then
attempts 14 and 15 couldn't have seen a branch target even if the branch ran, and the PC
trace can't detect execution.

**A detector that doesn't need it: a fault at an address we choose.** The SGX530 reports BIF
faults (attempt 10). An instruction that reads an unmapped address proves it executed if the
fault names that address. Attempt 16 has two programs, each with the data row and 0xa6c = 1:

| Program | Row 1 | Proves, by a fault at |
|---|---|---|
| F1 | MOVS dest 3 (DOUTD) from a layout-independent data row `[0x0e840000, 0, 0, 0]`: word 0 is the source address, word 1 the control (0 = 1 dword), then HALT | 0x0e840000: the PDS executes a MOVS and its DOUTD |
| F2 | `bra #0x4000`, then three HALTs | the first code word + 0x10000, past the code arena and the PD table: the PDS executes a BRA and fetches its target |

- 0x0e840000 is in the TI layout's reserved code heap, which is empty, and differs from
  the kicker check's 0x0e800000, so the two can't be confused.
- Run each with 0xa70 = 6, then with 0xa70 = 16, read back after the write. Run the
  recovery reset between variants, as the fault check does.
- If F2 faults where predicted, the PDS runs the SGX545's BRA, and the earlier "loop didn't
  hold" readings were the PC register's meaning, not the PDS.
- If F1 faults, MOVS and DOUT work too, and the DOUTU's own contents are the question
  again.
- If neither faults, the PDS doesn't execute our first code row at all.

## After the sixteenth attempt: does the PDS fetch the program at all?

Attempt 16 (log 2026-11-12-0116): neither F1 (a DOUTD from 0x0e840000) nor F2 (a BRA past the
code arena) faulted, at 0xa70 = 6 or 16.
- 0xa70 and 0xa6c read back as written.
- The fault check worked as before.

So our first code row doesn't execute as the SGX545's encoding. F2 has one loophole: a
narrower BRA target field could wrap into mapped memory. F1 has none.

That leaves two readings: the SGX530's PDS decodes these words differently, or it never
fetches and executes them at all. A fault can tell them apart: point the fetch itself at
an unmapped page.

**Attempt 17**, each variant followed by the recovery reset:

| Variant | Change | A BIF fault at … means |
|---|---|---|
| P3 | before the kick: `PDS_INV0`-`3` = 1 (DSC b0) and `PDS_INV_CSC` = 1 (KICK b0), all K-named on the SGX530; then the baseline, and F1 | no fault expected. If F1 now faults at 0x0e840000, a stale PDS cache hid our code |
| P2 | 0xa6c = 0x1000 (the code start 64 KiB past PDS0; read it back, and use the widest value that sticks) | 0x0ec14000 (or wherever the start lands): the PDS fetches code from memory at its start row |
| P1 | 0xa68 = 0x01040000, the offset of the empty 0x0e840000 from `PDS_EXEC_BASE` | 0x0e840000: the PDS fetches its program (data or code) from memory |

Order: the baseline, P3, P2, P1, then the fault check.

Reading:
- **P1 doesn't fault:** the PDS never fetches the program. `PDS_PC_BASE` only latches the
  start address, and the dispatch stalls before any fetch: a resource, a data-store
  allocation or an event-side condition.
- **P1 faults but P2 doesn't:** it fetches the data but not the code row.
- **Both fault:** the PDS fetches our code and doesn't execute it as the SGX545's
  encoding. The PDS ISA itself is then the question.

## After the seventeenth attempt: the PDS stops before it fetches

Attempt 17 (log 2026-11-12-0138):
- **P1:** with 0xa68 pointed at an empty page, `PDS_PC_BASE` showed that page + 0x10 (the
  computed start), and no BIF fault came. So the PDS forms the start address and stops
  before fetching anything through the BIF (C, as far as faults show).
- **P3:** the PDS cache invalidates changed nothing.
- **P2:** 0xa6c holds at most 12 bits (0x1000 read back 0).
- **The third PC value** changes per boot (0x136500 this time), not per image. It isn't ours.

So something the PDS needs before its first fetch is missing. Candidates: a data-store
or attribute allocation, or some other resource the setup didn't give it.

**Setup the SGX545 does and we skip.** Its vendor init script part 2 (SRVINIT.md) also writes
0xa00 = 0x7c000, 0xa74 = 0x208200, 0xa78 = 0x2c, 0xa7c = 0, 0xa80 = 0, 0xaac = 0x45 and
0xabc = 0xe. Those offsets sit in the same block as the event-PDS registers, which turned
out to be at the SGX545's offsets on the SGX530. Our SGX530 init writes none of them.

**Attempt 18, approved** (the coordinator, under the standing go and with the board on
the smart plug). Seven offsets inside the SGX register block, each with its known SGX545
value: no ranges, no sweeps, no other offsets.

| # | Variant | Writes |
|---|---|---|
| 1 | `PDS` alone | 0xabc = 0xe (named on the SGX530: `PDS`; 0xe sets its unnamed bits 1-3) |
| 2 | one at a time, each read once before the write and logged | 0xa00 = 0x7c000; 0xa74 = 0x208200; 0xa78 = 0x2c; 0xaac = 0x45 |
| 3 | the four together | as in 2 |
| 4 | only if 2 and 3 change nothing, by the same rule | 0xa7c = 0, then 0xa80 = 0 |

- Each variant runs the baseline and F1 (a fault at 0x0e840000 means the PDS now fetches).
  Success is a task starting or that fault.
- The recovery reset comes first, as always.
- The sweep stops at the first variant that hangs the bus or the board. A power cycle is
  only for recovery: it's logged, and the board's owner session is told first.
- Each offset's read-back goes in the fact sheet, tagged C for the value read on the
  SGX530 and G for what the register does.

## After the eighteenth attempt: 0xa74 makes the PDS execute

Attempt 18 (log 2026-11-12-0151): **0xa74 = 0x208200 makes the PDS execute (C).**
- **With F1:** a fault at 0x0e840000, F1's DOUTD source, with `BIF_INT_STAT` 0x4002. So the
  SGX545's MOVS→DOUTD encoding runs on the SGX530, and the DOUT DMA is requestor b1.
- **With the normal entry (MOVS→DOUTU, HALT):** a fault at 0x0d800000 = `PDS_EXEC_BASE` +
  0, `BIF_INT_STAT` 0x4004. That is requestor b2, as the kicker read.
- **`PDS_PC_BASE`:** 0 at every sample.
- The four writes together gave the same; the others had no effect alone. 0xaac doesn't
  latch. The fact sheet's "PDS block setup" table has every read-back.

**The question:** what reads `PDS_EXEC_BASE` + 0? A PC of 0 and the b2 requestor fit a PDS
program fetch at offset 0 (G). Either another event source that 0xa74 enables dispatches a
program whose address register is unset, or the DOUTU itself triggers a second PDS
program, as the SGX545's task setup can.

**Attempt 19.** 0xa74 = 0x208200 is now part of the SGX530's init-script part 2 (kernel
43831efb), so every start includes it. 0xabc, 0xa00, 0xa78, 0xaac, 0xa7c and 0xa80 stay out.
The run uses the `sgx530_bases` image with `code_base_pads=1`: every code base 0-15 then
points at mapped code, so a USSE task fetching through any base lands on a pad or the
ladder instead of faulting. That answers the coordinator's question too: the DOUTU task's
code base being unmapped would show as a pad, not a fault at `PDS_EXEC_BASE` + 0.

| Variant | Change | Tells |
|---|---|---|
| V0 | none | the control: the fault at 0x0d800000 again |
| V1 | PDS0 = data row + HALT (no DOUTU) | whether the offset-0 fetch is the DOUTU's doing: no fault means yes |
| V2 | a kernel page mapped read-only at 0x0d800000 holding a copy of PDS0 (data row, MOVS→DOUTU, HALT) | the offset-0 fetch then runs a working entry, so the ladder (0xa5a) or a pad lands if it is a program fetch |
| V3 | the same page holding data row + HALT | the fault should go away with no task, if it's a program fetch |

- The page at 0x0d800000 is `PDS_EXEC_BASE`'s first page, the PDS pixel heap's start,
  which is empty at init. Map exactly that one page, for this test only (approved under the
  standing go), with the recovery reset first.
- A ladder or a pad landing in V2 starts the microkernel.

**Making 0xa74 part of the SGX530's start.** It is the first setup write shown to matter.
Agreed from this side: once the coordinator agrees, it goes in init-script part 2, logged
as C for its effect. Its bit meanings stay G.

## After the nineteenth attempt: the DOUTU task fetches from PDS_EXEC_BASE

Attempt 19 (log 2026-11-12-0202):
- **V1 (no DOUTU):** no fault, and the clock status went idle. So the fetch at
  `PDS_EXEC_BASE` + 0 is the DOUTU's doing.
- **V2 and V3:** with a page mapped there, the fault moved to the next page, 0x0d801000,
  whatever the page held. Something reads linearly from `PDS_EXEC_BASE` + 0.
- **The pads:** none fired, though every code base pointed at mapped code.

Reading (G): the DOUTU starts a USSE task whose instruction fetch begins at
`PDS_EXEC_BASE` + 0, not at a `USE_CODE_BASE`. The fault's b2 is then the USSE code fetch.
The SGX530 resolves DOUTU word 0's code address against the PDS's base. Its field layout
is still U.

**Attempt 20: run the microkernel from there.** The SGX530 image's stage-1 path fits in
its first 4 KiB:
- the event entry and the ladder at 0;
- init at 0x238, ending near 0x350.

So alias exactly one page: map the code arena's page 0 read-only at 0x0d800000 (the
same approval as attempt 19's one page).
- **Expected if the task starts there:**
  - `EVENT_TIMER` 0xa5a, then init's period with enable;
  - the 0x28 marker;
  - version 0x00010000;
  - `INIT_STATUS` 1.
- **Then the first timer tick** branches to `kick_end` at 0x1920, on page 1, and faults at
  0x0d801000, b2. That fault is the expected end of this attempt, not a failure.
- **Branch targets** count from the task's code base. If that base is `PDS_EXEC_BASE`, the
  alias is the image's start, and the init path's branches land inside page 0.

If init passes:
- the next attempt aliases the whole image (pages 0-4);
- then the DOUTU word-0 fields get mapped, so the task can start at the kernel code heap
  without an alias.
- V4 (the BRA page) is moot: the fetch is the USSE's.

## The attempt after 20: where the DOUTU's code address comes from

The alias of attempt 20 is a diagnostic. The driver has to set the right address instead.

**What points at the answer (K).** The GPL DDK's `sgxconfig.h` refuses to build, on a core
without `SGX_FEATURE_EDM_VERTEX_PDSADDR_FULL_RANGE` (the SGX530 lacks it), unless the kernel
code heap lies within 64 MiB above the PDS pixel heap. TI's SGX530 layout puts kernel code at
`PDS_EXEC_BASE` + 0x01400000. That fits attempt 19 exactly (G): on such a core, the EDM
task's code address is an offset from `PDS_EXEC_BASE` with a limited range, and our word 0
(base index 15 in b0-3, offset 0) gave offset 0.

**Attempt 21: map DOUTU word 0's address bits, one bit per variant.** No alias. The detector
is the BIF fault page (calibrated), and the markers for a start that lands on mapped code.
- Word 0 = the temporary count (b24-31 = 6, as now) | 1 << b, for b = 0 ... 23, one variant
  each, from b0 up.
- Each logs `BIF_FAULT`'s page and requestor. The fault page minus `PDS_EXEC_BASE` gives
  bit b's weight, for any weight of 4 KiB or more. Bits that leave the page at offset 0
  weigh less than a page, or nothing.
- If the 18-bit field holds the offset in 128 B units, bits 21 and 19 together give 0x01400000:
  the kernel code heap. A variant landing there runs the image (`EVENT_TIMER` 0xa5a), and
  the sweep stops.
- Some weights land the fetch on mapped data, for example the kernel data arena at
  +0x01800000. The task may then run data as code, but it can only touch our own mapped
  GPU buffers, which every start rebuilds. The recovery reset follows each variant, as always.

**Then the fix:** word 0 = (kernel code − `PDS_EXEC_BASE`) in the measured units, in the
measured field, for the SGX530 only. The alias goes away. Recording the field as C finishes
the open question of what the SGX530's DOUTU word 0 holds.

## After the twenty-first attempt: a 512 KiB window, and the selector

Attempt 21 (log 2026-11-12-0214): every DOUTU word-0 bit faulted with requestor b2, none
started the image. The address weights:

| word 0 bit | fault offset | address weight | offset bit |
|---|---|---|---|
| b16 | +0x9000 | 0x1000 | 12 |
| b17 | +0xa000 | 0x2000 | 13 |
| b18 | +0xc000 | 0x4000 | 14 |
| b4 | +0x10000 | 0x8000 | 15 |
| b5 | +0x18000 | 0x10000 | 16 |
| b6 | +0x28000 | 0x20000 | 17 |
| b7 | +0x48000 | 0x40000 | 18 |

(each minus the +0x8000 the selector adds)

**Reading (C for the weights, G for the rest):**
- b16-b18 and b4-b7 are the task's code offset, bits 12-18. That is a **512 KiB window**
  from `PDS_EXEC_BASE`, the SGX545's window size.
- b8-b15 are offset bits below 4 KiB, which the fault page can't show.
- b0-b3 and b19-b23 each add 0x8000 and aren't linear address bits. They are the
  base/segment selector that moves the 512 KiB window, the SGX545's `(off>>19) + base`
  mechanism. Our normal word 0 has base index 15 and offset 0, so it fetched the window at
  `PDS_EXEC_BASE` + 0.

The kernel code heap at `PDS_EXEC_BASE` + 0x01400000 is 40 windows away, past a 4-bit
selector. The SGX530 resolves the task's code address against `PDS_EXEC_BASE`, not against
`USE_CODE_BASE_15`, which it ignores. So the fix is to place the kernel code heap where the
selector reaches and set word 0 to point there, not to encode +0x01400000.

**Attempt 22: the selector's stride and reach.** No alias. At offset 0, one variant each:
- b0-b3 as the base index 0, 1, 2, 4, 8 (the normal word uses 15);
- b19, b20, b21, b22, b23 alone.

Each logs the fault page. The stride is a window per step for a base index, and larger for
the segment bits. The reach is the largest in-range fault before it wraps or faults at
`PDS_EXEC_BASE` + 0.

**Then the fix.** Within the selector's reach of `PDS_EXEC_BASE`, give the SGX530 a kernel
code heap at an offset the selector names exactly, and set the EDM DOUTU word 0 to that
selector with offset 0. For example, if the base index steps 512 KiB, the heap one window up
(`PDS_EXEC_BASE` + 0x80000, base index 1) starts the image. The alias of attempt 20 goes
away, and the heap layout's kernel-code placement becomes the SGX530's rule, recorded as C.

## After the twenty-first attempt, re-read: no selector in word 0

The attempt-21 data, reread with the kernel session: base index 1/2/4/8 (word 0 b0-b3) and
each of b19-b23 fault at `PDS_EXEC_BASE` + 0x8000, a flat shift, not a 512 KiB stride. The
normal word (b0-3 = 0xf, b27, offset 0) faults at + 0. So:
- The address bits b4-b7 and b16-b18 reach a **512 KiB window** (offset bits 12-18), and
  b8-b15 the sub-4 KiB bits.
- Nothing in word 0 strides by a window. A linear field with no striding bit has no
  striding combination, so **word 0 as we set it carries no working segment**: the reach is
  512 KiB above `PDS_EXEC_BASE`.

**The tension with the DDK (K).** The SGX530's kernel code heap is at `PDS_EXEC_BASE` +
0x01400000 (0x0ec00000), matching TI's `sgxconfig.h` exactly. A flat 512 KiB reach can't
touch it. Yet the DDK runs the SGX530 with kernel code there. So the segment must come from
somewhere we haven't set, or the EDM's code address isn't `PDS_EXEC_BASE`-relative the way
word 0 alone suggests. Candidates: a `USE_CODE_BASE` the SGX530 EDM does use (attempts 10
and 19 say the pixel/vertex ones are ignored, but index 15 at a different encoding is
untested here), or `PDS_PC_BASE`/another base the EDM adds.

**Attempt 22 (ran as kernel b49aaf7c):** the repeatability sweep of the attempt-21 selector
cells (temps alone, temps|0xf, temps|b27, temps|0xf|b27, and the single selector bits), to
confirm the flat +0x8000 vs +0 split.

**Attempt 23, in one load, no alias, on top of attempt 22:**
1. **A real segment, if word 0 has one:** the computed words for segment 40 (offset
   0x01400000): 0x06100008 (nibble 8, high 2 in b19-b20), 0x06000007 (the SGX545
   `(seg+15)&15`), 0x06f80000-style. A fault at or near 0x0ec00000 names the fix.
2. **Does the EDM use `USE_CODE_BASE_15`?** With the normal word, set `USE_CODE_BASE_15` to
   three values (0, kernel code >> 7 = 0x1d8000, 0x08000000 >> 7 = 0x100000) and read where
   the fetch faults. If it moves, the EDM uses the register and the earlier setup was the
   bug; if it stays at `PDS_EXEC_BASE`, the EDM ignores it. This is the likeliest real
   answer, since the DDK reaches +0x01400000.

**The fix depends on the result:**
- A word-0 segment reaches 0x0ec00000: set the EDM DOUTU word 0 to it, SGX530 only, heap
  unchanged (matches the DDK). No alias.
- `USE_CODE_BASE_15` moves the fetch: program it for the EDM, heap unchanged.
- Neither: the reach is 512 KiB, so move the SGX530 kernel code heap within 512 KiB of
  `PDS_EXEC_BASE` and set word 0's offset. This departs from TI's layout; flag it to the
  coordinator before committing.

## After the twenty-third attempt: the fix — USE_CODE_BASE is addr >> 8

Attempt 23 (log 2026-11-12-0300) found the root cause. Code base 15 steers the EDM fetch,
and the mapping is `fetch = (CB15 << 8) & 0x0fffffff` (C):

| CB15 | fetch | model (CB15 << 8, 28-bit) |
|---|---|---|
| 0x1d8000 | 0x0d800000 | 0x0d800000 |
| 0 | 0 | 0 |
| 0x100000 | 0 | 0 (bit 20 << 8 = bit 28, masked) |

So **the SGX530 encodes `USE_CODE_BASE` as `addr >> 8` (256 B units), not `>> 7` like the
SGX545.** `USE_CODE_BASE_ADDR_SHIFT` is a register-field shift; the address granularity is
the hardware's, and it is a power of two larger on the SGX530.

Provenance: tag the shift **C**, from attempt 23's three cells. The GPL `sgx530defs.h`
defines only the field (`ADDR_MASK` 0x00FFFFFF, 24 bits; `ADDR_SHIFT` 0, the register-field
shift; `DM` b24-25) and has no `ADDR_ALIGNSHIFT`, so it doesn't give the granularity. The
`<< 8` map fits all three cells exactly, and its high half is the coordinator's "field bits
12-19 as 1 MiB units" (field bit 12 → address bit 20 = 1 MiB); the low half is attempt 21's
b4-b7/b16-b18 = address bits 12-18. The 24-bit field at `<< 8` reaches address bit 31,
masked to the SGX530's 28-bit space.

**Where it lives:** the shift is a per-core value in the driver's core table (7 for the
SGX545, 8 for the SGX530), not an `if (530)` in the code path, so a third core sets its own.

The driver sets code base 15 = `kernel_code >> 7` = 0x1d8000. Under `<< 8` that is
`PDS_EXEC_BASE`, so every attempt fetched the EDM task from `PDS_EXEC_BASE` + 0. That single
wrong shift is the whole of why stage 1 never started: the PDS ran, the DOUTU started a USSE
task, and the task fetched from the wrong page.

**The fix (SGX530 only):** program `USE_CODE_BASE` with `addr >> 8`. For code base 15 that is
`kernel_code >> 8` = 0xec000, whose fetch is `0x0ec00000` = the kernel code heap, the image.
The same applies to the pixel and vertex code bases in init-script part 2. The heap stays at
TI's layout; no alias.

**Attempt 24: the fix, confirmed on hardware.**
1. Code base 15 = 0xec000 (`kernel_code >> 8`), DM unchanged, the normal word. Expected: the
   microkernel runs — `EVENT_TIMER` 0xa5a then init's period, the 0x28 marker, version
   0x00010000, heartbeat, `INIT_STATUS` 1. Stage 1 passes.
2. Two cells to pin the shift: code base 15 = 0x1b0000 (fetch 0x0b000000) and 0xec001 (fetch
   0x0ec00100), logged by fault page.
3. If stage 1 passes, the driver change is `USE_CODE_BASE` encoding `addr >> 8` for the
   SGX530, every code base in part 2, recorded as C.

## After the twenty-fourth attempt: the task runs, then derails

Attempt 24 (log 2026-11-12-0311): the `>> 8` fix is confirmed.
- **V1** (code base 15 = 0x1b0000): fault at exactly 0x0b000000 = 0x1b0000 << 8 & 0x0fffffff.
  So `USE_CODE_BASE` is `addr >> 8` on the SGX530 (C, now four cells).
- **V0** (the fix, all code bases `>> 8`, code base 15 = 0xec000): the fault leaves
  `PDS_EXEC_BASE` for the first time, a b2 code fetch at 0x08039000. But `EVENT_TIMER` is 0
  and `INIT_STATUS` is 0.

So the EDM task now fetches from the kernel code heap (0x0ec00000) and runs, but derails
before the diagnostic ladder's first `str` (the `EVENT_TIMER` write) commits. This is the
**first time USSE code has actually executed on the SGX530**: every earlier attempt faulted
on the first fetch, at the wrong page. The ladder's instruction 1 is `str #691, r0`
(`EVENT_TIMER`), and `str`/`ldr` are flagged in ISA.md's SGX530 delta as possibly encoded
differently. 0x08039000 is not a literal in the `str` word, so it isn't a simple
bits-as-target misdecode; the probe localizes it instead.

**Attempt 25: the minimal USSE probe** (`cores/sgx530_usse.py`). The event task is the
smallest program that exercises one of each early instruction, then self-loops, with no
`phas.i` (V0 showed `limm` runs without it):
- `st.a.d` A -> ctlblk 0x28 = 0xbeef1 (before the str);
- `str` -> `EVENT_TIMER` = 0xa5a;
- `st.a.d` B -> ctlblk 0x18 = 0xc0de2 (after the str);
- a `ba` self-loop.

Build: `python3 ukernel/build.py sgx530_usse …`, then mkukfw. Image 0x68 (104 B), sha256
cb18a5a7…, same through asm.py and `sgx-asm`; `--check` crc 0xac4ecbbd; packaged 0bcce2c7….
It has only the `ctlblk` patch (no `pdtable`; the probe uses neither the PD table nor init).

Read-back after the start:
- **ctlblk 0x28 = 0xbeef1, `EVENT_TIMER` = 0xa5a, ctlblk 0x18 = 0xc0de2, and a self-loop
  with no fault** (the probe times out, `INIT_STATUS` 0): the USSE runs `st.a.d`, `str` and
  `ba`. Then the attempt-24 fault was later, in the init path, and attempt 26 bisects init.
- **ctlblk 0x28 = 0xbeef1, `EVENT_TIMER` = 0, ctlblk 0x18 = 0, a b2 fault**: `str` is the
  first to derail. `str`/`ldr` encode differently on the SGX530; decode them from the
  hwt_* suites and the fault, and fix the assembler for the SGX530.
- **ctlblk 0x28 = 0 and a fault**: even `st.a.d` derails; the fault address is the first
  real clue to the SGX530's store encoding.

The self-loop holds the USSE; recovery reset first, and the start's teardown already holds
the cores in reset on a timeout.

## After the twenty-fifth attempt: phas.i must come first

Attempt 25 (log 2026-11-12-0322): even the first `st.a.d` didn't land. The b2 fault address
depends on the image — 0x09039000 (the USSE probe) vs 0x08039000 (the attempt-24 ladder),
differing by 0x01000000 with a common low 0x39000. An image-dependent garbage fetch, before
any instruction commits.

Both the probe and the diagnostic ladder put work **before** `phas.i`. The real microkernel
entry has `phas.i` as its very first instruction, and the SGX530 lacks
`USE_UNLIMITED_PHASES`. So the SGX530 most likely requires `phas.i` first, to declare the
task's phase before it fetches anything; starting with `limm`/`st.a.d` leaves the phase
undefined and the fetch garbage. The SGX545 (unlimited phases) is lenient, which is why the
ladder-first diagnostics worked in design but never on the SGX530. **The whole diagnostic
approach, not the microkernel, was the blocker for the last several attempts.**

**Attempt 26, two parts:**
1. **The real image.** Run the normal `sgx530` firmware (not a diagnostic build) on the
   `>> 8` driver. Its entry is `phas.i` first. Expected: the microkernel inits —
   `INIT_STATUS` 1, version 0x00010000, heartbeat moving. That is **stage 1 passing**. The
   kernel's status dump already reads init, version, wait and heartbeat.
2. **The confirmation probe** (`cores/sgx530_usse_phas.py`): the USSE probe with `phas.i`
   first, then `st.a.d` A, `str` `EVENT_TIMER`, `st.a.d` B, self-loop. If the markers now
   land (ctlblk 0x28 = 0xbeef1, `EVENT_TIMER` 0xa5a, ctlblk 0x18 = 0xc0de2) where attempt 25
   faulted, `phas.i`-first is confirmed as the SGX530's requirement.
   - Build: `python3 ukernel/build.py sgx530_usse_phas …`, then mkukfw. Image 0x70 (112 B),
     sha256 51fd3545…, same through asm.py and `sgx-asm`; `--check` crc 0x26a72325; packaged
     084bd775…. Only the `ctlblk` patch.

If part 1 inits, stage 1 is done and the next step is get-misc-info (microkernel.md stage 3)
and the first TA. If it doesn't but part 2's markers land, the fault is past `phas.i` in the
real entry, and the probe grows one real instruction at a time.

## After the twenty-sixth attempt: the real image's first load

Attempt 26 (log 2026-11-12-0326):
- **The real `sgx530` image** (phas.i first) reaches a **b1 data fault at 0x0fd79000**, held
  100-500 ms, with `INIT_STATUS`/version/heartbeat/`EVENT_TIMER` all 0. So it runs past
  phas.i and the limms and issues a data access that faults — the first load,
  `ld.a.d r3, r17, #42 [moe]` (the heartbeat). This is further than any attempt before.
- **The phas probe** still derails on a b2 code fetch (0x010b9000), no markers.
- **0x0fd79000 is fixed:** the same address attempt 24's CB15 = 0xec001 gave, from a
  different image and start. So it comes from the data pointer (r17 + offset), not the code
  placement.

phas.i-first clearly helped the real image. Two differences remain between it and the bare
probe: the real entry patches **both** r17 (ctlblk) and r14 (pdtable), and its first data op
is a **load**, not a store.

**Attempt 27: mirror the real preamble** (`cores/sgx530_usse_mirror.py`). phas.i, both
patches, the exact heartbeat load `ld.a.d r3, r17, #42 [moe]`, then store what the load read
to ctlblk 0x28, write `EVENT_TIMER`, self-loop.
- Build: `python3 ukernel/build.py sgx530_usse_mirror …`, then mkukfw. Image 0x60 (96 B),
  sha256 8d385fcd…, same through asm.py and `sgx-asm`; `--check` crc 0xe92af0c4, patches
  0x8 ctlblk and 0x10 pdtable; packaged 9566c0a8….
- **Fault at 0x0fd79000 (b1):** the first load is the fault; the `[moe]` addressing or r17
  is wrong on the SGX530. The next step decodes the load's effective-address formula.
- **ctlblk 0x28 = 0 (the heartbeat) and `EVENT_TIMER` 0xa5a, self-loop, no fault:** the load
  and the stores work; the real image's fault is past the first load, and the probe grows
  one instruction at a time.

If the load addressing is wrong, it is likely an ISA difference in `ld.a.d`/`st.a.d`
offset scaling or the `[moe]` mode; document it C and hand the encoding to cleanroom-isa per
the clean-room rule.

## After the twenty-seventh attempt: [moe] addressing

Attempt 27 (log 2026-11-12-0332): the mirror probe reproduced the real image's fault
exactly — b1 at 0x0fd79000, on `ld.a.d r3, r17, #42 [moe]`, with no marker. r17 (ctlblk) is
0x0f003000, so the effective address 0x0fd79xxx is not `r17 + 42 × any clean unit`
(0x0fd79000 − 0x0f003000 = 0x00d76000, not a multiple of 42). So the load's address isn't a
plain base-plus-scaled-offset; `[moe]` is pulling in the USSE's MOE state.

The SGX545 image uses `[moe]` on 409 loads/stores and never runs an explicit MOE setup: it
relies on the default MOE state at program start, which hwt_p1 confirms is increment 1 on
the SGX545. The SGX530's default is untested. If it differs, every `[moe]` access is
corrupt, which is exactly the first `ld.a.d` failing.

**Attempt 28: the same load without `[moe]`** (`cores/sgx530_usse_nomoe.py`): the attempt-27
mirror with `[moe]` dropped from the `ld.a.d` and `st.a.d`.
- Build: `python3 ukernel/build.py sgx530_usse_nomoe …`, then mkukfw. Image 0x60 (96 B),
  sha256 410484bb…, same through asm.py and `sgx-asm`; `--check` crc 0x3e0a1c92; packaged
  911f1f0a….
- **The load now lands** (ctlblk 0x28 = 0, `EVENT_TIMER` 0xa5a, self-loop, no fault):
  `[moe]` is the cause — the SGX530's default MOE/address state differs from the SGX545's.
  The fix is a SGX530 MOE setup at task start (a `setfc`/MOE-control instruction), a
  530-only image change that leaves the SGX545 golden untouched. Decode the needed state
  from the hwt_* MOE suites, document it C, and the encoding goes to cleanroom-isa.
- **It still faults at 0x0fd79000**: `[moe]` isn't it; the base r17 or the `ld.a.d` offset
  encoding differs. Next is an offset sweep (vary #42) to read the effective-address
  formula, as the USE_CODE_BASE bits were read.

## After the twenty-eighth attempt: not [moe], not a scaled offset

Attempt 28 (log 2026-11-12-0336): the no-`[moe]` probe faults at the same 0x0fd79000. So
`[moe]` is not the cause, and 0x0fd79000 is a deterministic function of r17 (0x0f003000) and
the `ld.a.d` that is neither `r17 + 42 × a clean unit` nor MOE-derived.

**Attempt 29** isolates one `ld.a.d`, gated by `str` (a register write, not `a.d`
addressing, so it signals success without depending on the thing under test). `EVENT_TIMER`
= 0xa5a means the load did not fault; a fault names the effective address.

| Probe | `ld.a.d` | Asks |
|---|---|---|
| `sgx530_usse_ld0` | `r3, r17, #0` | does the offset matter? If it still faults at 0x0fd79000, the address doesn't depend on the offset |
| `sgx530_usse_ld0nb` | `r3, r5, #0`, r5 = `limm` 0x0f004000 | does the base register matter? If the fault stays at 0x0fd79000, the base operand isn't read from the register |

- Build each: `python3 ukernel/build.py sgx530_usse_ld0 …` / `… sgx530_usse_ld0nb …`, then
  mkukfw. `sgx530_usse_ld0`: image sha256 675b7843…, crc 0xc7a8e64a, packaged 7013b5a6….
  `sgx530_usse_ld0nb`: image d0ce59a3…, crc 0xd7ce33b4, packaged e1d819da…. Both same
  through asm.py and `sgx-asm`.
- Reading:
  - **ld0 passes (`EVENT_TIMER` 0xa5a):** r17 + 0 is a valid address; the offset scaling is
    wrong. Next: an offset sweep to read the unit.
  - **ld0 faults at 0x0fd79000:** the offset is irrelevant; the base/address comes from
    elsewhere. Then ld0nb separates the register from a fixed source.
  - **ld0nb's fault moves with r5, ld0's with r17:** the base register works, so it's the
    offset. If both stay at 0x0fd79000 whatever the base, the `a.d` addressing reads its
    base from a fixed place (an internal/index register), not the operand — an ISA
    difference to decode from the hwt_* suites, document C, and hand to cleanroom-isa.

## After the twenty-ninth attempt: limm, not ld

Attempt 29 (log 2026-11-12-0340) found the real cause: **`limm` loads the wrong value on the
SGX530.**
- **ld0** (`ld.a.d r3, r17, #0`): b1 fault at 0x0fd79000 — the offset doesn't matter.
- **ld0nb** (`ld.a.d r3, r5, #0`, r5 = `limm` 0x0f004000): b1 fault at **0x0c004000**. The
  base register is read, and the loaded value is 0x0c004000, not 0x0f004000.

0x0c004000 = (bits 26-31) | (bits 0-20) of 0x0f004000, with **bits 21-25 dropped** (worth
0x03000000, the exact difference). So `ld` is fine; the SGX530 decodes the `limm` value
field differently from the SGX545, reading bits 21-25 from a different place in the
instruction word (as 0, where the SGX545 encoding puts them). Every large `limm` in the
image — and the patched r17 — loads wrong, which is why the first `limm`-built address
faulted. Tag C (attempt 29). The low field (0-20) and bits 26-31 are correct.

**Attempt 30: the `limm` field sweep** (`cores/sgx530_limm_b21.py` … `b27.py`). Each `limm`s
r5 = a single bit 21-27, then `ld.a.d r3, r5, #0`; the fault page is the value the SGX530
loaded, so it reads where each value bit lands.

| Probe | r5 value | img sha256 (16) | packaged (16) |
|---|---|---|---|
| b21 | 0x00200000 | 95130a769f8fccdb… | 38e9b5547babb439… |
| b22 | 0x00400000 | 2d012fa428a416c4… | c43d04a4e107c711… |
| b23 | 0x00800000 | ac21851a3603ef16… | eaf9700b460e8b79… |
| b24 | 0x01000000 | 72ae46039b9b122a… | 53f4edc3c9710c6a… |
| b25 | 0x02000000 | 8230559bf0473b63… | aedb1b4b53b4c8e6… |
| b26 | 0x04000000 | 073cbbc7686de92a… | 79c09d2225fefa74… |
| b27 | 0x08000000 | 9a9a05867a58946a… | ecfdf000ac1d8f09… |

Build each with `python3 ukernel/build.py sgx530_limm_b<n> …` then mkukfw; all same through
asm.py and `sgx-asm`. `EVENT_TIMER` 0xa5a means the value decoded to a mapped address (no
fault); otherwise the fault page is the decoded value. A bit at its own weight is correct;
one landing elsewhere, or faulting at 0, shows the SGX530's field position for that bit.

With the sweep, write the SGX530's `limm` value-field layout into ISA.md (C); the encoding
goes to cleanroom-isa for `sgx.xml` and to the driver for `fw_limm`; then the image builds
with `ISA_CORE = 530` and gets its own golden. The SGX545 golden is untouched.

## After the thirtieth attempt: ld.a.d's source operand, not limm

Attempt 30 (log 2026-11-12-...) **retracted the limm reading.** The seven `sgx530_limm_b21..b27`
probes each `limm`ed a different single bit into r5, then `ld.a.d r3, r5, #0`, and **all
faulted at the same 0x0c004000** — the `limm` value doesn't matter. So `limm` is fine
(ISA.md retraction, commit 628f2415).

The fault address depends on **which register `ld.a.d` names**, not its contents:
- `ld.a.d r3, r17, #0` → 0x0fd79000;
- `ld.a.d r3, r5, #0` → 0x0c004000 (any r5).

So the SGX530 derives `ld.a.d`'s base from the source-operand field (the register number),
not from the register's contents. The offset doesn't matter either (attempt 29). This is an
`ld.a.d` source-operand decode difference from the SGX545 (which loads `[contents +
offset]`).

**Attempt 31: the register-number sweep** (`cores/sgx530_ldreg_r0.py` …). `ld.a.d r3, rN,
#0` with the register value unused; the fault page is the address the SGX530 derives from
naming rN. r5 and r17 are known (0x0c004000, 0x0fd79000); these fill in the rest.

| Probe | reg | img sha256 (16) | packaged (16) |
|---|---|---|---|
| r0 | 0 | fdf991dfae928e52… | a1f0273a986cfd72… |
| r1 | 1 | 10c357de7fb026ed… | 9bf16d19208814da… |
| r2 | 2 | e306298f6590af2b… | 35f83e3a8f747b3a… |
| r3 | 3 | fcf0c79f98111276… | a9c77fb5bdab5199… |
| r4 | 4 | de7729ecacc0688a… | 63c98cbec606820d… |
| r6 | 6 | 375fa4590dc1ec5e… | 45d33975e862cb01… |
| r8 | 8 | 95ce1938a9843280… | d60bd70036780c14… |
| r16 | 16 | 9c284028437ded28… | 8576a33a8d1b85ca… |
| r31 | 31 | ec3ec17493749fef… | 1a46787cacb89952… |

Build each `python3 ukernel/build.py sgx530_ldreg_r<n> …` then mkukfw; all same through
asm.py and `sgx-asm`. `EVENT_TIMER` 0xa5a = the derived address was mapped (no fault); else
the fault page is that address.

**And the which-register probes** (`cores/sgx530_ldpre_r{0,1,2,4,5,17}.py`, the kernel
session's refinement): each preloads r0..r22 = (N+1)<<16 (distinct, unmapped, all below
2 MiB) then `ld.a.d r3, rK, #0`. A fault at (M+1)<<16 names the register M the SGX530 read
for operand rK. Run these first:
- fault at (K+1)<<16 → it reads the named register (contents), so the base is right and the
  fault comes from elsewhere;
- fault at (M+1)<<16, M a fixed offset from K → a register-field shift;
- fault at none of the preloaded values → the operand field is used as an address, and the
  `sgx530_ldreg_r*` sweep maps the register number to that address.

Then document the `ld.a.d` operand decode C in ISA.md and hand it to cleanroom-isa.

## After the thirty-first attempt: limm doesn't write its register (and str is suspect)

Attempt 31 (log 2026-11-12-0350): the register-number sweep has **no pattern** — r0 →
0x0055d000, r1 → 0x0722a000, r2 → 0x003e1000, r4 → 0x09e06000, r6 → 0x0503f000, r8 →
0x08779000, r16 → 0x002ba000, r31 → 0x0d04f000, plus r5 → 0x0c004000 and r17 → 0x0fd79000.

So `ld.a.d` reads the named register correctly, and each register holds a **stable
power-on/reset leftover value** at task start. r5 (0x0c004000) and r17 (0x0fd79000) kept the
same page across three boots **and** across whatever `limm` wrote into them before the load.
So **`limm` never wrote its destination** — that is why the `limm` value was irrelevant
(attempt 30) and the leftovers are stable. `ld.a.d`'s operand decode is fine; the question is
`limm`'s destination encoding, or whether `limm` executes at all on the SGX530.

(Numbering: the `sgx530_ldpre_*` preload probes ran as attempt 32; the `limmwr` test below is
attempt 33, `aluwr` attempt 34.)

**ld0nb already proved `limm` doesn't write (C).** Attempt 29's `ld.a.d r3, r5, #0` after
`limm r5, #0x0f004000` faulted at r5's leftover 0x0c004000, not the limm value — so the load
read r5's unwritten leftover. The fault page, not `str`, is the readout, and it already
settles that `limm` does not write its destination.

**Attempt 33: `limm` write via `str`** (`cores/sgx530_limmwr_r{5,17,0}.py`). `phas.i`, then
`limm rK, #0xa5a`, then `str rK` to `EVENT_TIMER` (`str` reads the register back). No load
before it, so nothing faults first. **Result (log 2026-11-12-...):** no fault, the task
self-loops, and `EVENT_TIMER` stays 0 — so `limm`, `str` and `ba` all execute without
faulting, but `str` to `EVENT_TIMER` reads back 0 (its write doesn't take, or isn't
visible). So attempt 33 can't separate `limm` from `str`, and `str` is suspect too. The
fault-page readout (below) sidesteps both.

| Probe | reg | img sha256 (16) | packaged (16) |
|---|---|---|---|
| limmwr_r5 | r5 | dbec9d41ef880909… | 4a6c94ae0405f1fe… |
| limmwr_r17 | r17 | f9d92d918d41bf9d… | a9dcba48f2c9197f… |
| limmwr_r0 | r0 | c78e65c5c998db18… | 0b18629005dbeee9… |

Build each then mkukfw; all same through asm.py and `sgx-asm`. Read `EVENT_TIMER`:
- **0xa5a:** `limm` wrote the register and `str` read it — both work. Then the real fault is
  elsewhere, and the leftover reading needs revisiting.
- **the register's leftover** (r5 0x0c004000, r17 0x0fd79000): `limm` did not write it. Then
  the SGX530 decodes `limm`'s destination field, or the whole `limm`, differently — the next
  sweep is `limm rK` for several K, each read back by `str`, to find where the write lands.
- **0 or something else:** `str` itself may not read the register as expected; cross-check
  with a second register.

The which-register probes (`sgx530_ldpre_r*`) confirmed it (C): each faulted at exactly its
register's unwritten leftover, never at a preloaded `(N+1)<<16` value, so preloading with
`limm` reached no register.

**Attempt 34: does a non-`limm` write land?** (`cores/sgx530_aluwr.py`) — the clean test,
with the fault page as readout (no `str`). `or r5, r5, r17` (r5's leftover 0x0c004000 |
r17's 0x0fd79000 = 0x0fd7d000), then `ld.a.d r3, r5, #0`. A fault at **0x0fd7d000** means the
ALU wrote r5; at **0x0c004000** means it did not. Image 0x48 (sha256 81dd5785…, same through
asm.py and `sgx-asm`; crc 0x1dd7e9fa, packaged 18ce50ba…).
- `limmwr` leftover + `aluwr` 0x0fd7d000 → the register file writes, but `limm` specifically
  does not. The fix is a SGX530 `limm` decode (destination field or opcode), handed to
  cleanroom-isa.
- both leftover → no write lands at all; `str`/the write path is broader, and `ld` reading
  fine would need rechecking.

This is the real blocker: with `limm` not writing, every constant and address the microkernel
builds is its register's leftover. Decode it C and hand the behavior to cleanroom-isa.

## After the thirty-fourth attempt: zero temporaries (the likely cause)

Attempt 34 (log 3296b7ed): `aluwr` (`or r5, r5, r17`, then `ld r5`) faulted at **0x0c004000**,
r5's leftover, not 0x0fd7d000. So the `or` didn't write r5 either — **no register write
lands**, not just `limm` (attempt 33, with `str` also not taking). Source operands decode
fine (attempt 31: `ld rN` reads register N). So writes go nowhere the reads see.

**The hypothesis (the coordinator's, the likely cause): the task runs with zero
temporaries.** On the SGX545 the DOUTU word 0 carries the temp count in bits 24-31
(`(n+3)>>2`, PDS.md — not in any GPL header, so **U** on the SGX530). The driver sets it to 6
(24 temps; attempt 24's `ukernel_temps`). If the SGX530 reads the temp count from a
different position — as it read the code address from `>> 8` and the segment elsewhere —
the task gets **0 temporaries**: every write to an r-register is dropped, and reads return
the register bank's leftover. That matches attempts 31-34 exactly, and it is one field, not
a whole destination-encoding difference.

**Attempt 35: sweep the DOUTU temp-count field position** (driver-side, the kernel session
builds DOUTU word 0). For the EDM entry DOUTU, put the temp value (e.g. 6, or a larger
count) at candidate positions — the SGX545's b24-31, and shifted down — one per variant,
leaving the code-address bits (0-23) intact (the entry's offset is 0, so b4-23 are free).
Readout: the `aluwr` image (81dd5785…, packaged 18ce50ba…), by its fault page:
- **0x0fd7d000:** that position is the SGX530's temp-count field — the `or` now writes r5.
  Set the temp count there, and register writes (so `limm`, `imae`, the whole microkernel)
  start working.
- **0x0c004000:** still 0 temps; try the next position.

**The cheapest, most likely variant first: temp field = 8 (bit 27).** `pvrsgx_doutu` sets
`0x8000000` (bit 27). The attempt-24 override is `(doutu & ~GENMASK(31,24)) | FIELD_PREP(…,
DIV_ROUND_UP(ukernel_temps, 4))`, and `GENMASK(31,24)` includes bit 27, so `ukernel_temps =
24` writes field value 6 and **clears bit 27**. PDS.md: the microkernel's own entries
(pds0/2/3/4) have bit 27 set = field value 8 = 32 temporaries, and the SGX545 keeps it (it
has no override). So attempt 24's reduction to 24 temps cleared the bit the vendor entry
carries, and that alone could be why 31-34 saw 0 temps. Variant 0: `ukernel_temps = 32`
(field 8, bit 27 set). If `aluwr` then writes (0x0fd7d000), re-run the real `sgx530` image
with it — likely init completing. Only if that's flat do the group ORs below matter.

Note (the kernel session's constraint): b0-3/b4-7/b16-18 move the code fetch (attempts
21-22), so the free bits for a moved field are b8-15, b19-23 and b27-31; the sweep ORs each
group all-ones onto the normal word (0x0000ff00, 0x00f80000, 0xf0000000) and bisects the one
that works.

If a position works, re-run the real `sgx530` image: with temporaries, `limm` and the stores
should land and init can complete. Also worth a look: `phas.i`'s `temps4` field (ISA.md) as
an alternate temp source, since the entry's `phas.i #0, #0` carries 0 there.

## After the thirty-fifth attempt: phas.i's temps4

Attempt 35 (log 16baa779) was flat on the DOUTU temp field. `aluwr` faulted at 0x0c004000
for field 6, field 8 (bit 27), and `|= 0xff00 / 0xf80000 / 0xf0000000`; the real image faulted
at 0x0fd79000 for all except `|= 0xff00` (b8-15), which ran without faulting — because b8-15
are code-offset bits below the fault page (attempt 21), so it just moved the entry inside
page 0. So **the task's temporaries do not come from DOUTU word 0.** Restoring bit 27 (field
8) didn't help either.

**Attempt 36: `phas.i`'s `temps4` field.** On the SGX545, `phas.i #pc, #temps4` carries the
temporaries/4 (ISA.md), and the entry's `phas.i #0, #0` has 0 there. If the SGX530 allocates
the task's temporaries from `phas.i` rather than the DOUTU, `temps4 = 0` gives 0 temps —
matching attempts 31-35. `cores/sgx530_aluwr_ph{8,6,63}.py` is `aluwr` with `phas.i #0,
#temps4` for temps4 = 8 (32 temps), 6 (24), 63 (max):

| Probe | temps4 | temps | img sha256 (16) | packaged (16) |
|---|---|---|---|---|
| aluwr_ph8 | 8 | 32 | 1674add53368bbf0… | 1793ae6426fdde95… |
| aluwr_ph6 | 6 | 24 | a0ed44b23da3a86e… | d9ab5f7caa9ec807… |
| aluwr_ph63 | 63 | 252 | 982cc613659d369d… | a789287df9c7271b… |

Build each then mkukfw; all same through asm.py and `sgx-asm`. Readout is the fault page:
- **0x0fd7d000:** `phas.i`'s `temps4` is the SGX530's temp source — the `or` now writes r5.
  Then set the entry (and every task's) `phas.i` `temps4` on the SGX530, a 530-only image
  change via a per-core flag; the SGX545 `phas.i #0,#0` is unchanged, golden intact. Re-run
  the real image with it: likely init completing.
- **0x0c004000:** still 0 temps; `phas.i` isn't it either.

**Attempt 36 result (log 2026-11-12-...):** `aluwr_ph8/ph6/ph63` all faulted at 0x0c004000.
So `phas.i`'s temps4 isn't the temp source. Both temp-count hypotheses (DOUTU word 0,
`phas.i` temps4) are ruled out. **The strongest remaining lead is the host-side init diff
below** (the 530 writes 3 of the 545's 16 setup registers) — a driver-side change, the next
step (attempt 37). A `setfc`/MOE-style USSE setup, or `phas.i`'s rate/phase semantics, are
fallbacks only if the init writes don't help.

## The host-side init diff (the coordinator's survey, likely the register-file setup)

The 545 `init_regs` (part 2) writes **16** setup registers; our 530 `init_regs` writes **3**.
All of these offsets are unnamed in both the 530 and 545 GPL headers — they come from the
captured 545 srvinit script (SRVINIT.md), so **G**. The 530 was stripped to the minimum plus
0xa74 (attempt 18). Missing on the 530 (13), with the 545's values:

| offset | 545 value | note |
|---|---|---|
| 0x818 | 0x7ff | USE/partition allocation (G) — prime temp-setup suspect |
| 0x810 | 1 | " |
| 0x814 | 0xffff | " |
| 0x800 | 0x30 | " (0x800 is `EVENT_STATUS_TPC_CLEAR`'s offset on the 530; the write is a different block) |
| 0x804 | 0xffff | " |
| 0x4e8 | 0x100 | (G) |
| 0xa00 | 0x7c000 | (G) |
| 0x13c | 0 | (G) |
| 0xa7c | 0 | (G) |
| 0xa80 | 0 | (G) |
| 0xa78 | 0x2c | tested alone in attempt 18 (no PDS effect) |
| 0xaac | 0x45 | " |
| 0xabc | 0xe | `PDS` on the 530 (K); tested alone, no effect |

The 530 writes only `EVENT_HOST_ENABLE`, `EVENT_HOST_ENABLE2`, `0xa74 = 0x208200`.

**The hypothesis (strong): the missing writes include the host-side USE register-file /
temp-partition setup the task needs.** 0x810/0x814/0x818 = 1/0xffff/0x7ff and 0x800/0x804 =
0x30/0xffff read like resource allocation. Without them the task has no register file, so
writes drop and reads return leftovers — attempts 31-35 exactly. Attempt 18 tested the PDS
block (0xa78/0xaac/0xabc/0xa00) only against "does the PDS run," never against "do writes
land," and never wrote 0x810-0x818 / 0x800-0x804 / 0x4e8 / 0x13c at all.

**Attempt 37 (approved): write the 545 `init_regs` offsets the 530 skips.** The coordinator
approved, under the standing go, these exact offsets at the 545 captured values (G): 0x800,
0x804, 0x810, 0x814, 0x818, 0x4e8, 0x13c, 0xa00, 0xa7c, 0xa80, plus 0xa78/0xaac/0xabc. (These
are ask-first: unnamed in both GPL headers. Approval is per these offsets, not implied by the
diff.) Run it as a driver-side sweep, not folded into `sgx530_init_regs` until a variant
proves what's needed:
- each offset read once before the first write, logged;
- the recovery reset first;
- the full set together first, with `aluwr` as the readout;
- if writes land (0x0fd7d000), bisect starting with 0x810/0x814/0x818, so the table keeps
  only what's needed;
- stop at the first hang; a plug cycle only to recover, the board owner told.

Record the values G (545 capture) and the effect C once measured. If writes land, re-run the
real image: likely init completing.

## After the thirty-seventh attempt: consolidation — the USSE executes but no write retires

Attempt 37 (log b487d86a): the init-regs writes are flat, and the read-backs close the lead
— **0x810/0x814/0x818/0x800/0x4e8/0xaac don't latch** (read 0 after the write), so those
registers aren't implemented on the SGX530 and can't be the register-file setup. (0x804,
0xa00, 0xa78, 0xabc do latch.) Read-backs recorded C in sgx530.md; the host-side init-regs
lead is closed.

**The consolidated finding (C, from attempts 24-37):**
- The open microkernel's USSE code **executes** on the SGX530: `phas.i`, `ld.a.d` (it
  generates the address and faults/loads), an unconditional `ba` (the self-loop runs), and
  instruction fetch all work, with the code base at `>> 8` and `phas.i` first.
- **No register write retires.** `limm`, an ALU `or`, and `str` to `EVENT_TIMER` all fail to
  change the register a later read sees. Reads (`ld.a.d rN`) return each register's stable
  power-on leftover, the same across boots (attempts 31-34).
- **Ruled out as the cause:** the DOUTU word-0 temp field (35), `phas.i`'s temps4 (36), and
  the host-side init-regs setup (37). DOUTU word 1 = 2 and word 2 = 0 are flag/unused, not a
  temp field (PDS.md); the SGXMKIF init struct has no register-file field.

So the SGX530 runs the task but writes results nowhere a read sees — a register-file /
write-back / bank difference, not a single misplaced field. The two readings left:
1. **Writes are dropped** — the task has no writable register file (0 temporaries), from a
   setup the SGX530 needs that neither the DOUTU, `phas.i`, nor the host init provides.
2. **Writes land in a bank reads don't see** — compute writes go to one bank, `ld.a.d`
   reads its base from another, for the same register number.

**Direction (for the coordinator):** our event entry uses **temporaries** as the pointer
base (UKERNEL.md: "a reserved temp, not a secondary attribute", because the bare-DOUTU event
context has no SA DMA), relying on SGX545 behavior. The SGX530 lacks `USE_UNLIMITED_PHASES`
and has 2 USE pipes (not 4); its DDK microkernel may structure the EDM task differently
(phases, or primary/secondary attributes set by the PDS rather than temporaries). Rather
than more single-register sweeps, the next step is likely to compare how TI's GPL/DDK SGX530
EDM task is built, or to decode the SGX530's USSE register-bank/write-back with cleanroom-isa.
A cheap probe meanwhile: a PDS `DOUTA` that writes a primary attribute before the DOUTU,
then a USSE read of it — if attributes are visible where temporaries aren't, the microkernel
should use them on the SGX530.

## Attempt 38: instruction pairing (the GPL feature-flag lead)

The GPL `sgxfeaturedefs.h` gives the likely cause. The SGX545 has
`SGX_FEATURE_USE_NO_INSTRUCTION_PAIRING`; the **SGX530 lacks it** (its block has only
`NUM_USE_PIPES 2`, `AUTOCLOCKGATING`, 28-bit space). So the SGX530's USSE **pairs /
schedules adjacent instructions**, and the open microkernel is written for the no-pairing
SGX545. If the SGX530 pairs a write with the next, dependent instruction — `or r5, r5, r17`
with the following `ld.a.d r3, r5` — the `ld` reads r5's **pre-write** value. That is exactly
"reads return stable leftovers, writes don't appear": the write happens, the paired read
sees the old value. ISA.md: `nosched` (h11/h22) suppresses this, and the vendor compiler sets
it around a register write (ISA.md "the compiler puts `nosched` on the words between an
internal register's write"); the SGX545 never needs it.

**The test: space the write from the read with nops** (`cores/sgx530_aluwr_nop{1,2,4,8}.py`):
`or r5, r5, r17`, N nops, `ld.a.d r3, r5, #0`. If the `ld` then faults at **0x0fd7d000** (the
`or` result), a scheduling/pairing hazard is the cause, and the write was landing all along.

| Probe | nops | img sha256 (16) | packaged (16) |
|---|---|---|---|
| nop1 | 1 | d296a90ac9be258b… | afb04b326b7b272f… |
| nop2 | 2 | 2e8d6976207d8e3c… | b5e3e7fad93dbd58… |
| nop4 | 4 | b5bc6ad91895e152… | 784507c19b3b1ee4… |
| nop8 | 8 | ecb151283f792a27… | 57476af8622e0928… |

Build each then mkukfw; all same through asm.py and `sgx-asm`. The smallest N that gives
0x0fd7d000 is the hazard distance.

**If it's pairing:** the fix is per-core — on the SGX530, break scheduling between a write
and its dependent use, either with `nosched` on the writing instruction or by spacing
(the assembler/`sgx.xml` for `nosched`, via cleanroom-isa; or the microkernel emits the
barriers for the SGX530 build). The SGX545 image is unchanged. Then re-run the real image:
with the hazard handled, `limm`, the stores and init should work. This also fits why *every*
write looked dropped: the microkernel is dense with write-then-use pairs.

## After the thirty-eighth attempt: pairing ruled out

Attempt 38 (log ...): `aluwr_nop1/2/4/8` all faulted at 0x0c004000 — up to 8 nops between the
`or` and the `ld` don't reveal the write. And attempt 32's `ldpre_r5` ran 23 `limm`s before
its `ld r5` and still saw the leftover. A scheduling/pairing window of 8-23 instructions isn't
the cause; **pairing is out**. So the write genuinely does not retire anywhere a read sees,
even with large separation.

**What's now established (C):** the USSE executes, and no register write (by `limm`, ALU `or`,
`str`, or an `ld` destination) is ever visible to any read; reads return stable leftovers.
Ruled out: the DOUTU word-0 temp field (35), `phas.i` temps4 (36), host init-regs (37,
several unimplemented), and instruction pairing (38). This is a register-file / write-back
difference, and the cheap single-variable hypotheses are exhausted.

**Attempt 39 result (cheap, low registers) — flat.** `aluwr_dst0-3` all faulted at the
destination's own leftover (r0 0x0055d000, r1 0x0722a000, r2 0x003e1000, r3 0x08439000),
never the `or` result. So no low range of registers is writable either; the cheap register
probes are closed. The `pa0` primary-attribute probe below (the USSE reader `sgx530_pa0.py`,
image d69730f4…, packaged f59ce697…, built and verified; `ld.a.d r3, pa0, #0`) pairs with the
driver's `pds0` DOUTA of 0x0e850000 into PA0: a fault at 0x0e850000 = PDS-set attributes are
visible where temporaries aren't. The probes:

**Attempt 39 (cheap, the kernel session's angle): low registers.** `cores/sgx530_aluwr_dst{0,1,2,3}.py`:
`or rD, rD, r17` then `ld rD` for D = 0-3, to check whether a low register is a writable
temporary where r5 isn't. Expected `or` result (fault page) if it writes: r0 0x0fd7d000, r1
0x0ff7b000, r2 0x0ff7d000, r3 0x0ff7d000; the register's leftover if not (r0 0x0055d000, r1
0x0722a000, r2 0x003e1000, r3 0x08439000). Images: dst0 6f023fd2…, dst1 db215ee8…, dst2
fd6a024d…, dst3 854e77b8… (packaged 43860788…, abc1853a…, be8af3a4…, a90c2e2b…).

**Direction (for the coordinator).** The cheap probes are spent. Two substantive options:
1. **The PDS primary-attribute probe (the original (c)):** the driver's `pds0` does a `DOUTD`
   of a known value into a primary attribute, then the USSE reads it. If attributes
   (PDS-set) are visible where temporaries (USSE-written) aren't, the SGX530 microkernel
   should use attributes for its pointers, not temporaries — a 530-specific restructure. This
   is the most informative remaining probe; it's joint (driver `pds0` + a USSE-read image).
2. **An AM335x vendor capture.** TI's DDK SGX530 microkernel works; a capture of how it sets
   up the EDM task (temps, phases, register file) would show the answer directly, the way the
   SGX545 capture did. If a board can be obtained, capture beats continued probing here.

## After the fortieth attempt: probe (c) was malformed, not answered

Attempt 40 (log 5730ea81), the `pa0` primary-attribute probe with the kernel session's
`pds0` DOUTA:
- Baseline (normal `pds0`): `ld.a.d r3, pa0, #0` faulted b1 at **0x0f2d9000** — PA0's own
  stable leftover (C). So `pa0` reads work; PA0 just holds a leftover with no DOUTA.
- DOUTA variant (2 data rows, 0xa6c = 2, 0xa70 left at 6): faulted b2 at **0x0f400000** =
  the usse_pixel heap base = code base 0 under `>> 8`. So this variant's DOUTU issued with
  word 0 ≈ 0 (code base 0, offset 0) — the task never fetched our image. **The probe was
  malformed, not answered:** the PDS didn't build a valid DOUTU word.

Likely cause (G, the kernel session): 0xa70 is the program/data length in dwords and stayed
6, so the row holding the DOUTU word 0 wasn't loaded and the MOVS sourced 0. The fix is the
same probe with **0xa70 = 8** (enough for the 2 data rows plus the code), which also tests
whether 0xa70 is the data size. The `sgx530_pa0` reader (image d69730f4…) is correct and
unchanged; only the driver's `pds0` data length changes. A fault at 0x0e850000 would then be
the real "attributes are visible" result; another code-base-0 fault would mean the DOUTU is
still malformed.

## After the fortieth attempt (rerun): PDS-written attributes work (C)

Attempt 40 rerun (log 84a3c5a0) gave a real PA0 result:
- A DOUTA variant at 10:17 ran its DOUTA (value 0x0e850000, ctrl 0 → PA0) before its broken
  DOUTU. ~9 minutes and several resets/unload/suspend/reload later, variant 0 (normal `pds0`,
  **no** DOUTA) read `ld.a.d r3, pa0, #0` = **0x0e850000** — the value the earlier DOUTA
  wrote. (Variant 0 had earlier read PA0's leftover 0x0f2d9000, before any DOUTA.)
- The two-row DOUTA program with 0xa70 = 6 and 8 both still faulted at code base 0: **0xa70
  is not the data length** (that guess is unsupported); the two-row program's DOUTU word
  still comes out 0.

**Confirmed (C):** the PDS's DOUTA writes PA0; the task's load through PA0 reads what the PDS
wrote; PA0 persisted across the recovery resets, the module unload, runtime suspend and the
reload. So **PDS-written primary attributes are visible to the task, where task-written
temporaries are not** — the first register file that works, and the first supporting fact for
the pointers-in-attributes direction. (It also fits the "stable leftovers": the register-file
contents persist across our resets and power-gating.)

**Open:** why the two-row DOUTA program's DOUTU word comes out 0 — likely the second data
row's layout under the SGX530's PDS interleave. A one-row variant (DOUTA from D0(0)/D0(1),
the DOUTU word in D1(0)/D1(1), no second row) would give a clean same-run confirmation.

**The design question (for the proposal):** the attribute result is encouraging, but the
microkernel needs **task-writable scratch** for its logic (the heartbeat `imae r3, r3, #1`,
dispatch, address arithmetic), not just readable pointers. Attributes are PDS-written.
Whether the microkernel can run without a task-writable register file, or whether another
file (outputs `o`) is task-writable, is the next question — see the restructure proposal.

## After the forty-first attempt (log `st41`, build `f85c22d9`): outputs are task-writable (C)

Attempt 41 ran the hinge in one slot: probe 2 (the output-write test) on the plain driver,
and probe 1 (the one-row PA0 DOUTA confirmation) across a three-variant sweep. Board up 1:51,
same boot as attempt 40.

**Probe 2 — `sgx530_aluwr_o0` (image 3d8e8a72…, pkg c30c27a1…, crc 0x8050de8b):** `or o0, r5,
r17` then `ld.a.d r3, o0, #0`. b1 fault at **exactly 0x0fd7d000** = r5's leftover 0x0c004000 |
r17's 0x0fd79000. The `or` wrote `o0` and the dependent `ld` read it back. **Output registers
are task-writable (C)** — the first task-side register write to retire on the SGX530. The
output bank is usable scratch where the temporary bank is not. This is the hinge the
restructure rested on: it is positive, so the restructure is viable (see
[sgx530-attr-restructure.md](sgx530-attr-restructure.md)).

**Probe 1 — `sgx530_pa0` reader across the sweep:**
- Baseline: b1 at 0x0e850000 — PA0 still holds attempt 40's 10:17 DOUTA value, ~19 minutes
  and several reloads later. Confirms the attribute persistence again.
- DOUTU `w1 = 0` control: b1 at 0x0e850000 — a task with DOUTU word 1 = 0 starts normally.
- One-row DOUTA `[V=0x0e860000, 0, 0, doutu]`: b2 at 0x0f400000, **code base 0 — the same
  failure as the two-row form.** With a DOUTA before it, the DOUTU issues with word 0 ≈ 0 in
  both the one-row and two-row layouts, while the same DOUTU words *without* a preceding DOUTA
  run. So the earlier "second data row's interleave" guess is **not** the cause.

A plain PA0 baseline load after the one-row DOUTA (10:38 UTC, same boot, log 41 / 8f8e2933)
read **0x0e860000**: the one-row DOUTA **did** write PA0 (C). So the DOUTA lands; it is the
DOUTU after it that issues at code base 0.

**Reading (G):** the fault is not the PDS data-row interleave (one- and two-row forms fail the
same way); it is a **DOUTA-before-DOUTU** effect — a DOUTA in the same PDS program disturbs the
following DOUTU's word 0.

**Correction to the earlier note:** an earlier version of this entry said the driver's `pds3`
sets PA0 on the SGX530 and so attribute setup is unblocked. That is wrong. On the SGX530 the
driver's event register points at **`pds0` (a bare DOUTU + HALT, `pds_ours()`); `pds3` has
never run there.** Attempt 40's PA0 write was a **standalone probe DOUTA program**, separate
from the launch DOUTU, and the attribute **persisted** between the two invocations. So **no
single program has yet been shown to do a DOUTA then a DOUTU on the SGX530** — the one thing
the restructure's attribute setup needs. This is an open problem, analysed in
[sgx530-attr-restructure.md](sgx530-attr-restructure.md) §6.

**Net:** the microkernel now has a task-writable file (outputs). Attribute *input* works
(DOUTA writes PA0, and it persists), but DOUTA-then-launch in one program is unproven on the
530 and is the next PDS problem to solve (or to sidestep via the observed persistence).

## After the forty-second attempt (11:06-11:08 UTC, f85c22d9's driver): all outputs writable (C)

The first attempt-42 build (the store-based `outsweep`, 5729e2bd) was never run: its stores used
`r17` as the base through `a.patch(17, "ctlblk")`, a `limm` that does not retire on the 530, so
`r17` stays its unmapped leftover and the stores would fault before reaching the scratch words —
and the `or` needs that same leftover to make 0x0fd7d000, so the two uses of `r17` contradict
(caught before board time). It was replaced with the per-output fault-readout
`or oN, r5, r17; ld.a.d r3, oN`, eleven images o0..o10 (all 22 hashes verified against the
build).

**Result (C):** every output o0 through o10 faulted b1 at **exactly 0x0fd7d000** = r5's leftover
0x0c004000 | r17's 0x0fd79000. Each `or oN` landed and the `ld` through `oN` read it, so
**o0..o10 are all task-writable** — covering the restructure's measured peak output pressure of
10 (`ukernel/pressure.py`).

**Caveat (G, flagged by the board session):** every probe wrote the **same** value, so this does
not rule out the outputs *aliasing* one physical register. The restructure needs 10 *distinct*
scratch registers, so a follow-up (`cores/sgx530_odist0.py`, `_odist9.py`) writes o0..o9 ten
**distinct** values (rN's own leftover, via `or oK, rN, rN` — no `r17`, whose bits would mask the
difference) and reads o0 (expect 0x0055d000) and o9 (expect 0x0d04f000); its own source's value =
distinct, any other page = a later write clobbered it = aliasing. Pending a board slot.

## After the forty-third attempt (path A, the .x2 DOUTA): the DOUTA form is not the cause (C)

Attempt 43 (log 8be4dc7d, build 661edcb7, with the `sgx530_pa0` reader) ran path A — a single
PDS program doing a DOUTA then a DOUTU — in three variants:

- Baseline: b1 at 0x0e860000 (PA0 still holds attempt 41's one-row DOUTA value).
- Plain one-row DOUTA (V 0x0e860000): b2 at 0x0f400000, code base 0.
- The `.x2.s27` DOUTA (word | 0x28000000, V 0x0e870000): b2 at 0x0f400000, **code base 0, the
  same.** The board session verified the OR is purely additive — it sets b29 (op nibble 2) and
  b27 (s), while the src rows, swizzle and dest all sit at b0-23 and cc at b24-26, so every
  source field is intact and the word went 0x07… → 0x2f… cleanly.

**Confirmed (C): the DOUTA's form (07 vs 2f) is not what breaks the following DOUTU.** My
`2f`/`.x2.s27` hypothesis is ruled out. The DOUTA still writes PA0; a DOUTU after it still
launches at code base 0.

**Next (both sessions agree): mirror `pds3`'s exact slots** — DOUTA value from an input not a DS
word, DOUTU word 1 = 2 from a DS word, word 0 from a DS1 word holding `doutu()`, and the many
instructions `pds3` puts between the DOUTA and the DOUTU (the probe puts them adjacent). Those
are the remaining differences from the one construction known to work on the 545. Order-swap
(DOUTU first, then DOUTA) is the diagnostic if that is flat too. Analysis:
[sgx530-attr-restructure.md](sgx530-attr-restructure.md) §6.

## After the forty-fourth attempt (c105cac9): outputs are distinct (C); a store gate appears

**Distinctness — the scratch gate is green (C).** The ten `odist` images each read their own
source's leftover: o0 0x0055d000, o1 0x0722a000, o2 0x003e1000, o3 0x08439000, o4 0x09e06000,
o5 0x0c004000, o6 0x0503f000, o7 0x08779000, o8 0x002ba000, o9 0x0d04f000 — all matching the
expected table. So **o0..o9 are ten distinct, task-writable registers**, covering the
restructure's peak output pressure of 10. (`odist10`, the margin-1 check, did not run in that
slot; folded into attempt 45.)

**st-from-output surfaced a store gate, not an answer.** With PA0 set to a mapped control-block
scratch address (ctlblk+0x60), `stout_o0`'s `st.a.d pa0, #0, o0` **faulted** — BIF 0x4004 at page
0x0f003000, the control block's own (mapped) page. The store uses PA0 as its base correctly, but
the write does not land, so whether `st` reads an output as data is still unseen. Also confirmed
in the same slot: attempt 43's `.x2` DOUTA **did** write PA0 (a `stout` variant using the
PA0 it left faulted at 0x0e870000, C) — the DOUTA writes regardless of form; only the following
DOUTU fails.

**Reading (G), grounded in the docs.** This looks like **EDM_PROTECT** enforcement. UAPI.md P2:
on the SGX545 a *client* task's `st` into EDM_PROTECT kernel memory wedged the GPU (HWR), while
the microkernel — running as the EDM data master — writes its control block normally. So
EDM_PROTECT distinguishes the EDM from other requestors. If the SGX530 numbers its data masters
differently from the 545's (where code base 15 = the microkernel/EDM), the task launched at code
base 15 is **not** recognised as the EDM, and its stores to EDM_PROTECT pages fault — which would
block **every** microkernel store to its control block (heartbeat, status, dispatch state). A
direct check: `BIF_FAULT` CID (b0-3) reads **0xf** for an EDM fault (UKERNEL.md, 2026-11-03); if
the stout store's CID is not 0xf, the store is not coming from the EDM, confirming this.

This is a **third gate** on the restructure, alongside distinctness (now green) and
DOUTA-then-DOUTU: the microkernel cannot run if its stores fault, whatever the register files do.
Attempt 45 (approved): the code-base-15 data-master field at 0/1/2, then the data arena mapped
without EDM_PROTECT on the 530. Analysis: [sgx530-attr-restructure.md](sgx530-attr-restructure.md) §5.

## After the forty-fifth attempt (5ee84d86): EDM_PROTECT is the store gate (C); margin one (C)

**The store gate is EDM_PROTECT, not the data master (C).** `stout_o0` (PA0 = ctlblk+0x60) was
run with the code-base-15 data-master field at 3/0/1/2: **every one faulted** BIF 0x4004 at the
control-block page, so the DM field does not change it. Then with the kernel data arena mapped
**CACHECONSISTENT only (no EDM_PROTECT)**: **no fault at all** (BIF int 0, the USE busy in its
loop). So EDM_PROTECT is what blocks the task's stores, whatever the data master. The fix is
driver-side and clean: the SGX530 maps the microkernel's writable structures without EDM_PROTECT.
(The 545's `BIF_FAULT` CID-in-b0-3 does not show on the 530 — its low 12 bits read 0 — so the
EDM-vs-not question can't be read off the fault there; the EDM_PROTECT result settles it anyway.)

**Margin one (C).** `odist10` faulted b1 at 0x0fd79000 = r17's leftover, so o10 is distinct too:
**eleven distinct task-writable outputs**, one more than the peak of 10.

**Open: the store does not reach host-visible memory yet.** With EDM_PROTECT off the store no
longer faults, but the version word (t[0x18], where PA0 points) still read **0**. Two candidates:
(1) `st` cannot read an **output** as its data source; or (2) `idf`/`wdf` fences but does not
flush the USE data cache to memory (the DDK uses the op0x100 data-cache flush). `stout_o0`
already carries `st; idf; wdf`, so a missing fence is not it. The control `sgx530_stout_r5`
stores a **temp** (r5's leftover 0x0c004000) through the same path: reads 0x0c004000 → `st`
reaches memory and (2) is wrong, so the failure is (1) st-from-output; reads 0 → (2), a
cache-flush problem for any source. This matters for the restructure: the microkernel must store
its output-scratch to the control block. Analysis:
[sgx530-attr-restructure.md](sgx530-attr-restructure.md) §5.

## After the forty-sixth attempt (8abc9c05): store visibility, not st-from-output (C); pds3 flat

With EDM_PROTECT off by default, three results:
- **`stout_r5` (store a temp) and `stout_o0` (store an output): both no fault, USE busy, t[0x18] = 0.**
  So the failure is **not st-reading-an-output** — a temp store doesn't reach visible memory
  either. The store completes without faulting but does not show in the control block (C).
- **pds3-mirror variant 1 flat:** DOUTU 0x07003885 (w1=2, w0 from ds1[3]) + 13 × `tst` spacers
  still launched at code base 0 (b2 0x0f400000). So w1=2, the DS1 w0, and spacing are **not** the
  DOUTA-then-DOUTU cause. The in0-source DOUTA (variant 2) and the order-swap remain.

**Store-visibility reading (G).** `st; idf; wdf` is exactly what the open microkernel's own
heartbeat/status stores use, and those land on the SGX545 — so the 530 difference is not a missing
flush instruction. `ld.a.d` computes addresses correctly on the 530 (loads fault at their expected
pages), and the stout store uses offset `#0`, so address-unit scaling is unlikely to be it. That
points to either the store not committing, or the host read not being coherent with the GPU write
(the arena is read through a write-combined CPU mapping). Per the coordinator, check the simpler
cases first, with **no new instructions**: scan the whole control block and its neighbouring page
for `stout_o0`'s distinctive 0x0fd7d000 — found off t[0x18] = a misplaced store (an addressing
difference); absent = not committed or a non-coherent read. Only then weigh a documented flush.
Analysis: [sgx530-attr-restructure.md](sgx530-attr-restructure.md) §5.

> **Board probing is paused here (after attempt 47).** For the one-page handoff — what works, the
> four gates, and what the AM335x vendor capture must show — see
> [sgx530-state.md](sgx530-state.md).

## After the forty-seventh attempt (50367971): the write context, not the register files (C)

Attempt 47 ran all three open gates in one slot. The results converge on one cause.

- **Gate 2 (DOUTA-then-DOUTU): flat (C).** Variant 2 (the in0-source DOUTA) still launched at
  code base 0 (b2 0x0f400000). And the baselines are telling: v0/v2/v4 (a bare task reading PA0)
  did **not** fault, so PA0 was still a mapped address — meaning the pds3-layout DOUTA (attempt 46
  and the in0 variant) **never wrote PA0 at all**, unlike the standalone `.x2` DOUTA of attempt 43
  (which did land, 0x0e870000). So in the pds3 multi-instruction layout even the DOUTA fails;
  only a **standalone** DOUTA writes an attribute. Path (A) is dead; path (B), a standalone DOUTA
  re-issued by the driver, is the only attribute-input mechanism.
- **Gate 3 (store visibility): the store does not commit to DRAM (C).** `stout_o0` (0x0fd7d000)
  and `stout_r5` (0x0c004000) appear **nowhere** in the data arena (0xe000) or the code arena
  (0xe000), read from DRAM after an `rmb`. Since `pa0` = ctlblk+0x60 is *inside* the data arena,
  this is not a misplaced store — the store simply does not reach memory.
- **Gate 4 (a retiring load/write destination): fails (C).** `aluwr_pa0w` did not fault, and with
  PA0 confirmed still mapped (v3/v4), that means `or pa0, r5, r17` did not land: **the task cannot
  write the pa bank.**

**The unified picture (G).** On the SGX530 event task as we launch it (event kick → code base 15
DOUTU), the **only** write that takes effect is an **ALU write to an output**. Temporaries don't
retire; the pa bank doesn't retire; `ld` cannot target an output (not encodable), so a load has no
retiring destination; memory stores don't commit; and the task is not the requestor `EDMPROTECT`
admits. That is one signature, not five bugs: the task lacks a valid **write / EDM-task context**,
and only the output/emit path — which does not go through that context — works. The event→DOUTU
launch we use is not the full EDM task setup the vendor driver uses.

**Consequence: the register-file restructure cannot make the microkernel run.** Scratch in outputs
is real, but the task cannot load memory into a usable register (gate 4) or store to the control
block (gate 3), so it cannot read the CCB/control block or persist state — the microkernel's basic
job. The restructure was necessary groundwork but is not sufficient. **The AM335x vendor capture of
TI's working SGX530 EDM task setup is now the critical path** (fact sheet gap, open question §7):
it should reveal the task-context setup that makes writes commit. Continued register-file probing
will not unlock it. Analysis: [sgx530-attr-restructure.md](sgx530-attr-restructure.md) §3, §5.

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

## After attempt 47: board probing resumes; fresh black-box hypotheses

There is no AM335x board, so the write-retire blocker is pursued on the OMAP3630 board
itself. A separate analysis-side session studies the vendor stack and will write its findings into
`docs/porting/cores/sgx530-vendor-setup.md`; this (implementation) side works from `docs/` only and
contributes the black-box checks below, run on the OMAP3630 board under its standing go (RAM boot
only, named registers only).

All hypotheses below are grounded in our own board tests, not in any vendor or GPL source. Attempt
47 ruled out: the DOUTU temp-count field position (35), `phas.i` temps4 (36), the host init-regs we
tried (37), instruction pairing to 8 nops (38), the limm value-field layout (30), `[moe]`/offset
scaling (27-29), the `EDMPROTECT` data-master field 0-3 for the store fault (45), the 2f `DOUTA`
form (43), the pds3 slot layout (46-47), and st-from-output vs temp-store (46). The hypotheses here
are not covered by any of those.

Ordered cheapest first. "Cheap" = kernel-side, no new image; "medium" = one new probe image.

- **H-A — the store target page's `EDMPROTECT` state (cheap; may need no run).** Attempt 45 showed a
  task store to an `EDMPROTECT` page (PTE/PDE b4) **faults**; gate 3 (46-47) showed a store to the
  data arena **commits to neither arena** and `pa0` is in the data arena so it is "not misplaced."
  Open question to the kernel session: was b4 actually **clear** on the exact page the 46/47 store
  targeted? If every page we stored to had b4 set, "stores never commit" is really "every page was
  EDM-protected." Cheap confirm: re-run `stout` to a page with b4 explicitly clear and writable to
  all masters. Distinguishes "no store path at all" from "always-protected."
- **H-B — the task store is held in a GPU cache, unflushed when the host scans (cheap).** Our
  `stout` probe does `st; idf; wdf; loop`, then the host scans DRAM after an `rmb`. The store may
  sit in a GPU-side cache that the task's own `idf/wdf` does not drain all the way to DRAM. Cheap
  confirm: after the store task, have the **host** do a BIF cache invalidate (and read the
  `EVENT_STATUS2` cache-invalidate-complete bit) **before** the DRAM scan. If the value then
  appears, stores do commit and the gate is a flush, not a missing write context.
- **H-C — the launch must open a write window (medium).** Gate 1/2: ALU writes to **outputs**
  retire, a **standalone** `DOUTA` writes a `pa` that persists, but temporaries and the `pa` bank
  written by the task itself do not retire (attempt 47). Hypothesis: the task launch has to declare
  a non-zero `pa`/secondary-attribute **allocation** (the write window the restructure doc's gate 2
  is about), separate from the temp **count** already ruled out in attempt 35; without it the
  temp/`pa` writes alias nowhere while the output/emit path still works. Test: a probe that declares
  a non-zero secondary-attribute allocation in the launch, then an ALU write to a temp and to `pa`,
  readout via the `aluwr` fault path.
- **H-D — the data-master type gates the whole write context, not just the store fault (medium).**
  Attempt 45 swept `USE_CODE_BASE`'s data-master field only for the **store fault**. Re-check
  **temp retirement** across that field: launch `aluwr`-to-temp under each DM value. If one makes
  temp writes retire, the task was running as the wrong data master, which would also explain why
  it is not the `EDMPROTECT` requestor.

Recommend **H-A and H-B first** (cheap, no image, one board slot), then **H-C**. Report each
result to the coordinator before any long run. The analysis-side capture remains the definitive
answer; these narrow it and may unblock sooner.

## The vendor-launch test (sgx530-vendor-setup.md §9), queued ahead of H-C

The analysis side's static read of the vendor stack
([sgx530-vendor-setup.md](sgx530-vendor-setup.md)) says gates 1/3/4 are one launch-context
difference, not three, and gives the vendor's event-task launch. This is built from that doc and
runs ahead of H-C. It is **driver-side**: the aluwr/stout probe images are the unchanged stage-1
ones (the `event` entry is at offset 0, so no in-segment offset to encode; `temps` = 18). Nothing
in `ukernel/` changes, so the SGX545 golden stays byte-identical. The driver (sgx530-kernel) makes
the launch changes:

1. **Event PDS data in the 8-word interleave** (§2): place the three DOUTU words at data-segment
   memory words **0, 1, 8** (DS0[0], DS0[1], DS1[0]).
2. **The DOUTU words** (§3): word 0 = **0x0020000F** (code base 15 in bits 0-3, offset 0, bit 21
   set, **bits 22-31 clear** — in particular do *not* encode the manifest temp count into bits
   24-31), word 1 = **0x04000000**, word 2 = **0x00000001**.
3. **Event control words:** `0xa6c` = **3**, `0xa70` = **0x00013d01**.
4. **Map the kernel data arena with `EDM_PROTECT`** again (revert the attempt-45 stopgap), to test
   the store path under the vendor's requestor assumption (§6).
5. Keep `phas.i` first (the probe cores already do).

**Readout** (as in stage 1): `aluwr` — does an ALU write to a **temporary** retire (fault at the
OR'd page 0x0fd7d000, not the leftover 0x0c004000; gate 1)? `stout_o0`/`stout_r5` — does a **store**
commit to DRAM without faulting on an `EDM_PROTECT` page (gates 3/4)?

**Bisect** if writes retire: word 2 first, then word 1, then word-0 bit 21, then `0xa70`. Then
re-run one DOUTA-then-DOUTU under the 8-word interleave to close gate 2. If the store still does not
commit with `EDM_PROTECT` on, vary word 1 bit 26 alone. Same board rules and lock turn-taking as
H-A/H-B; batch into one slot if those have not run yet.
