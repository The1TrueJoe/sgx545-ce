# The compiler encodes through the isaspec encoder; the old encoder is gone

`mesa/src/sgx545/compiler` now emits every instruction through the encoder
isaspec generates from `mesa/src/sgx545/isa/sgx545.xml`. That XML was written
from `docs/ISA.md` alone. `usse_forms.h`, `usse.c` and `usse.h` are deleted,
and so are the tools that generated and checked them (`tools/usc/gen_forms.py`,
`check_enc.py`, `tests/enc_lines.c`).

## What changed

- `sgx_forms.h` / `sgx_forms.c` (new): the forms the compiler emits.
  - A form is an opcode plus the fields it fixes, such as `pck.u8.f32` or `movc.i32`.
  - Each form lists its operand slots: the slot's role, the register files it
    encodes, its modifiers, and whether it follows format select. Instruction
    selection checks operands against them, as it did against the old tables.
- `sgx_encode_ins()` fills a `struct sgx_isa_instr` and calls `sgx545_isa_encode()`.
- `sgx_program.c` encodes these three directly:
  - branches, once their targets are known (before, it patched l0-17 of a placeholder);
  - the dependent-sample `smp` template;
  - the discard `nop`.
- Two defaults surfaced while switching over: the write mask is 1, or 15 in
  `sopwm` (ISA.md: `mask=` "default 1", sopwm "default 15").

## Results

| check | result |
|---|---|
| Corpus: `tools/isa/corpus/c1-c3.glsl` and `tools/usc/tests/*/*.glsl`, via `run_corpus.py --layout`, plain and `--f16` | 2600 USP/code files byte-identical to um-re 74ad392; logs identical (1308/1338 compile, the same 30 fail before and after) |
| The same corpus with `sgxc -F -L -D` (the gallium driver's front-face, uniform and constant modes), um-re's sgxc against this one | 2600 files byte-identical, logs identical |
| `dep`/`sys` with `-F`, which emits `xor`/`and` (gl_FrontFacing) | Byte-identical (12 bitwise words) |
| Each instruction, with the old encoder still linked | ~24,000 encoded with both encoders: 0 differences (after the mask defaults) |
| Slot capabilities: role, files, neg/abs, format select, for every (form, operand) the corpus reaches, plus smp's LOD slot | Old and new agree |
| Coverage | Every form the compiler emits, including 180 `br`, 32 discard `nop`s, 2148 `ld`, 1213 `wdf`, and dependent `smp` templates in 208 USPs. `fadm`, `fmsa`, `fcos` and `pck.f32.f16` are in the form table but nothing emits them |
| `sgx545_isa_test` | 246/246 |

## Next

- usse-compiler: `blind.py` and its own tests on this commit.
- The gallium driver links `idep_sgx545_compiler`, which now brings
  `libsgx545_isa` with it; the target build (zig) still has to be run.
