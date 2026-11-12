# Bringing up a PowerVR SGX GPU

This project took the SGX545 from no documentation to a desktop, games and a fully open
stack: kernel driver, Mesa driver and compiler, and microkernel. Everything we learned on
the way is written down so that the next SGX core takes a fraction of the time. This page
is the map. The guides it links go stage by stage, with the tools, what "done" looks like,
and the traps we fell into.

The stack supports the whole SGX family with one kernel driver, one Mesa driver and one
microkernel source. A new core is added as data in each of them, not by copying code.

## The cores

| Core | ISA | Example hardware | Status |
|---|---|---|---|
| SGX545 | USSE | Intel Cedarview (Atom N2600, N2800, D2500, D2550, D2700); Intel Atom CE5300/CE5310 | Supported |
| SGX535 | USSE | Intel Poulsbo (GMA 500), Apple A4 | Wanted; fact sheet: [cores/sgx535.md](cores/sgx535.md) |
| SGX530 | USSE | TI OMAP3 | Wanted; fact sheet: [cores/sgx530.md](cores/sgx530.md) |
| SGX540 | USSE | TI OMAP4 | Wanted |
| SGX543, SGX544, SGX554 | USSE2 | TI OMAP5 and AM57xx, Allwinner A31 and A83T, some MediaTek SoCs | Wanted; needs USSE2 support in the compiler and assembler |

The cores marked USSE share an instruction set with the SGX545, so most of the work is
the per-core facts and the platform glue. Each core's facts, sourced and tagged, go in a fact sheet under
[cores/](cores/) before they go into the driver tables. USSE2 is a different instruction set and a
larger project.

## The guides

| Guide | Covers |
|---|---|
| [kernel.md](kernel.md) | Publishing the GPU, power, the MMU, loading the microkernel, kicks, interrupts, recovery, runtime PM |
| [microkernel.md](microkernel.md) | Bringing the open microkernel up on a new core, from first boot to full frames |
| [isa.md](isa.md) | Establishing a core's instruction set and getting the compiler to generate code for it |
| [state.md](state.md) | Decoding command streams and GPU state, and checking them against the hardware |
| [mesa.md](mesa.md) | The Mesa driver, from the first clear to a desktop, dEQP and performance |

## The order that works

Each stage builds on the one before, and each has a test that shows it's done. Don't skip
ahead: most of our worst days came from debugging a later stage on top of an unproven
earlier one.

1. **Know your board.** A way to recover it (serial console, network boot, a power switch
   you can control), and a way to run a kernel without flashing it.
2. **Identify the GPU.** Find its registers, power it up, and read the core ID and
   revision. ([kernel.md](kernel.md))
3. **Learn what's safe to touch.** Read only registers you have named; a blind sweep of a
   GPU or display register range can hang the whole machine.
4. **Boot the microkernel.** Get the GPU's embedded processor running our code and
   answering. ([microkernel.md](microkernel.md))
5. **First kick, first tile pass, first frame.** Send work, watch the tiler (TA) build the
   scene, then the 3D pass render it, and compare the pixels. ([microkernel.md](microkernel.md),
   [state.md](state.md))
6. **The kernel driver.** Buffers, GPU address spaces, the job queue, interrupts, and the
   tests that exercise them. ([kernel.md](kernel.md))
7. **Mesa: the first clear and triangle**, then textures, the compiler, blending and
   depth. ([mesa.md](mesa.md), [isa.md](isa.md))
8. **Conformance.** dEQP-GLES2 and piglit, with a baseline to compare every change against.
9. **The display.** Pair the GPU with the display device, then X with glamor, then a
   desktop with its compositor.
10. **Robustness.** GPU hang recovery, the parameter buffer running out of memory,
    several programs at once, power management, and memory released when programs exit.
11. **Performance.** Measure against a reference fairly, find the real bottleneck, then
    optimize. ([mesa.md](mesa.md))

## Words you'll meet

| Term | Meaning |
|---|---|
| USSE / USSE2 | The SGX's programmable shader cores and their instruction set; USSE2 is the 543/544/554 generation |
| PDS | A small programmable unit that sets up and starts USSE tasks |
| Microkernel | The program the GPU's embedded processor runs to schedule work; ours is open |
| Kick | The host telling the microkernel there is new work |
| CCB | The circular command buffer the host and microkernel share |
| TA | The tile accelerator: the tiler pass that bins a scene's geometry into tiles |
| 3D | The pass that renders the binned tiles to pixels (ISP for depth and visibility, TSP for texturing and shading) |
| Parameter buffer (PB) | Memory the TA writes the binned scene into |
| DPM | The unit that manages parameter-buffer pages |
| BIF | The GPU's memory interface and MMU |
| VDM | The vertex data master, which reads the TA's control stream |
| PBE | The pixel back end, which writes finished tiles to memory |
| ZLS | Depth and stencil load and store |

## The rest of the documentation

- **The hardware as we know it:** [ISA.md](../ISA.md) (instruction set),
  [STATE.md](../STATE.md) (render state), [PDS.md](../PDS.md), [TEX.md](../TEX.md)
  (textures), [UKERNEL.md](../UKERNEL.md) (the microkernel), [UAPI.md](../UAPI.md) (the
  kernel interface), [WINSYS.md](../WINSYS.md), [PERF.md](../PERF.md).
- **How we found it:** the [build log](../log/BUILDLOG.md), entry by entry, with frames
  and data. When a guide says "we hit this", the log has the full story.
- **How we work:** [CLEANROOM.md](../CLEANROOM.md) (evidence and roles) and
  [CONTRIBUTING.md](../../CONTRIBUTING.md).

## The tools

[tools/](../../tools/README.md) holds the bring-up tooling: command-stream tracing and
decoding, frame and state generators, the instruction-set assembler and hardware
instruction tests, the PDS tools, texture-layout tools, the offline microkernel emulator,
and the scripts we used on our test boards. Each guide says which tool belongs to which
stage.

## Adding a core

1. Open a "New SGX core" issue with your hardware and what you can already run on it.
2. Work through the guides for the parts you're doing. You can do one part at a time: a
   core can be accepted in the kernel before it draws anything, as long as what it claims
   is tested.
3. Attach the hardware evidence each guide asks for to your pull request.

## Owners

Each core has an owner who tests it on real hardware and reviews changes to it.

| Core | Owner |
|---|---|
| SGX545 | maintainer |
