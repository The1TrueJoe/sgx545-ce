# tools/usc: the SGX545 shader compiler's tooling

The compiler itself (the part a driver links) is `mesa/src/sgx545/compiler/`. This
directory holds `sgxc`, the checks and the EA3 test harness.

NIR in, USSE machine code out, packed in the DDK 1.7 USP container (docs/ISA.md, "The USP
container"). The GLES driver's finalizer, or a gallium driver's, links it into a runnable
program. It is written in C against Mesa's NIR and is meant to become the compiler of a
Mesa gallium driver.

| file | what |
|---|---|
| `gen_forms.py` | writes `mesa/src/sgx545/compiler/usse_forms.h`, the USSE encoding tables, from `tools/isa/hw.py` (all 172 forms) |
| `mesa/src/sgx545/compiler/usse.[ch]` | the encoder: `hw.encode()` ported to C over those tables |
| `…/sgx_compiler.h` | the API: `sgx_nir_options()`, `sgx_assign_varyings()`, `sgx_compile()` |
| `…/sgx_nir.c` | NIR lowering: I/O and varying sets, uniforms (constant numbers, F16/C10 formats), sin/cos range reduction, samplers |
| `…/sgx_compile.c` | instruction selection (vector ops as one repeated instruction where the operands allow), F16, textures, discard, control flow, liveness and register allocation (Mesa's `util/ra`), encoding |
| `…/sgx_usp.c` | the USP writer: header, list A/C, code, label, branch and sample blocks |
| `sgxc.cpp`, `meson.build` | standalone compiler: GLSL ES 1.00 via Mesa's GLSL frontend, each stage linked on its own |
| `check_enc.py` | the C encoder against `hw.py` on every corpus word |
| `check_out.py` | every emitted word: decodes, round-trips, and matches libusc's encoder (`oracle.py`) |
| `run_corpus.py`, `layout.py` | compile a `tools/isa/corpus` file |
| `count.py` | instruction counts against libusc |
| `hw/glt.c`, `hw/uspsub.c`, `hw/hwcmp.py`, `hw/calib.py` | EA3: draw with the DDK's driver running our USPs, against the DDK's own |
| `tests/gl4es/` | gl4es's Minecraft 1.7.10 shaders (`fpegen.c` runs gl4es's generator) and `mc_run.py` |

## Building

In a Mesa checkout (tested at 27.0.0-devel 177e630), make `src/sgx545/` hold a link
`compiler -> <sgx545-ce>/mesa/src/sgx545/compiler`, a link `sgxc -> <sgx545-ce>/tools/usc`,
and a `meson.build` with `subdir('compiler')` and `subdir('sgxc')`. Then add
`if with_gallium` / `subdir('sgx545')` / `endif` after `subdir('compiler')` in `src/meson.build`:

```
meson setup build -Dgallium-drivers=softpipe -Dvulkan-drivers= -Dplatforms= -Dglx=disabled \
    -Degl=disabled -Dgbm=disabled -Dllvm=disabled -Dopengl=true -Dgles2=enabled
ninja -C build src/sgx545/sgxc/sgxc
```

softpipe is only there because Mesa builds its GLSL compiler only with some gallium driver
enabled. Mesa's build wants Python ≥ 3.10 with mako and a bison newer than macOS's 2.3.

```
sgxc [-v] [-p] [-u name=k] [-a name=slot] [-s name=unit] -o prefix a.vert b.frag
```

`-p` compiles mediump in F16, the way the DDK does. `-u`, `-a` and `-s` pin the constant
numbers, attribute slots and sampler numbers. The DDK's GLSL compiler chooses those itself
and keeps them outside the USP, so a USP made to run under the DDK's driver must reproduce
them. `hw/calib.py` measures them on the EA3, and `layout.py` guesses them. A name prefixed
`vert:` or `frag:` applies to one stage only.

## Checking

```
python3 -I tools/usc/check_enc.py                     # C encoder == hw.py on 4154 corpus words
LIBUSC=.../libusc.so.1.7.862890 venv/bin/python -I tools/usc/check_out.py OUTDIR
python3 -I tools/usc/hw/hwcmp.py OUTDIR               # EA3: DDK's USP vs ours, same GL program
```

`hwcmp.py` runs each test program twice under the DDK's GLES driver. The first run uses
libusc's USP. The second uses ours: `uspsub.so` writes it into libusc's own result buffer.
The script then compares the two frames pixel by pixel.
