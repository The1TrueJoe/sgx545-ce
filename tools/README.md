# tools

Bring-up and test tooling. None of it ships; the driver lives in `../drm`, `../mesa` and
`../ukernel`. [docs/porting/](../docs/porting/README.md) says which tool belongs to which
bring-up stage. Build scripts expect
[zig](https://ziglang.org) (`zig cc`) for cross-compiling to the EA's x86_64 and i386.

| Directory | What it does |
|---|---|
| `trace/` | `pvrtrace`, an `LD_PRELOAD` tracer for DDK clients: every bridge call, mapping and GPU memory snapshot |
| `decode/` | Capture readers: memory map, TA command, diffs, `mkreplay.py` (capture to replay script), `cmpfb.py`, `fig.py` (frames to PNG) |
| `replay/` | Native x86_64 replayer for captured frames |
| `gen/` | Frame generator: builds every buffer of a frame from GL-level parameters (`docs/GEN.md`) |
| `isa/` | `asm.py` (USSE text to words and back through `sgx-asm` / `sgx-disasm`, built from `sgx.xml`), hardware tests (`hwt*.py`), shader corpus |
| `pds/` | PDS disassembler and example listings |
| `state/` | Render-state decoder (`decode.py`, `--usse` disassembles the programs), per-draw statistics (`vdmstats.py`), the per-unit busy/idle split of a register sample (`busysplit.py`), the formulas that rebuild it (`build.py --check`) |
| `probe/` | GLES2 (`probe.c`) and GLES1 (`probe1.c`) programs driven by environment variables, for one-change captures |
| `ddk/` | Helpers for the Intel DDK userspace, kept for A/B testing: `sgxinit`, `gles2tri`, `gpubench`, `busidshim`, `sgxregs.sh` |
