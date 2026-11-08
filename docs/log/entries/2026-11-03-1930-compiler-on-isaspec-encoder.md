# The compiler encodes through the isaspec encoder; the old encoder is gone

`mesa/src/sgx545/compiler` now emits every instruction through the encoder
isaspec generates from `mesa/src/sgx545/isa/sgx545.xml`. That XML was written
from `docs/ISA.md` alone. `usse_forms.h`, `usse.c` and `usse.h` are deleted,
with `tools/usc/gen_forms.py`, which generated them. `check_enc.py` now
checks the isaspec encoder (via `sgx545_isa_test -t`) against `hw.py` on the
corpus words; `tests/enc_lines.c` is retired.

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
| The corpus as usse-compiler defines it: `tools/isa/corpus/c1-c3.glsl` and every `tools/usc/tests/*/*.glsl`, F32 and `--f16`, `--layout`; every file `run_corpus.py` writes (`.usp`, `.code`, `work/`) | 7886 files, `diff -rq` clean against bb8c44e's sgxc (all objects rebuilt for each side) |
| The same corpus with `sgxc -F -L -D` (the gallium driver's front-face, uniform and constant modes) | 2600 USP/code files byte-identical, logs identical |
| `dep`/`sys` with `-F`, which emits `xor`/`and` (gl_FrontFacing) | Byte-identical (12 bitwise words) |
| Each instruction, with the old encoder still linked | ~24,000 encoded with both encoders: 0 differences (after the mask defaults) |
| Slot capabilities (role, files, neg/abs, format select) for every (form, operand) the corpus reaches, plus smp's LOD slot | Old and new agree |
| Coverage | Every form the compiler emits, including 180 `br`, 32 discard `nop`s, 2148 `ld`, 1213 `wdf`, and dependent `smp` templates in 208 USPs. `fadm`, `fmsa`, `fcos` and `pck.f32.f16` are in the form table but nothing emits them |
| `meson test sgx545_isa` (`-Dbuild-tests=true`) | OK (246/246) |
| `mesa/build.sh x86_64` (zig, the gallium driver) | Builds and links `libgallium-26.2.5.so`, no sgx545 warnings |

## Validation (usse-compiler, black-box)

- Its own corpus build (host Mesa 26.2.5 + patch 0004): 7886 files byte-identical.
- `sgx545_isa_test gles2.txt int.txt -f 200000`: 0 failures.
- `blind.py` on the `-t` tables: 246 exact.
- `check_out.py --no-oracle`: all 13690 emitted instructions decode with no
  unknown bit and round-trip.
- `check_enc.py`, ported to the isaspec encoder: 4154 corpus words, 3981
  identical, 0 differ, 173 the assembler doesn't take (163 efo `i=`, 10 fdual
  with `sd=`/`sb=`/`sc=`; the compiler emits neither).
- A fresh blind draw over 174 forms: 166 exact. The 8 misses are isa-package
  gaps, untouched by this change; nothing the compiler emits is affected:
  - the assembler doesn't take efo `i=`, dot `cs=`/`as=`, `firh.s8`, st `am3` with
    `.q`, or rlp `unused2=`;
  - an fcos with both f16 and c10 gets h7 wrong: `struct sgx_isa_src.fmt` holds one
    format, not both flags.

## Next

- The isa-package gaps above.
