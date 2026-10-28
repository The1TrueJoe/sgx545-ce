# tools/ukernel: the microkernel offline

| file | what |
|---|---|
| `emu.py` | USSE emulator for microkernel-style code: the integer, memory, register-bank and flow-control forms, decoded by `tools/isa/hw.py`. Stops on anything it has no rule for |
| `sgxsim.py` | the machine around it: kernel data laid out as srvinit does, the five microkernel PDS programs as behaviour, a model of the registers the microkernel polls, and the host side (init, CCB kicks, events) |
| `fromtrace.py` | device memory, HW render contexts and kick parameters from a pvrtrace log (DDK capture or `tools/gen` `frame-fake -t`) |
| `run.py` | a whole frame through a microkernel: init, each TA kick, TA finished, 3D end of render, DPM 3D free; prints every register access, store and task |

    srvinit/fetch-firmware.sh <ddk> /tmp/uk.bin          # the DDK's, for comparison (never committed)
    python3 tools/ukernel/run.py /tmp/uk.bin capture.bin -o ddk.txt

`frame-fake` builds on macOS too (clang, `-D__linux__`, an empty
`linux/ioctl.h` stub that includes `<sys/ioctl.h>`), so a generated frame can
be run on any machine. What the runs showed, and the rules the emulator and
model follow, are in `docs/UKERNEL.md`.
