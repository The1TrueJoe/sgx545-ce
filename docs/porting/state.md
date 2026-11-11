# Decoding a core's command streams and GPU state

This guide shows how to work out what a new SGX core expects from its command streams
and state words, and how to check each finding on the hardware. It covers:
- the TA and 3D control streams;
- the state words;
- the parameter buffer;
- the DPM, ISP and TSP state.

Everything here is black-box. You observe a driver that already works on your board,
change one input at a time, replay the results, and measure the hardware. You don't
need any driver source.

The SGX545 results are already written up, so link to them rather than repeating them:
- [`docs/STATE.md`](../STATE.md): every word of the TA command, the VDM stream, the TA
  state block, the render target and the textures, with what is known about each field;
- [`docs/UKERNEL.md`](../UKERNEL.md): what the microkernel writes to the hardware for a
  TA kick and a 3D start;
- [`docs/TEX.md`](../TEX.md) and [`docs/PDS.md`](../PDS.md): textures and PDS programs;
- [`docs/log/BUILDLOG.md`](../log/BUILDLOG.md): the build log, with what was tried and
  what each hardware run showed.

Most SGX cores share the SGX545's layout. Start from STATE.md and look for the
differences, rather than decoding from scratch.

## The method

### 1. Capture a reference

Run a driver that already draws correctly on your board under `pvrtrace`, an
`LD_PRELOAD` interposer on the services bridge (`tools/trace/`). It records:
- every bridge call, with its timestamp and its input and output structures;
- every mapping of GPU memory;
- when asked, a snapshot of every mapped GPU buffer at a kick.

From that record, `tools/decode/memmap.py` rebuilds the GPU's view of memory, buffer by
buffer and device address by device address. The decoders then read the TA command each
kick points at and follow it into the VDM stream, the state blocks and the programs.

If no working driver exists for your board, start from STATE.md's values instead and
treat each of them as a guess until your hardware confirms it.

### 2. Make one change at a time

A capture of a whole application tells you what the words are, but not what any one of
them means. For that, take pairs of captures that differ in exactly one input:
- `tools/probe/probe.c` (GLES2) and `probe1.c` (GLES1) take every input from environment
  variables: the clear color, vertices, depth/stencil/blend state, textures, render
  target size and format, and more (see the header of each file);
- `tools/decode/diff.py A.bin B.bin KICK` prints every word that differs between the two
  captures at that kick, buffer by buffer.

Change one GL call, diff, and the words that moved are the ones that call controls. For
a numeric field, change the input in steps (1, 2, 3, 16, 17, 33 pixels, say) to find its
width, its units and any rounding.

### 3. Decode, and tag what you know

`tools/state/decode.py --kick N --usse CAPTURE.bin` prints every word of the kick:
- the TA command and its tail;
- the render details and the parameter-buffer descriptor;
- the VDM stream with each draw and state program;
- the TA state blocks;
- the pixel and vertex PDS programs, with the USSE programs they start disassembled.

Write down what you learn in the form STATE.md uses, with a tag on every field:
- **D**: derived, and the value follows from the inputs;
- **C**: confirmed by a hardware replay;
- **G**: a guess;
- **U**: unknown, so pack the value you saw.

A field stays U until a capture varies it. Don't name a bit because a neighboring core
had a bit of that name there.

### 4. Rebuild the words from your formulas

Once a group of words looks understood, generate them from GL-level parameters, using
nothing but your notes, and compare the result against a capture:
- `tools/state/build.py --check CAPTURE.bin` does this for the SGX545;
- `tools/gen/` builds a whole frame from parameters (see [`docs/GEN.md`](../GEN.md)).

Any word the formulas get wrong is a gap in the notes.

### 5. Poke one field and replay

A decoded meaning is a hypothesis until the hardware agrees with it.
- `tools/decode/mkreplay.py` turns a capture into a script.
- `tools/replay/replay` runs that script through the bridge with no vendor library
  loaded.
- `--poke VA:word,...` and `--poke-at EVENT:VA:words` change single words before a kick.
  `--write`, `--usse` and `--verts` replace whole buffers, programs or the geometry.
- `tools/decode/cmpfb.py` compares the rendered target with a reference readback.

Change one field, replay, and see whether the output changes the way your reading
predicts. A replay that hangs or recovers is evidence too: write it down with the value
that caused it.

### 6. A/B against your own driver

Capture your own driver the same way, on the same path:
- the same board, kernel and client binary;
- the same window system, scene and moment in the application;
- only the GL library swapped.

Then decode both captures and compare them by role, not by address. Compare:
- each draw's state block;
- each program's length and inputs;
- the outputs per vertex;
- how the frame is split into kicks and scenes.

Line up kick timelines with `tools/decode/kicktimes.py LOG` (see the pitfalls below)
before you compare times.

### 7. Measure busy and idle time per unit

Command streams show what the GPU was asked to do; they don't show how long each unit
took. For that, sample registers while the application runs:
- **`tools/ddk/regsample.c`** polls a fixed list of named registers through `/dev/mem`
  and records each change with a TSC timestamp. Among them are:
  - `CLKGATESTATUS` (0x008): which units are clocked;
  - `EVENT_STATUS` / `STATUS2`: TA finished, end of render, out of memory;
  - the DPM counters.
- **`tools/state/busysplit.py CSV`** turns a sample into:
  - the share of wall time with the TA and 3D clocked together, the TA alone, the 3D
    alone, and nothing at all;
  - each unit's total busy share;
  - the per-frame TA and 3D segment lengths;
  - `--idle` adds where the idle stretches sit between units.

Use the same sampler, window position and length for both drivers, so the comparison is
fair.

**Finding the clock groups on a new core:** the bits of `CLKGATESTATUS` fall into groups
that switch together. On the SGX545:
- **0x3 (3D):** goes off at the end of render;
- **0x58 (TA):**
- **0x33004 (pixel side):** toggles fast during a render;
- **0xCEF80 (shared):** on whenever either unit, or the microkernel, is running;
- **0x20 (DPM):** held on.

To label the groups on another core:
1. Sample a run.
2. Count each bit's rising edges and busy share; bits in one group have identical counts.
3. Match the groups' falling edges against the TA-finished and end-of-render events.

The group that's on whenever anything runs includes the microkernel's own work. So never
make the microkernel wait for it to clear: it can't see it go off while it is running.

### 8. Decode the words that stay unknown

For a word that no single change moves:
- **Collect every value it takes** across all your captures. A word that never varies
  is packed as a constant, and that's fine.
- **Correlate it with the scene,** not just the GL state: the render target size, the
  number of draws, the kick's position in a multi-kick scene, the frame number.
- **Change it in a replay one bit at a time,** and record what each bit does and which
  values recover the GPU.
- **Look at who reads it.** A word copied into a register at a kick (see the register
  tables in UKERNEL.md) is described by that register. A word the GPU writes back, such as
  the region headers or tail pointers, is described by where the GPU writes it and what
  reads it later.

## The tools, with commands

| Step | Command |
|---|---|
| Capture | `PVRTRACE_OUT=out.bin LD_PRELOAD=pvrtrace.so CLIENT`; `tools/trace/trace.sh out.bin PROGRAM` wraps it with a watchdog. `PVRTRACE_SNAPFILE=/tmp/snapnow PVRTRACE_SNAPN=5` snapshots only the 5 kicks after `/tmp/snapnow` appears, so a long run stays small |
| Build the tracer | `tools/trace/build.sh` (i386 and x86_64) |
| Kick timeline | `python3 tools/decode/kicktimes.py out.bin 1 100000` |
| Draw statistics | `python3 tools/state/vdmstats.py out.bin --draws` |
| Decode a kick | `python3 tools/state/decode.py --kick N --usse out.bin` |
| Diff two captures | `python3 tools/decode/diff.py a.bin b.bin N` |
| Rebuild and check | `python3 tools/state/build.py --check out.bin` |
| Replay | `python3 tools/decode/mkreplay.py out.bin dir/` then `replay dir/script dir/data` on the board |
| Compare a render | `python3 tools/decode/cmpfb.py dump.bin ref.rgba W H --png out.png` |
| Register sample | `regsample <bar0> <ms> out.csv [stopfile]` on the board (BAR0 from `/sys/bus/pci/devices/<dev>/resource`) |
| Busy split | `python3 tools/state/busysplit.py out.csv --idle` |
| Is the GPU there? | `tools/ddk/sgxregs.sh` on the board: identity registers only |

## Pitfalls

Every one of these produced a wrong conclusion on the SGX545 before it was caught. The
build log has the details.

### Warm-up is not steady state

A sample taken 35 s after a game world loaded showed 27% of wall time idle. The same
setup sampled 90 s in, at steady state, showed 5%.

**Before you trust a window,** check two things:
- the busy ms per second (`busysplit.py`) should be flat across the whole window;
- the number of frames rendered inside the window should match the frame rate the
  application reports when it's standing still.

Sample both drivers at the same point in the run.

### "TA alone" is not "TA time"

When the TA and the 3D overlap, the busy split divides the TA's time into a part alone
and a part with the 3D. The reference driver's TA-alone slice (~35 ms per frame) was
once quoted as its TA time, and compared with our serial ~40 ms. Its TA was in fact
clocked ~77 ms per frame (41 ms alone and 35 ms under its 3D). That sent a whole
investigation after a TA deficit that didn't exist.

**Compare each unit's total busy time, and say whether the other unit overlapped it.**
When two units share the USSE, a unit's time while overlapped isn't comparable with the
same unit running alone.

### Count kicks and scenes, not "frames"

One application frame can be several kicks. In a scene:
- the first kick has `bFirstKickOrResume`;
- the last has `bLastInScene`, and only that one renders;
- the kicks in between bin only.

`kicktimes.py` prints these flags as `F`, `L` and `-`. Two things to watch for:
- **A reference driver may keep a scene open across many application frames.** One
  rendered a pbuffer only every 44-566 `glFinish` calls, so its "frame time" excluded
  most of the 3D work. Force a render in every frame on both drivers (a 1-pixel
  `glReadPixels`, or a swap) before comparing.
- **Two drivers can split the same frame differently.** Compare per scene and per draw,
  not per kick.

### Check how each side waits

A test harness that polls for completion in growing sleep steps rounds every frame up to
its next check. A 3 ms render polled at 2.55 ms and then 3.55 ms costs about 4.4 ms, and
looked like a 30% GPU deficit. A wait that blocks on the kernel's event doesn't do that.

Before you compare times, look at the calls between kicks in `kicktimes.py`'s output
(event waits, sync-token flushes, transfers).

### Snapshots change what you measure

A snapshot copies every mapped buffer, which takes 0.5-3 s per kick. While snapshots run:
- kick timings and frame rates are meaningless;
- the snapped window may land in a different phase of the test than you meant.

Some more practical points:
- **Take timings** from a run with snapshots off (`PVRTRACE_NOSNAP`), or from kicks
  outside the snapshot window.
- **Trigger snapshots** with `PVRTRACE_SNAPFILE` and a delay after a log line. If the
  client buffers its stdout, the line can appear long after it was printed, so check
  which phase the snapped kick is actually in.
- **Watch the disk:** a whole-memory snapshot can be about 80 MB per kick. Twelve of them,
  about 1 GB, filled the board's root filesystem and truncated the files the run itself
  wrote.

### Compare by role, not by address

The same buffer lands at different device addresses in different drivers and runs.
Match buffers by what points at them:
- the TA command's render details and PB descriptor;
- the PDS program a state word names.

A word that differs only because it holds an address isn't a finding.

### What not to read

- **Never sweep a register BAR.** A read-only walk of a display controller's register
  block hung the whole board. Read only registers you have named; `regsample.c` and
  `sgxregs.sh` are written to that rule.
- **Event-status bits are acknowledged within microseconds.** A 20 µs sampler won't
  see the TA and 3D trigger bits; use the clock groups for busy time instead.
- **An emulator or simulator answers every poll.** It can't show hardware timing,
  directory-cache effects or a dropped kick. When the simulated register state matches
  and the hardware still differs, capture the live values at that moment on the board.

## Recording what you find

- Every hardware run that settles a field goes into the build log
  ([`docs/log/`](../log/)): what you ran, what you expected, what happened.
- Write the field itself into the core's state notes (STATE.md for the SGX545), with its
  tag.
- Keep captures out of the repository. Commit the decoded values and the commands that
  reproduce them, not the binary traces.
