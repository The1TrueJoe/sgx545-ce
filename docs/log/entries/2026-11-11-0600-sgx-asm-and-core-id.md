# sgx-asm and sgx-disasm, and a core id in sgx.xml

Two pieces of UKERNEL-TOOLING.md for the Mesa side of the microkernel's
assembler.

**The tools.** `sgx-asm` and `sgx-disasm` (`mesa/src/sgx/isa/assembler.c`,
`disasm.c`) are built with `-Dtools=sgx`, as `etnaviv-disasm` is with
`-Dtools=etnaviv`. The option is `mesa/patches/0006`. Both use the library
the compiler uses: the parser of ISA.md's text notation, then the encoder
and decoder generated from `sgx.xml`. There is no second encoder.

- `sgx-asm` assembles a whole file in one run, one instruction per line. It
  writes hex words (the default), binary (`-b`) or a listing (`-L`), and
  follows `setfc` from the format control given with `-F`.
- Errors come out as `<file>:<line>: <text>: <message>`, with exit status 1.
- `sgx-disasm` reads binary, or with `-x` hex lines (including `sgx-asm`'s
  output).
- `mesa/tests/isa-standalone/standalone.sh` builds both without Mesa, for a
  build that pins `sgx.xml`.

**The core id.** isaspec has one parameter the XML can see,
`ISA_GPU_ID()`, and it carried format control. It is now
`core * 100 + format control` (`sgx_isa_gpu_id()`), so a `<gen>` range
selects a core the way freedreno's `gpu_id` selects a chip.

- The root bitset is limited to SGX545 (`<gen min="54500" max="54599"/>`).
- `struct sgx_isa_fc` carries the core; 0 means SGX545, so existing callers
  are unchanged.
- A core `sgx.xml` doesn't describe is refused by `sgx_isa_core_supported()`
  and the tools (`-c 535`; `-c` takes `545` or `sgx545`), and its words
  don't decode.
- Nothing gates on the core yet: a second core's fields wait for its ISA.md
  section.

**Also.** `src/sgx/meson.build` enters `compiler/` only for the gallium
driver: without a driver that uses NIR, Mesa builds a stub libnir, which the
compiler test can't link against.

## Results

| check | result |
|---|---|
| `meson test --suite sgx`, `-Dtools=sgx` only (native host) | 4/4: `sgx_isa` (319 instructions: the lists plus the core checks), `sgx_pds` (57), `sgx_csb` (66), `sgx_isa_tools` (287 lines through both tools as hex and binary, plus the error paths) |
| `isa-standalone`, fuzz of 200,000 words | 102,393 words decode and reassemble to themselves |
| Encoded words | unchanged: every word a list quotes still matches, and the core checks compare core 0 with core 545 |
