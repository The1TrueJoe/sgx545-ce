# Upstream readiness

What has to be true before the open stack's pieces go to their upstreams
(Mesa, the kernel), tree by tree: what is done, and what is still open. No
merge requests yet.

## Clean-room trees: `mesa/src/sgx545/isa`, `pds`, `csbgen`

The USSE instruction set and the PDS programs (isaspec), and the TA/3D state
words (csbgen).

### Done

| item | state |
|---|---|
| Layout like Mesa's other isaspec users | `isa/` and `pds/` follow `src/etnaviv/isa`: the XML, the `isa.h` the generated decoder includes, custom targets for the decoder (`<name>-isa.c/.h`), the encoder (`-encode.h`) and the enums, a static library and an `idep_`, and `tests/meson.build` |
| Layout like Mesa's csbgen users | `csbgen/` follows `src/imagination/csbgen/rogue`: `<name>.xml` -> `csbgen/<name>.h` through the shared `gen_pack_header.py`, and the per-file loop is the same. `sgx545_csb.h` plays the part of the Vulkan driver's `pvr_csb.h` (address type, includes, the pack-in-place macro). The XML files are renamed `vdm.xml`, `ta.xml`, `pbe.xml`, `tex.xml` |
| Tests under `meson test` | `meson test --suite sgx545` runs `sgx545_isa` (assemble -> encode -> disassemble over three lists; a line's given words must match), `sgx545_pds` (the same for PDS, plus the DOUT data words), and the new `sgx545_csb` (66 state words the documentation quotes, packed and unpacked). It passes 3/3 on a native host build with `-Dbuild-tests=true`. The csb test found an XML default that was wrong for one T3 form (tex2 clears b23); the test case now sets it. |
| Nothing outside `meson test` in the Mesa tree | `isa/tests/standalone.sh` (a library build without Mesa, which `tools/usc/check_enc.py` and `drm/tools/gen_usse_corpus.py` use) moved to `mesa/tests/isa-standalone/`, which isn't copied into Mesa. Its `.gitignore` went with it |
| Mesa style | one `mesa/src/sgx545/.clang-format` for all of `src/sgx545` (shared with the compiler): `InheritParentConfig`, `DisableFormat: false`, column limit 80, as `src/asahi` and `src/freedreno`. Every C file here is formatted with it. One macro list, the assembler's flag table, is hand-wrapped between `clang-format off/on`. Comments are in American English (`colour` -> `color`). A `src/sgx545/**/*` line in Mesa's `.clang-format-include` belongs to the MR |
| Licence headers | every source file, XML, meson file, script and test list has `Copyright © 2026 Joseph Telaak` and `SPDX-License-Identifier: MIT` (the test lists as `#` comments). The READMEs carry none, as Mesa's don't |
| A README per tree | `isa/README.md` and `pds/README.md` now describe the tree rather than its history: what it is, the files, the generated files, the tests, the isaspec patch, what isaspec can't express, and who uses it. The answered-gap lists and the old encoder's story stay in `docs/log`. `csbgen/README.md` is new: files, use, conventions, tests |
| No DDK inputs | the build and the tests read only the XML, the test lists and Mesa. The test lists' words are the ones the ISA/PDS/state documentation quotes. Comments no longer name the DDK, a capture rig or USEASM, and the `#i16-fmt` enum nothing used (an unused-variable warning in the generated decoder) is gone |
| Unchanged output | after the restructure, the offline winsys harness (clears, draws, scissors, end-of-tile, over 60 frames) is still byte-identical to um-re 9145c84 (66.9 MB of rings). `mesa/tests/isa-standalone` runs the ISA lists: 317 instructions, 0 failures |
| Warnings | no warnings from the three trees on the host build (clang) or the zig x86_64 build. The PDS library keeps `-Wno-unused-variable` for isaspec's tables of encode-only roots (below) |

### Open

1. **Where the trees go.**
   - `src/sgx545/` is a vendor-neutral top-level directory, like
     `src/etnaviv`. Imagination's code lives in `src/imagination/`, so
     reviewers may want `src/imagination/sgx545/` (or `sgx/`).
   - The state XML could then join `src/imagination/csbgen/` beside `rogue/`
     and `volcanic/`. That needs `src/imagination/meson.build` to enter
     `csbgen` for the gallium driver too, not only for the Vulkan driver and
     tools.
   - A decision for the MR series, not for the code.
2. **The isaspec change** (`mesa/patches/0004`) goes upstream first, as two
   commits:
   - `<param>`s on a derived field of bitset type;
   - the encoder's handling of several `<override>`s defining one field.
   - isaspec has no test suite. The SGX545 ISA test is the first user of
     both, so the commits should point at it, or bring a minimal XML case.
3. **The PDS decoder's unused tables.** The DOUT data words are encode-only
   roots, and isaspec emits decode tables nothing uses for them, so
   `-Wno-unused-variable` is needed. Options:
   - isaspec skips decode tables for roots no `#instruction` reaches;
   - the data words move to a second XML that only the encoder reads.
4. **The documentation the comments cite.** `docs/ISA.md`, `docs/PDS.md`
   and `docs/STATE.md` live in sgx545-ce. Upstream needs them either as
   `docs/drivers/sgx545/*.rst` in Mesa or behind a stable link from each
   README. The comments cite them by section name.
5. **A disassembler tool.** `etnaviv-disasm` is built with
   `-Dtools=etnaviv`. Ours is `sgx545_pds_test -d` and the ISA test's text
   output. A `sgx545-disasm` (USSE and PDS) behind `-Dtools=sgx545` needs a
   `meson.options` entry.
6. **csbgen's unknown bits.** Constant bits of unknown meaning are
   `unknown_<bits>` fields with defaults. csbgen also has `mbo`
   (must-be-one), which fits only bits that are always 1. A reviewer may
   prefer fewer named unknowns. The spec side tags each field known,
   only-constants-seen, or unknown (STATE.md "Field layouts for packing").
7. **PDS word 1 for one unit with no secondaries.** The winsys writes 0xb,
   but the documented value is 12 (`18030001`). The packers keep the
   winsys's bytes. Settling it is a driver change: winsys/driver owners.
8. **`src/sgx545/compiler`** isn't covered by this pass (the compiler
   session's tree): no `.clang-format`, and it has its own tests.
9. **The gallium/meson hooks** (`mesa/patches/0001`, `0002`) become part of
   the driver's MR series. Nothing in these three trees depends on them
   beyond `with_gallium_sgx545` entering `src/sgx545`.
