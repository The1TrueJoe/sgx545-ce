# What the SGX530 taught the porting guides

The SGX530 is the rehearsal for folding in more cores (the SGX535 next). Each session that
brings it up appends what the guides got wrong or left out, and every SGX545 assumption it
had to turn into a per-core field. Facts about the core itself are in
[cores/sgx530.md](cores/sgx530.md); this page is about the guides and the code's shape.

## Kernel side (powervr_sgx)

### Where docs/porting/kernel.md was wrong, missing or misleading

1. **Stage 1 assumed every bus hands the driver a clocked core.** For a device-tree SoC it
   said "enables the clocks and the power domain", but on TI parts the GPU node has no
   clocks: the ti-sysc parent module owns them and applies the SGX interrupt quirk when the
   child is runtime-resumed. The probe has to hold the parent awake around the first read.
   Fixed in the guide. Other SoCs (Allwinner, Ingenic) put clocks on the node, and the
   driver refuses those until it handles them.
2. **"Map `reg`" maps 32 MiB on the OMAP3**, a window with nothing named past the
   registers. The driver maps only `PVRSGX_REGS_MIN`. The guide now says so.
3. **A known core the driver can't run was refused before its ID was read.** The guide's
   "a part that only gets as far as stage 3 stays out" needs the ID and revision read
   first. The table now lists the core, the probe reads the two registers, logs them and
   stops.
4. **The table section listed `PVRSGX_FEATURES_REQUIRED` and a 32-bit `va_bits` as hard
   requirements.** One kick, one memory context and 28 bits each needed code, not just a
   table row. They are per-core properties now, each independent (single-context.md
   section 0). The guide's "what is not per-core yet" list was updated.
5. **Nothing said that a register a core lacks must be guarded.** Absent registers are 0 in
   the table, and the code wrote them unconditionally: `CLKGATECTL2`'s write would have
   landed on `CLKGATECTL` at offset 0 on the 530. Use `sgx_wr_opt()` for every table
   register that a core may lack, and skip it in the dump. KUnit `powervr_sgx_regs`
   checks this.
6. **The ARM build wasn't considered.** `set_pages_array_wc/wb`, `drm_clflush_sg` and
   `page_address()` of a "write-combined" page are x86-only ideas. On ARM the linear map
   stays cacheable, so the pages are cleaned once through the DMA API, page tables get a
   write-combined vmap, and imports are synced with `dma_sync_sgtable_*`
   (`pvrsgx_cache.h`). A new architecture should start there.
7. **There was no way to build for a board.** CI has to build the board's exact kernel
   config, or the module's symbol versions won't match. `drm/ci/omap3630.config` plus a
   fragment works; omap2plus_defconfig with DRM=m doesn't, because the board has DRM=y.
   **The config isn't enough: the compiler must match too.** The first modules, built with
   Ubuntu's gcc 13, were refused on the board ("disagrees about version of symbol
   dev_driver_string", ...). Its kernel is from gcc 15.3, which sets `CC_HAS_COUNTED_BY`
   and other options that change what genksyms hashes. CI now uses a gcc 15.3 toolchain and
   checks those options against the board's config. Check the board's `CONFIG_CC_VERSION_TEXT`
   before the first load.
8. **The isolation model was presented as universal.** On a one-directory core the
   firmware is in every client's address space, which no table entry can fix. That is a
   uAPI promise (UAPI.md 7.7, D13), not a porting detail. The guide should send a
   single-context core to single-context.md before any code.
9. **The out-of-tree DDK patch breaks on any refactor of the files it touches.**
   `drm/ci/ddk-compat.patch` has to be rebased (a 3-way `git merge-file` against the old
   patched tree) whenever those files change, or the CE5300 CI fails.
10. **Stage 1 assumed nothing ran between the start and the status read.** On a
    device-tree core, runtime PM is active right after probe and autosuspends 500 ms
    later. The suspend sends the microkernel's power-off through the CCB, stage 6 work,
    and stops the heartbeat the runbook reads a second later. The first attempt pinned it
    by hand (`power/control` = `on`). The driver now holds a runtime-PM reference on a
    single-context core (kernel.md section 9).
11. **A failed start said too little.** One line after 500 ms (init 0, two memory reads
    outstanding, no fault) couldn't tell a stuck interconnect from a stalled table walk
    or a program that never ran. On a core with guessed event registers, the start now
    logs `CLKGATESTATUS`, `EVENT_STATUS`, `BIF_MEM_REQ_STAT` and `BIF_INT_STAT` with the
    clocks on, before the kick, and every 100 ms after it. Where the SoC has a readable
    interconnect wrapper (the OMAP36xx's OCP block), it logs that too. Plan the
    diagnostics into stage 1 before the first attempt, not after it.
12. **The SoC's interconnect wrapper is part of the core's bring-up.** TI's GPL OMAP3
    services name it (`ocpdefs.h`), and mainline's ti-sysc programs it. The fact sheet
    lists it ([cores/sgx530.md](cores/sgx530.md), "The OCP wrapper"). Another SoC's
    glue may have its own.

### SGX545 assumptions turned into per-core fields

| Assumption | Was | Now |
|---|---|---|
| The heap layout (UAPI.md section 5) | `PVRSGX_HEAP_*` constants everywhere | `family.heaps` (`struct pvrsgx_heap_layout`); the constants only fill the 545's |
| The client heap table for `HEAP_INFO` and `VM_MAP` | a static array | built from `family.heaps` |
| VA space of a VM | `SZ_4G` | `BIT_ULL(family.va_bits)` |
| PB size limit | 0x8000 pages | `heaps.pb_max_pages` |
| Bank registers | `SGX_CR_BIF_BANK_SET`, `SGX_CR_BIF_BANK0` | `regs.bif_bank_set`, `regs.bif_bank0`, 0 when absent |
| The EDM's own list | list 15 | `num_dir_lists - 1`, which is 0 on a single-context core (shared with the jobs) |
| What list 0 holds when idle | an empty directory | `pvrsgx_idle_mmu()`: the firmware's on a single-context core |
| BIF fault address field | b12-31 | `regs.bif_fault_addr` |
| USE code base data-master field | b25-26 (`0x2000000`, `0x6000000`) | `regs.use_code_base_dm_shift` |
| The init kick | `EVENT_KICK2` | `EVENT_KICK` when `regs.event_kick2` is 0 |
| The microkernel's code base in DOUTU | `PVRSGX_HEAP_KERNEL_CODE` | `pvrsgx_doutu(kcode, addr)` |
| The entry program's data layout | the 545's two-word interleave | `PDS_DATA_INTERLEAVE_2DWORDS`; a layout-safe program otherwise |
| HW context directory word | the PD's physical address | `pvrsgx_vm_pd_word()`: a PD-table slot on a single-context core |
| Secure-mode firmware | required | required on multi-context cores; single-context needs `SINGLE_CONTEXT` instead |
| Overlap (credit 2) | a module parameter | off on single-context cores |
| Errata | none handled | `family.quirks` (`BRN_28889`: split invalidates in the CCB) |
| Bus | PCI or gma500 | plus device tree (`pvrsgx_of.c`) |

### Open, for the next core

- The SGX535 behind gma500 (Poulsbo) needs the gma500 auxiliary device for the 535, and
  its fact sheet notes gma500's off-by-one directory-list offsets.
- `regs.event_pds_*` for any core but the 545 come from a hardware test, not a header.
  Plan a stage-1 test for them.
- The validator has one USSE table. A core whose encodings differ (pairing, phases) needs
  `family.isa` to select a second one.

## Microkernel, ISA and Mesa side (ukernel/, tools/isa/, mesa/)

### Where the guides were wrong, missing or misleading

1. **"A config file plus tests, never handler logic" is too strong.** UKERNEL-TOOLING §3
   and §7 say a core with no behavioral difference is only a config file, and a
   contributor never edits the handler logic. The 530 needed four handler paths that the
   545 doesn't have: one memory context, BRN_28889, bounded waits, and a heartbeat word.
   They went in as per-core feature flags in `ukernel.py`, with SGX545 defaults that emit
   the old code, so the 545 image is unchanged. The guides should say that a feature flag
   in `ukernel.py`, defaulted so every existing image stays byte-identical, is the expected
   way. What a new core must not do is fork.
2. **"Overrides only what moves" misses fields.** The register *offsets* that moved were
   the easy part. Bit *fields* moved too: the ISP field of `CLKGATECTL`, the ISP bit of
   `CLKGATESTATUS`, the TA bit of `SOFT_RESET`, and the mask of `BIF_MEM_REQ_STAT`. The
   545 code had them as literals (`limm r7, #3`, `limm r5, #4`). Every literal bit pattern
   in a handler is a per-core field until shown otherwise.
3. **The BIF model is assumed.** microkernel.md stage 4 ("point the BIF at the context
   PD"), stage 8 (secure mode) and UKERNEL.md's bank scheme all assume several directory
   lists and per-requestor banks. A one-context core changes the host interface:
   - the EDM runs on the job's PD;
   - the firmware heaps must be in every client PD;
   - the PD has to come from somewhere a client can't write;
   - the firmware PD goes back before any completion write;
   - secure mode can't hold.
   None of that is in the guides, and it took a design round with the kernel side. Stage 8
   should say it applies to multi-context cores only.
4. **Unnamed registers.** The guides say to discover addresses "from captures and the
   emulator, not by scanning", and that a new core overrides only what moved. The
   event-program registers (`EVENT_PDS_ENABLE`/`BASE`/`DATA0`/`DATA1`) and the EDM task
   registers aren't in any GPL header for either core; the 545's were measured. On a core
   with no working vendor driver there is no capture. The guides need a method for this
   case:
   - infer the offset from the neighboring registers the headers do name (tag it G);
   - confirm it at the first boot with one named read and a write/read-back;
   - replace what can't be named (the EDM task registers for the watchdog) with a
     microkernel-written word.
5. **A wait on a missing bit freezes the EDM.** Nothing in the safety section warns that
   every `wait()` on a status bit the new core lacks spins forever, and that this looks
   like a dead core. The 530 build bounds every wait and logs the one that timed out in the
   control block. A first boot on any new core should do the same.
6. **The ISA instrument needs a frame first.** isa.md says to run `hwt_sanity` first, as if
   the instrument were there from the start. `hwt.py` replays a captured vendor frame,
   which a core without a working vendor stack doesn't have. `hwt_open` draws through the
   open stack, so it needs the microkernel through stage 5 (a frame) on the new core. The
   order is microkernel first, ISA tests second, and the guides should say so.
7. **The microkernel's ISA surface is small.** Neither guide says that the microkernel uses
   only integer, bitwise, test, load/store and flow forms, with no float, F16, C10 or efo.
   So the microkernel can be brought up on the 545's encodings of those forms, and only
   they need confirming before it can move onto `sgx-asm -c <core>`. The open F16, C10
   and efo questions only block the compiler.
8. **PDS data layout per core.** isa.md's list of what may differ per core has no PDS row
   beyond "the PDS program words". The 530 lacks `PDS_DATA_INTERLEAVE_2DWORDS`, so PDS.md's
   two-word DS0/DS1 interleave is the 545's. Every PDS program depends on it: the kernel's
   microkernel entry and event programs, and Mesa's vertex and pixel programs.
9. **The multi-kick gap is a host problem only.** The kernel audit lists "Microkernel
   start: needs EVENT_KICK2" against cores without it. For the microkernel nothing changes:
   the event entry tells init from a kick by `ui32InitStatus`. Only the host's init kick
   moves.
10. **The Mesa guide and table don't exist where expected.** The brief named
    `mesa/src/sgx/common/sgx_core.*` and `docs/porting/mesa.md`; neither exists. Mesa's
    per-core table is `sgx_devices[]` in `src/gallium/drivers/sgx/sgx_device.c`, keyed by
    the `CORE_ID` id field, and a new core's ID is unknown until the first hardware read.
    The pattern used:
    - core id 0 is inert (it matches nothing);
    - a `provisional` flag makes the screen warn once;
    - the SGX545's limits stay, marked unconfirmed;
    - F16 is off until confirmed.
11. **Board userspace isn't glibc.** `mesa/build.sh` had glibc targets only. Our OMAP3630
    test board is musl, so the `armv7` target is `arm-linux-musleabihf`. Ask the board
    owner for the libc before the first cross build.
12. **isaspec gen ranges.** UKERNEL-TOOLING §1 says "a gen range widens past 545 only when
    ISA.md documents that field for that core", as if per bitset. isaspec allows one
    contiguous range per bitset, with a child's range inside its parent's. The plan
    (cleanroom-isa) works differently:
    - widen the root once (53000-54599);
    - pin every 545-only class to 54500-54599;
    - give a form that differs per core two same-name bitsets;
    - give a field that differs inside a form an override on `ISA_GPU_ID()/100`;
    - have sgx-asm refuse forms outside the core instead of emitting 0.

13. **The firmware packager has 545 assumptions too.** `drm/tools/mkukfw.py` sits between
    the microkernel build and the loader. It knew only the `ctlblk` patch (`--manifest`
    packaged that one alone), only the patch values 1-5 and only feature bits 0-1. The
    530's second patch (`pdtable`, loader value 6) and its feature bit 2 needed all three
    to change. None of the guides mention the packager. A new core's microkernel changes
    can need a `drm/tools` change, so the guides should list it with the loader, and the
    manifest's patches should be packaged by name, all of them, rather than one fixed
    patch.

### SGX545 assumptions turned into per-core fields

In `ukernel/cores/sgx545.py`, at their SGX545 values; `sgx530.py` overrides them.

| Field | Was | Why |
|---|---|---|
| `MULTI_MEM_CONTEXTS` | implicit (list 1 + `BANK0` at every job) | the 530 has one directory list |
| `CTX_DIR_LIST` (derived) | `0xc38` literal | the job's list: 1, or 0 on one context |
| `CR_BIF_DIR_LIST_BASE0`/`1`, `CR_BIF_BANK0`, `CR_BIF_CTRL`, `BIF_CTRL_*` | literals | absent registers must fail the build (`None`) |
| `CR_BIF_MEM_REQ_STAT`, `BIF_MEM_REQ_MASK` | not used | the BRN_28889 drain; b0-10 against b0-7 |
| `PD_TABLE_SLOTS`, the `pdtable` patch | none | one-context cores take the PD from the host's read-only table |
| `FIX_HW_BRN_28889` | none | the 530's erratum |
| `WAIT_BOUND`, `WAIT_TIMEOUT_ID`/`CT` | unbounded waits | bring-up on unconfirmed bits |
| `HEARTBEAT` | the host read `EDM_TASK_REG0/1` | unnamed on the 530 |
| `CR_CLKGATECTL`, `CR_CLKGATESTATUS`, `CLKGATE_ISP_MASK`/`AUTO`, `CLKGATESTATUS_ISP` | `0x0`/`0x8`, `#3`/`#2`/`#1` | status moved to 0x4, ISP field to b4-5 |
| `CR_SOFT_RESET`, `SOFT_RESET_TA` | `0x80`, `#4` | the 2D reset takes b1, so the TA is b3 |
| `DCU_INVALID` 0 = no acknowledge | unconditional `EVENT_STATUS2` b10 ack | the 530's `STATUS2` has only b0-1 |
| `OTPM_CLEAR_WAITS` | two fixed waits | `STATUS2` b12 doesn't exist on the 530 |
| `MULTI_EVENT_KICK` | none | documents that the microkernel needs no change |
| `FEATURES` (manifest default) | `UK_FEATURES` default 2 | the 530 can't be `SECURE` |
| `MISC_CORE`, `CR_CORE_ID`, `CR_CORE_REVISION` | already per core | (were already) |

Tooling made per-core: `golden/check.py <core>` (it built only the 545), a golden per
core, the manifest's `waits` map (only when waits are bounded), the ISA coverage gate run
with `--core`, and `test_sgx530.py` next to `test_emu.py`.

### 2026-11-11: before the first start

14. **"The first boot" isn't only a host-side step.** microkernel.md's stage 1 says what the
    host checks (`ui32InitStatus`, the heartbeat), not what the microkernel touches to get
    there. The 530 image's init wrote two SGX545 DPM registers (0x65c, 0x630) that nothing
    names for the 530, so the first boot would also have been the first write to unnamed
    MMIO. Before the first start on a new core, list in the emulator every register the
    init, timer and get-misc-info paths touch, and gate any that the core's header doesn't
    name. The 530 does that with `INIT_DPM_CLEAR` (545: the two literal `str`s at init; 530:
    none), and `test_sgx530.py` "stage 1" keeps it that way. The same trace lists the
    instruction forms stage 1 executes, so a passing boot is coarse evidence for those
    forms on the new core.

### 2026-11-11: the assembler's core, and what a third core needs from the tooling

15. **"Assemble with `sgx-asm -c <core>`" assumes sgx.xml already describes the core.**
    UKERNEL-TOOLING §2 makes `build.py` pass the image's core to `sgx-asm`, and
    `sgx-asm -c 530` refuses because sgx.xml describes only the SGX545. The image's core
    and its instruction encodings are separate facts. `ISA_CORE` in the core config says
    whose encodings it uses: 545 for the SGX530 until sgx.xml has the SGX530's confirmed
    forms. The manifest records it (`"isa"`) when it differs from the core, and CI checks
    the image through `sgx-asm -c sgx<ISA_CORE>` against the golden.

**What the SGX535 will need**, from what the SGX530 took ([sgx535.md](cores/sgx535.md)):

16. **A config that mixes the two.** The SGX535 has the SGX530's low registers and the
    SGX545's BIF (16 lists, banks, 32 bits), one kick, and none of BRN_28889. So
    `cores/sgx535.py` would start from `sgx530` and set `MULTI_MEM_CONTEXTS`, the list-1
    and `BANK0` offsets, `FIX_HW_BRN_28889` off and `SECURE` possible, and keep the
    bounded waits and the heartbeat for bring-up. That works only because every
    difference is an independent flag. Multi-context with one kick is a combination no
    image has built yet. The event entry doesn't care (entry 9), but the 535 needs its own
    golden and emulator cases.
17. **Shared test harness.** `test_sgx530.py` carries its own harness (the machine, the
    kernel CCB queue, the event run, the stage-1 register and form listing). It should
    move to a module that `test_sgx535.py` imports, leaving each file only its core's
    cases. The stage-1 allowlist belongs in the core config, as the set of registers the
    core's K header names, not as a test constant.
18. **A per-core register model in the emulator.** `tools/ukernel/emu.py` models the
    SGX545's registers for every core, so an access to an offset the core lacks only
    shows when a test lists it. If the emulator knew the core's named set, and flagged
    anything outside it that no "accepted unknown" list covers, gap-13 registers would
    show in every test, not only stage 1.
19. **One list of cores for CI.** Each core adds a line to `ukernel-golden`,
    `ukernel-sgxasm` and `ukernel-isa-gate` (golden, `sgx-asm` golden, coverage). A
    `CORES` list in `ukernel/cores/` that the workflows loop over would make a third core
    one config file plus tests, as UKERNEL-TOOLING promises.
20. **Revision first.** Poulsbo's revision is U (the DDK lists four). As for the 530,
    `mkukfw --core-rev`, the kernel's part entry and Mesa's table entry wait for the
    first ID read, and Mesa's provisional entry pattern (entry 10) applies.
21. **Mesa target.** The Atom Z5xx has no x86-64, so the 535 uses `build.sh i386`, which
    is glibc and already built. gma500 hosts it through the auxiliary device, as on
    Cedarview, so the armv7 work doesn't carry over.

### 2026-11-11: the ISA delta

22. **The microkernel's ISA surface isn't risk-free just because it's small.** Entry 7 said
    the microkernel can come up on the SGX545's encodings because it uses only integer,
    load/store and flow forms. True, but its very first instruction is `phas.i`, and the
    SGX530 lacks `SGX_FEATURE_USE_UNLIMITED_PHASES` (and `USE_NO_INSTRUCTION_PAIRING`).
    isa.md's list of what may differ per core should name the scheduling features
    (phases, pairing) next to the formats. Before a first start, list the image's forms
    with the features that could change each (ISA.md, "Other cores: the SGX530"), and
    put the riskiest form on the failure table, so a dead init points at it.

### 2026-11-11: the first start

23. **Log the memory-side progress words when a start fails, not only when it passes.** The
    runbook asked for the heartbeat after a start. The first attempt timed out, the probe
    freed the arenas, and the one word that splits "the USSE never ran" from "init
    stalled" was gone. The event entry's first write is the heartbeat (instruction 6). It
    comes after a PDS fetch, a USSE fetch and one load, so on a failure it localizes the
    stall from memory alone, with no register read. For any core, the failure path should
    dump the control block's progress words from the CPU mapping before teardown. The
    runbook for each start should say, for the image, which word the event writes
    first.
24. **`imae` adds 16-bit sources.** The heartbeat is `imae r3, r3, #1, #1`, so it counts
    to 0x10000 and then restarts at 1: every 65 s at 1 kHz. A watchdog that only asks
    "did it change" is fine. One that asks "did it grow" trips at the wrap. The first
    diagnostic image put a 32-bit marker in the heartbeat and lost its high half at the
    first increment. The emulator caught it, since it models imae's source width. Put
    32-bit markers in words that nothing increments.
