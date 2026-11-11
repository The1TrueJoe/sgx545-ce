# Microkernel tooling for the SGX family (design)

**Status: approved by Joseph, 2026-11-11. Migration in progress (§6).** Both
recommendations were accepted: the core config is a Python module of plain
constants, and `isaspec` patch 0004 and the core-id parameter are carried pinned
until they land upstream.

Today's microkernel is one Python module, `ukernel/ukernel.py`, hardcoded to
SGX545 (`CORE_NAME = "sgx545"`), assembled by `tools/isa/asm.py` and packaged by
`drm/tools/mkukfw.py`. It works, and its serial image is a fixed 14400 bytes we
treat as a regression oracle. This document proposes how to grow that into tooling
for the whole SGX family without forking per core and without losing that
byte-for-byte regression check.

**Cores, in the order we expect to support them:**

| core | USSE | notes |
|---|---|---|
| SGX545 | USSE1 | shipping; CE5300 (rev 1.0.14) and Cedarview (rev 1.0.13) |
| SGX535 | USSE1 | next; the original SGX, single pipe |
| SGX530 / SGX540 | USSE1 | later; low-end variants |
| SGX543 / SGX544 / SGX554 | USSE2 | later; dual USC, MP configurations |

**Fixed decisions (from Joseph; this builds on them, does not relitigate):**

- Upstream layout: one kernel driver (`powervr_sgx`) with a per-core table; one
  Mesa driver; firmware binaries per core into `linux-firmware`
  (`powervr_sgx/sgx<core>_ukernel.bin`) with `WHENCE` pointing at the source; the
  microkernel source and its assembler in their own repository, because Mesa does
  not host firmware.
- **One** instruction-set description is the source of truth. It lives in Mesa and
  is used by both the Mesa compiler's encoder and the microkernel assembler. The
  firmware repo pins a copy; CI checks it still matches Mesa's, so the two encoders
  cannot drift.
- A Python assembler is fine. The kernel build never runs it (it consumes the
  prebuilt `.bin`).

**One invariant above all:** every step here keeps the SGX545 serial image
byte-identical (14400 B). It is the regression test for the whole refactor — if a
generalization changes the 545 output by one byte, it is wrong until explained.

---

## 1. The ISA description

**It already exists.** The description is `mesa/src/sgx/isa/sgx.xml`, in Mesa's
`isaspec` XML format, authored from `docs/ISA.md` alone (clean-room). Meson
generates Mesa's decoder and encoder tables from it, and Mesa's text assembler
(`sgx_isa_asm.c`) is hand-written C over the generated `struct sgx_isa_instr`. So
the "one description in Mesa" decision is already met on the Mesa side. The tooling
work is to bring the **microkernel's** Python assembler onto the same `sgx.xml` and
off the current assembler's encoding tables (`tools/isa/hw.py`), which are not from
`ISA.md` — that move is the clean-room fix (§2); the process is in
[`CLEANROOM.md`](CLEANROOM.md). *(The roles: cleanroom-isa is the implementation side — it authors `sgx.xml`
from `ISA.md` and never reads `hw.py`; usse-compiler is the spec side — it
writes `ISA.md` and runs `blind.py` against the oracle.)*

**The clean-room invariant** (cleanroom-isa, verbatim):

> No build product (Mesa, the µK firmware, the assemblers) imports, links or is
> generated from `hw.py`, `fields.json` or `names.json`. The ISA description is
> authored from `ISA.md` alone; the oracle contributes encoded words for black-box
> comparison, never field layouts or names; its disagreements return only as
> `ISA.md` changes made by the spec side.

`hw.py` stays a spec-side test oracle, run by hand (`blind.py`, `hwt.py`,
`check_enc.py`); no build or shipping test imports it. `sgx.xml` is **hand-written**
to `ISA.md` — only the C decoder and
encoder are generated from `sgx.xml`, not `sgx.xml` from `ISA.md`. A disagreement
between the description and the oracle is fixed by changing `ISA.md` (the spec side,
usse-compiler); the implementation side (cleanroom-isa) then updates `sgx.xml` to
match — never by copying the oracle's tables.

**Family variants.** `isaspec` expresses core variants with `<gen min= max=>` on
bitsets and fields, the way freedreno keeps a3xx–a7xx in one ir3 XML. The USSE1
cores (530/535/540/545) are gens of one `sgx.xml`: a gen range widens past 545 only
when `ISA.md` documents that field for that core, and each core gets its own
`ISA.md` section, its own corpus and its own `blind.py` score (passing on 545 says
nothing about 535). Per-core field differences are `isaspec <override>` expressions
on a core-revision parameter, with a features list for whole instruction classes a
core lacks (`fmad16`, `efo`, `smp.f16`, …) — usse-compiler. Today `isaspec`'s
`<gen>` ranges compare against `gpu_id`, which carries only the format-control bits
(bit 0 C10, bit 1 F16); so a core-id needs a `gpu_id` encoding such as core-number ×
4 plus the fc bits, with the fc expressions masking the low bits. cleanroom-isa
designs that when the spec side writes a second core's `ISA.md` section; until then
there is nothing to gate on. USSE2 (543/544/554) is **not** in clean-room scope yet —
`docs/` has nothing on it — so the plan leaves room for a separate `sgx2.xml` that
shares the generators and tests but no bitsets; whether to merge it waits until
`ISA.md` has a USSE2 section. Leave room for it; do not design it now.

**The pinned copy.** The firmware repo pins `sgx.xml` (plus the in-flight `isaspec`
patch 0004 — derived bitset params and override fields — until it lands upstream).
CI then does two checks (usse-compiler): it compares the pinned `sgx.xml` and the
isaspec tooling revision **by hash** against Mesa's at the pinned Mesa commit
("same description"), and it **assembles the µK sources with the pinned tool and
diffs the binary** ("same output"). The two together make drift impossible.

**Validation is not a cross-check.** Once both the encoder and the assembler come
from one description, checking them against each other proves nothing. Independent
validation is (a) `blind.py` against the oracle over each core's corpus
(usse-compiler, the spec side, owns this; the implementation side never runs the
oracle and only gets back pass/fail plus the failing lines and words), and (b) a
text→encode→decode→text round trip (Mesa's `sgx_isa_test`, and the µK assembler's
own — §5). Moving the µK assembler onto `sgx.xml` takes `hw.py` out of
everything that ships.

---

## 2. The assembler

The microkernel is written in USSE assembly (`ukernel.py`'s `a.i("fmad r1, pa2,
-sa3, c52 [rpt=4 moe]")` lines) and assembled at build time; today that is
`tools/isa/asm.py` riding `hw.py`, whose tables are not from `ISA.md`. The
clean-room fix (§1) is
to assemble against `sgx.xml` instead — and, per usse-compiler, **without writing a
second encoder.** There is one tested encoder (Mesa's, generated from `sgx.xml` by
`isaspec`, with the hand-written C text assembler `sgx_isa_asm.c` over it,
round-tripped by `sgx_isa_test`). The recommended way (cleanroom-isa): a host
`sgx-asm` / `sgx-disasm` tool built from `mesa/src/sgx/isa` behind `-Dtools` — it is
already Open item 5 of the Mesa upstream-readiness list, so it serves Mesa too. The
µK build shells out to it (or links a batch mode, one process per file) to assemble
each line. A fallback, only if the build cannot run a host tool: a Python encode
walk over isaspec's own model of `sgx.xml` (`isa.py` parses the XML; the encode path
it generates is C). That copies no tables, but it is a second encode implementation
that needs its own tests — so the host tool is the default.

The encoder tables are never duplicated and never touch `hw.py`, `fields.json` or
`names.json`. The remaining pieces are µK-specific authoring sugar, not a new
encoder:

- **Syntax.** What `sgx_disasm` (Mesa's generated decoder) prints, as `ISA.md`
  defines it: optional predicate, mnemonic with `.modifiers`, operands in hardware
  slot order, bracketed flags and fields. (Not `tools/isa/dis.py`, which sits next to
  `hw.py` and is not from `ISA.md`.) µK lines `sgx_isa_asm.c` does not parse yet
  are added to the parser from `ISA.md`, never copied from `asm.py`.
- **Macros / includes.** The microkernel today builds instructions through Python
  helpers (`invalidate()`, `dpm_freelist()`, `wait()`); those stay as the authoring
  layer. The assembler proper needs: symbolic constants (register addresses,
  control-block word offsets) and simple include of a per-core constants file, so a
  line can read `str #{CR_3D_KICK}` instead of a literal.
- **Symbols and relocations for the control block.** The event program carries the
  control-block base in a loader-patched `limm` (§1 of `UKERNEL.md`); control-block
  words are referenced symbolically (`SC_INFLIGHT`, `TD_ENTRY`, …) and resolved at
  assemble time from the per-core layout, so the same source assembles for a core
  whose layout differs.
- **Listing / map output.** `build.py` already emits a manifest of entry offsets;
  the assembler should also emit a full listing (address, bytes, source line) and a
  symbol map, for debugging a hang against a devcd PC.
- **Disassembler for round-trip.** The round trip is `sgx-asm ∘ sgx-disasm` ==
  identity over each ISA's corpus (§5), both built from `sgx.xml` — not the
  `hw.py`-based `dis.py`.

---

## 3. Per-core microkernel configuration

Config files per core, **not** forks. Today the core specifics are scattered:
`CORE_NAME = "sgx545"`, hardcoded register addresses, and env flags for the
1.0.13-vs-1.0.14 variants (`UK_XFER_BANK`). The design collects them into one
per-core config the build selects.

**A core config declares:**

- **Register map:** the `CR_*` addresses (`CR_3D_KICK`, `CR_TA_KICK`, the DPM
  registers, BIF, event status/clear). Most are shared across the family; the config
  overrides only what moves.
- **Feature flags:** EDM present, DPM details (slot count, the OOM path), multi-core
  for the MP parts (543/544/554 MP), the USSE variant (selects the §1 ISA tables),
  core-revision quirks (the 1.0.13 transfer-BANK variant becomes a config flag, not
  an env var).
- **Control-block layout:** the word offsets (`SC_*`, `TD_*`, `WAIT_*`). The 545
  layout is the base; a core that needs more or different words overrides them, and
  the assembler resolves symbols against the selected layout (§2).

**How the build picks a core.** `build.py sgx545 out.bin manifest.json` (or
`SGX_CORE=sgx545`), defaulting to sgx545 so existing callers are unchanged. The
config is a data file (`cores/sgx545.py` or `.json`) the builder imports; the
handler logic in `ukernel.py` reads from the selected config instead of module-level
constants. A core with no behavioral difference is *only* a config file.

---

## 4. Repo layout and build

The microkernel source and assembler live in their own repository (the fixed
decision), today `The1TrueJoe/sgx545-ce`'s `ukernel/` + `tools/isa/`. Proposed
shape:

```
ukernel/            the microkernel source (handlers) + per-core configs
  ukernel.py        core-independent handler logic
  cores/            sgx545.py, sgx535.py, ... (register map, features, layout)
  build.py          build.py <core> <out.bin> <manifest.json>
isa/                the ISA description (pinned copy of Mesa's) + generated assembler
tools/              oracle, interleave, run_ours, disassembler (§5)
```

- **Reproducible output.** `build.py <core>` is deterministic; the 545 output is
  byte-identical (14400 B) as the regression check. CI builds every core and
  compares each against its checked-in golden image.
- **Firmware header / versioning.** `build.py` already emits a manifest
  (`core`, `version` = `UK_VERSION` major.minor, entry offsets, `build_id`);
  `drm/tools/mkukfw.py` wraps the `.bin` + manifest into the firmware blob the
  kernel loads (feature bits in the header, e.g. cleanup-v2). The versioning and the
  header checks `mkukfw.py` enforces generalize per core unchanged.
- **CI publishes binaries per core.** CI builds `sgx<core>_ukernel.bin` for each
  core and stages them for `linux-firmware` with `WHENCE` pointing at this repo and
  the exact source commit, so the published firmware is reproducible from source.

---

## 5. Tests

Generalize today's microkernel tooling per core:

- **The oracle / interleave tools.** `tools/ukernel/run_ours.py` builds the open
  microkernel and diffs the registers and memory it writes against the DDK
  firmware's, per phase, in the offline emulator (`sgxsim.py`);
  `interleave.py` / `interleave_ours.py` replay two-surface traces in a chosen order
  (how the overlap work was validated). Per core: the DDK reference firmware and the
  core's register model are config; the harness is shared.
- **The `run_ours` harness.** Stays the per-phase register/memory oracle; its "match
  the DDK" assertion is the correctness bar for each core.
- **Disassembler round-trip.** assemble∘disassemble == identity over each ISA's
  corpus, generated from the §1 description — the test that the assembler and the
  encoder agree.
- **The byte-identical golden.** Each core's serial image has a checked-in golden
  `.bin`; CI fails on any diff. This is the regression test that a shared-tooling
  change did not perturb a core.

---

## 6. Migration from `ukernel.py`, smallest steps

Each step keeps the 545 serial image byte-identical (14400 B); none changes
behavior.

1. **Golden-image CI first.** Check in the 14400 B 545 image and a CI job that
   fails on any diff, before touching anything. Every later step is gated on it.
2. **Extract the 545 config.** Move the register map, feature flags, and
   control-block layout out of `ukernel.py` into `cores/sgx545.py`; `ukernel.py`
   reads from it. Output unchanged.
3. **Core selector.** `build.py <core>` defaulting to sgx545; the builder imports
   the selected config. Output unchanged.
4. **Symbolic assembler.** Teach the assembler the per-core constants file so source
   reads `CR_3D_KICK` / `SC_INFLIGHT` symbolically; verify the 545 output is
   unchanged.
5. **ISA coverage gate** (before the switch, usse-compiler). Assemble every µK
   source line through `sgx_isa_asm` / `sgx_isa_test` against `sgx.xml`. Any form the
   µK uses that `sgx.xml` lacks — a special class, MOE control, `ld`/`st` variants,
   the loader-patched `limm` — becomes an `ISA.md` item (spec side) and then an
   `sgx.xml` item (implementation side) first. The switch cannot land until coverage
   is complete.
6. **Assemble through `sgx.xml`.** Switch the µK build from `asm.py`/`hw.py` to the
   `sgx-asm` host tool built from `sgx.xml` (§2). The tool must reproduce the 545
   image bit-for-bit. From here, any byte difference against today's `asm.py` image
   is either an `sgx.xml` bug or an `asm.py`/`hw.py` bug, resolved through `ISA.md`
   (the spec side) — never by making `sgx.xml` match `asm.py`. CI also hash-checks the
   pinned `sgx.xml` against Mesa's (§1).
7. **Second core (SGX535).** Add `cores/sgx535.py` and its golden; nothing in the
   545 path changes. This is the proof the generalization is real.
8. **USSE2 later.** When a 543/544/554 target is real, add the USSE2 ISA tables and
   a core config; the schema already left room (§1).

Steps 1-4 are pure refactors of this repo. Steps 5-6 are the cross-repo piece with
Mesa — the §1 clean-room boundary is settled, coverage is proven, and the µK
assembles through the `sgx-asm` host tool (§2). Steps 7-8 are additive, and step 7
is exactly what an outside contributor does (§7).

---

## 7. Porting the microkernel to a new core

A contributor adds a core with **a config file and tests, never by editing the
assembler or the handler logic.** The staged bring-up blueprint is
[`docs/porting/microkernel.md`](porting/microkernel.md) — the processor booting our
code through OOM and overlap, each stage with its tools, its "done", and the traps
that cost time, plus the fragile-board safety rules. This section is the summary;
approve them together. The likely first outside port is SGX535 — the reMESA SGX535
developer.

**Provenance rule.** Every hardware fact in a core config or `ISA.md` section comes
from tests on real hardware — register probes, captured command streams, devcd
dumps, the oracle. **No code, disassembly or binaries from a vendor driver go in the
repo**, and no config field is copied from one. A vendor image may be *run* as a
black-box oracle to compare outputs (as SGX545's bring-up used the DDK firmware),
but its internals never enter the tree.

**Bring-up order** — each item is a config entry plus a test:

1. **Register map.** The core's `CR_*` addresses: core id/revision first (to confirm
   the part), then event status/clear, the kick registers, DPM and BIF. Most match
   the family; probe only what the config overrides.
2. **Reset and boot.** The host reset / re-init sequence (`UKERNEL.md` §5a) and the
   control-block base hand-off (the loader-patched `limm`). The core boots the
   microkernel and reaches its event loop.
3. **The first kick.** One TA + 3D renders a generated frame (`tools/gen`), read back
   and compared against the core's own black-box oracle — the "frame from code"
   milestone.

**Using the tools on a new core.** The config selects the core's register model and
(when it exists) its `ISA.md` gen; the shared harness does the rest — `run_ours.py`
diffs the microkernel's per-phase registers and memory against the core's reference
firmware in the emulator, `interleave.py` replays multi-render orders, `blind.py`
scores the core's clean-room ISA against its corpus, and the round trip checks the
assembler. So a new core contributes: its config, its `ISA.md` section + corpus (if
its ISA differs from 545's), its reference-firmware oracle, and a golden image.

**Evidence to publish a core's firmware** — before `sgx<core>_ukernel.bin` is staged
for `linux-firmware`:

- the per-phase oracle matches the reference firmware (TA/3D, completion, transfer);
- a real frame renders on real hardware and reads back correct;
- if the ISA differs, `blind.py` passes on the core's corpus and the round trip is
  clean;
- the serial image is reproducible (a checked-in golden, CI diff-clean);
- `WHENCE` points at the source commit, and the provenance rule above is met.

## Open items before implementation

cleanroom-isa and usse-compiler have given their input (§1–§2); both offered to
check the final draft. What is left to settle:

- **The `sgx-asm` host tool** (§2): the recommended mechanism (cleanroom-isa), built
  from `mesa/src/sgx/isa` behind `-Dtools` as Mesa upstream-readiness Open item 5.
  Confirm the µK build shelling out to it is acceptable, and the batch mode.
- **The core-id isaspec parameter** (§1): a new parameter, distinct from `gpu_id`'s
  format-control state, that cleanroom-isa designs so `<override>` can select
  per-core fields.
- **Decided (Joseph, 2026-11-11) — core-config language:** a Python module of plain
  constants (not a general program): imported directly by the Python builder with no
  loader, it matches the existing module and can carry the few derived register
  values a pure-data file cannot, while a contributor still edits only values, never
  logic.
- **Decided (Joseph, 2026-11-11):** carry `isaspec` patch 0004 and the core-id
  parameter pinned until they land upstream; the hash + binary-diff CI (§1) keeps
  Mesa and the µK in lockstep meanwhile, at no cost to reproducibility.

The migration (§6) does not start until Joseph approves.
