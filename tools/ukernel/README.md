# tools/ukernel: the microkernel offline

| file | what |
|---|---|
| `emu.py` | USSE emulator for microkernel-style code: the integer, memory, register-bank and flow-control forms, decoded by `tools/isa/hw.py`. Stops on anything it has no rule for |
| `sgxsim.py` | the kernel-data layout the emulator runs use and a model of the registers the microkernel polls |
| `fromtrace.py` | device memory, HW render contexts and kick parameters from a pvrtrace log (DDK capture or `tools/gen` `frame-fake -t`) |
| `pds.py` | a PDS program interpreter: runs an event/entry PDS program (DOUTD DMA into the secondary attributes, DOUTA into a primary-attribute register, DOUTU launch) against device memory and starts the USSE task, so the whole event-program -> microkernel path runs offline |

`frame-fake` builds on macOS too (clang, `-D__linux__`, an empty
`linux/ioctl.h` stub that includes `<sys/ioctl.h>`), so a generated frame can
be run on any machine. What the runs showed, and the rules the emulator and
model follow, are in `docs/UKERNEL.md`.
