# Adding an SGX core

The stack is built to support the whole PowerVR SGX family with one kernel driver, one
Mesa driver and one microkernel source. A new core is added as data in each of them,
not by copying code.

## The cores

| Core | ISA | Example hardware | Status |
|---|---|---|---|
| SGX545 | USSE | Intel Cedarview (Atom N2600, N2800, D2500, D2550, D2700); Intel Atom CE5300/CE5310 | Supported |
| SGX535 | USSE | Intel Poulsbo (GMA 500), Apple A4 | Wanted |
| SGX530 | USSE | TI OMAP3 | Wanted |
| SGX540 | USSE | TI OMAP4 | Wanted |
| SGX543, SGX544, SGX554 | USSE2 | TI OMAP5 and AM57xx, Allwinner A31 and A83T, some MediaTek SoCs | Wanted; needs USSE2 support in the compiler and assembler |

The cores marked USSE share an instruction set with the SGX545, so most of the work is
the per-core facts and the platform glue. USSE2 is a different instruction set and a
larger project.

## What adding a core involves

1. **Kernel** ([kernel.md](kernel.md)): an entry in `powervr_sgx`'s core table, and the
   platform glue that publishes the GPU (a `gma500` chip entry on Intel, a device-tree node
   elsewhere).
2. **Microkernel** ([microkernel.md](microkernel.md)): a configuration for the core, and a
   firmware build from it.
3. **Mesa** ([mesa.md](mesa.md)): an entry in the driver's per-core table.
4. **State** ([state.md](state.md)): how to decode the core's command streams and GPU
   state against a working driver, and how to check each finding on the hardware.
5. **Evidence**: each guide lists the minimum hardware results before an entry is accepted.
   Attach them to the pull request.

Open a "New SGX core" issue first, with your hardware and what you can already run on it.
You can work on one part at a time; a core can be accepted in the kernel before it draws
anything, as long as what it claims is tested.

## Owners

Each core has an owner who tests it on real hardware and reviews changes to it.

| Core | Owner |
|---|---|
| SGX545 | Joseph Telaak |
