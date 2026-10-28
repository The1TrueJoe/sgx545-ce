# tools

Reverse-engineering and test tooling. None of it ships on a box; the driver lives in
`../kernel`, `../srvinit`, `../mesa` and `../libpvr`. Build scripts expect
[zig](https://ziglang.org) (`zig cc`) for cross-compiling to the EA's x86_64 and i386.

| Directory | What it does |
|---|---|
| `trace/` | `pvrtrace`, an `LD_PRELOAD` tracer for DDK clients: every bridge call, mapping and GPU memory snapshot |
| `decode/` | Capture readers: memory map, TA command, diffs, `mkreplay.py` (capture to replay script), `cmpfb.py`, `fig.py` (frames to PNG) |
| `replay/` | Native x86_64 replayer for captured frames |
| `gen/` | Frame generator: builds every buffer of a frame from GL-level parameters (`docs/GEN.md`) |
| `isa/` | USSE assembler/disassembler, encoding tables (`hw*.py`), libusc encoder oracle, hardware tests (`hwt*.py`), shader corpus |
| `pds/` | PDS disassembler and example listings |
| `state/` | Render-state decoder and the formulas that rebuild it (`build.py --check`) |
| `probe/` | A GLES2 program driven by environment variables, for one-change captures |
| `ddk/` | Helpers for the Intel DDK userspace, kept for A/B testing: `sgxinit`, `gles2tri`, `gpubench`, `busidshim`, `sgxregs.sh` |
