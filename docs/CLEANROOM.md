# Clean-room process

Decided by Joseph, 2026-11-01: "clean room as much as possible so we can really keep the
whole thing open source and non reliant on obscure binaries". This page says how the work
is split so that everything we ship is written from our own documentation, and nothing
that ships needs a proprietary binary to build or run. It refines AGENTS.md §3.5 and §3.6.

## Two roles

**Spec side.** Studies the hardware and the proprietary stack and writes what it learns
into `docs/`, in its own words.
- Sources it may use: traces of the DDK and Mesa-style A/B captures, hardware tests on
  the EA boxes (`tools/isa/hwt_*`, replays with one field poked), black-box use of
  proprietary binaries as oracles (encode this, draw that, compare), and static analysis
  of the DDK (disassembly, decompiles).
- Every fact is tagged with how it was found (`hardware`, `capture`, `oracle`, `static`).
  A `static` fact must be confirmed by `hardware`, `capture` or `oracle` evidence before
  shipping code depends on it.
- Output: `docs/*.md` and test material under `tools/`. Never shipping code.

**Implementation side.** Writes the code that ships, from `docs/` only.
- Reads `docs/`, the public Linux and Mesa sources, and public specifications. It does
  not open decompiler output, disassembly listings of proprietary code, `tools/isa/hw*.py`
  or any other table built while reading proprietary code, or the DDK binaries.
- When `docs/` is missing something or seems wrong, it asks the spec side to fix the doc.
  It never works the answer out from proprietary material itself.
- Output: `mesa/` (driver and compiler), `ukernel/`, the new kernel driver, `srvinit/`.

**Validation** is black-box. The implementation's output (instruction words, frames,
register writes) is compared with the oracle or the hardware, and mismatches go back to
the spec side as doc fixes. Telling the implementation side "word N differs, see
ISA.md §x" is fine. Handing it the oracle's table or code is not.

Sessions are assigned one role. A session that has read decompiles or listings stays on
the spec side for that subsystem.

## What this means per component

| Component | Today | Clean-room plan |
|---|---|---|
| Compiler ISA (`mesa/src/sgx545/compiler`, `usse_forms.h`) | Generated from `tools/isa/hw.py`, which was written while reading libusc decompiles and later confirmed black-box (`docs/COMPILER-UPSTREAM.md` §2) | Rewritten from `docs/ISA.md` only, as a Mesa isaspec XML, by a session that never saw `hw.py` or the decompiles. Validated black-box against the encoder oracle and hardware. `hw.py` stays in `tools/` as a test oracle. |
| Mesa gallium driver (`mesa/src/gallium/drivers/sgx545`) | Written from `docs/STATE.md`, `PDS.md`, `TEX.md`, `GEN.md`, partly by sessions that also read DDK decompiles | Provenance audit; anything written from decompiles rather than docs gets rewritten from the docs |
| Open microkernel (`ukernel/`) | Written from `docs/UKERNEL.md`, by the session that also produced the spec from disassembly | Before linux-firmware submission, re-implementation or review from `UKERNEL.md` alone by a fresh session |
| Kernel driver (M2) | Not started | Implementation side from day one |

## Binaries

- **Build:** nothing we ship needs a DDK input. The open microkernel builds from
  `ukernel/build.py` alone. `srvinit -o` is the target default; the `-f` path,
  `fetch-firmware.sh` and `tools/ddk/` exist only for A/B testing.
- **Run:** the target stack loads no Intel or Imagination code at any point.
- **Tests:** the DDK stays useful as an oracle for A/B comparisons, but no shipping test
  depends on it, and none of it is committed.

## License

Our code is MIT, with `Copyright © 2026 Joseph Telaak` and an SPDX line (Joseph,
2026-11-01). That includes the open microkernel. The kernel driver will be GPL-2.0 as
Linux requires, and the uAPI header dual-licensed (AGENTS.md §3.1).
