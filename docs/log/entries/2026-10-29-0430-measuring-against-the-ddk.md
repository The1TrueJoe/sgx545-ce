# Measuring against the DDK, properly

By 3 a.m. our driver looked to be at the DDK's frame rate. A closer look
showed that two of our measurements had been misleading, and that we're at
about 82% of the DDK.

## What was wrong with the bench

- **It ended too early.** In the bench world, chunks keep loading for about
  two minutes after the world opens, and the frame rate climbs as they
  finish. A 110 s run stopped while ours was still at 11-18 FPS. That looked
  like a regression, but it was just the clock. `mcbench.sh` now runs 170 s
  by default, and only the last windows count.
- **gl4es' FPS lines came late.** gl4es prints them on stdout, which is
  buffered when it goes to a file. The lines arrived in 4 KB batches, and the
  last ones were lost when the game was stopped. The DDK's flat "30.00" was
  the menu's frame cap, read minutes late. With `MC_LINEBUF=1`, both
  launchers preload coreutils' libstdbuf, so each line lands when it's
  printed. The DDK's launcher for this is a copy of the runtime's
  (`mk-ddkrun.sh`); the runtime's own is left alone.

## The numbers, same hour, same runs

End of a 170 s run in the bench world, two runs each, alternating:

| | FPS |
|---|---|
| the DDK | 39-43 (about 41) |
| ours | 32-35 (about 33.5) |

What the rest of the night bought:
- The compiler's F16 work: Minecraft's terrain fragment program is down to 19
  instructions, the same as libusc's main program plus its unpack.
- The driver's own constant formats (`660743c`, `a43ae0b`): lowp colours
  uploaded as F16 of their C10 value, and literals in SA.
- C10 constants rounded from a double product, as the DDK does it: 0.3 gives
  77, where a float product is exactly 76.5 and rounds to even.

A new A/B, `sgx545_drvconsts.sh`, found two F16 bugs, which the compiler then
fixed (`9e34c55`). fog and c10scale are byte-identical to the DDK.

## Where our frame goes

| | per frame |
|---|---|
| the game's render, TA + 3D (`SGX545_SYNC=1`) | ~27 ms |
| the swap's copy to fb0 | 3.0 ms |
| the render thread on the CPU | ~24 ms |

- **The GPU sets the pace.** With constant-colour fragment shaders
  (`SGX545_EXP_FS_CONST=1`), ours runs 40-43 FPS and the render thread is on
  the CPU 99% of the time. So the whole gap to the DDK is GPU time,
  specifically the fragment work.
- **Our frames run in series.** A frame's TA and 3D are kicked together at
  the end of the frame. The next frame into the same target waits until the
  previous one is done (its render details are rewritten for the next
  render), so per frame the GPU costs TA + 3D one after the other.
- **The design allows overlap.** The RT data has two entries, and its region
  headers alternate between frames. That is the DDK's double buffering, built
  so the next frame's TA can run while this frame's 3D does.
- **Overlap is not safe to try blind.** The entry that renders is picked by a
  frame counter the CPU writes before each kick. Get it wrong and the 3D locks
  up on the fourth frame, as it did on the EA3 on 2026-10-28.
- **Next: the DDK's own kicks.** A trace of the DDK's game (pvrtrace, no
  snapshots) will show whether it kicks the next frame before the last one is
  done, and how.

Per-render latency in `SGX545_STATS` is now split between renders into the
scanout buffer and the rest (`c2720cc`).
