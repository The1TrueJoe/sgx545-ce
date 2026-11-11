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
8. **The isolation model was presented as universal.** On a one-directory core the
   firmware is in every client's address space, which no table entry can fix. That is a
   uAPI promise (UAPI.md 7.7, D13), not a porting detail. The guide should send a
   single-context core to single-context.md before any code.
9. **The out-of-tree DDK patch breaks on any refactor of the files it touches.**
   `drm/ci/ddk-compat.patch` has to be rebased (a 3-way `git merge-file` against the old
   patched tree) whenever those files change, or the CE5300 CI fails.

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
