# Lessons from the SGX530: the microkernel, ISA and Mesa side

The SGX530 is the rehearsal for adding other cores (the SGX535 is next). This page lists,
as they come up, every place where the porting guides (`docs/porting/*.md`,
`docs/UKERNEL-TOOLING.md`) were wrong, missing or misleading for the 530, and every SGX545
assumption that had to become a per-core field. **Append only:** add new entries at the
end and correct old ones by adding a new entry. The kernel side (`drm/`) keeps its own
list.

## 2026-11-11: the config, the ISA plan, the Mesa build

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

### SGX545 assumptions made per-core fields

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

## 2026-11-11: packaging

13. **The firmware packager has 545 assumptions too.** `drm/tools/mkukfw.py` sits between
    the microkernel build and the loader. It knew only the `ctlblk` patch (`--manifest`
    packaged that one alone), only the patch values 1-5 and only feature bits 0-1. The
    530's second patch (`pdtable`, loader value 6) and its feature bit 2 needed all three
    to change. None of the guides mention the packager. A new core's microkernel changes
    can need a `drm/tools` change, so the guides should list it with the loader, and the
    manifest's patches should be packaged by name, all of them, rather than one fixed
    patch.
