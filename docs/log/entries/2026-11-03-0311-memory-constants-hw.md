# Uniforms past sa127 load from memory, right on the hardware

CE5310 board (CE5310), 2026-11-03 ~03:00 UTC, desktop-GL build `x86_64-gl` with compiler 8af333d +
d6ec64f and driver 3d78059 + 9bd56d3, piglit `shader_runner` on the surfaceless platform.

An instruction names sa0-sa127, so a program's constants used to stop at sa127. Constants
that don't fit now live in a block in memory. The block is allocated per draw from the
frame's general ring. Its address goes in one SA register per 128 dwords, and the program
reads with `ld.a.d rN, sa<base>, #i` then `wdf`.

## What passed

- **Static spills**: a vertex program summing 40 and 118 vec4 uniforms, and a fragment
  program summing 40 and 120. Every value was right, with no recoveries.
- **Element by element**: one uniform at a time through the 118 vec4 vertex program
  (elements 0, 27-33, 58-61, 64, 90, 100, 116, 117), then combinations. This checks the
  offset rule (block word slot + 1) and each of the four base registers.
- **Dynamic indexing**: `u[i]` loops over 118 vertex and 126 fragment vec4.

## What still recovers

piglit `glsl-uniform-interstage-limits` recovers because of its run time, not a wrong
program. GLSL lowers `u[i]` to a select ladder over the whole array, because the driver
doesn't claim `indirect_const_addr`. That costs 360 `ld.a.d` per loop iteration in the
1,321-instruction vertex program. In the fragment program, at 126 iterations per pixel:

- a 25x25 px rect sometimes takes one recovery;
- a 62x62 px rect takes two recoveries and draws nothing.

Each of these recoveries was a TA lockup with no BIF fault. One recovery's reset logged
`SGXInitialise failed (24)`, and the next reset brought the GPU back.

## Fixed: indexed loads

Compiler 36ad6fc reads an indexed array that doesn't fit SA from the block. It clamps the
index, adds it to the base register with `imae`, and loads each component with `ld.a.d` and
an immediate offset, so it needs no register-offset form. Driver 1836bfc turns on
`indirect_const_addr`, so `u[i]` reaches the compiler as an indexed load instead of a select.

On the CE5310 board (~03:45 UTC), with 0 recoveries throughout:

- the loops above pass, including 126 iterations over the full 250x250 target (under 1 s);
- the static spills and combinations still pass;
- `glsl-uniform-interstage-limits` passes in its default, `--subdivide 5` and
  `--subdivide 5 --statechanges` runs (25 cases each).
