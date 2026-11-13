# Microkernel source: upstream comment pass — provenance moved here

For upstream readiness the microkernel source (`ukernel/ukernel.py`,
`ukernel/cores/sgx545.py`) had its inline bring-up provenance thinned: dev-box
names and dates, session attributions, commit SHAs, vendor-binary program-counter
citations, and internal dump/tool names were removed from the comments. The
mechanism and field explanations stayed; what each behavior *rests on* is recorded
here so none of it is lost. The change is comments-only — the assembled image is
byte-identical (golden `ukernel/golden/sgx545.sha256`, unchanged across the pass).
Companion to `2026-11-08-ukernel-bringup-history.md`.

## Commit → behavior it anchored

- `f073c83` — the kernel driver keeps credit 2 behind the `overlap` flag.
- `e21ff75` — a bare free-list reload at the partial render leaves a seed the ring
  can't track → leak.
- `57cfae8` — overlap credit=2 storm: a bare `BANK0=0x1f` zeros the 3D nibble,
  re-routing the in-flight 3D onto the idle list-0 PD (symptom: BANK0 0x1f, 3D on
  idle `DIR_LIST_BASE0`).
- `fc50a6d` — "ep#1" fault: releasing the held OOM allocation at `ta_finished`
  (before pages freed) over-allocates past the limit → BIF fault `0x1ffff000`.
- `645e48a` — a TA-finish free-list reload seeds a stale mid-scene snapshot → worse
  leak.
- `9efb1cb` — the second `0x658=0` clear made the OOM partial render run as a normal
  render; second resume died.
- `feb0de8` — sync-1 address taken from `rd+0x240` (a TA-command offset, not an rd
  offset) → render hang (DDK renders 10/10 without the bug).
- `568e24f` — proven dispatch order: `0x6` before `0x1000` in `k_oom`.
- `f6fb82b` / `466dc90` — wrong unpause order (unpause LAST) left the TA paused
  through the control clear; the resumed TA grabbed only ~2 pages and stalled.
- `8480a4f` — slot-1 stale DPM control leaks (count recovers, ring short).

## Box + date → behavior confirmed on hardware

- **CE5310 board, 2026-11-02**: `SA_KCCB` must be word `0x16`/+0x58 (not word 16/+0x40);
  the dpm_freelist load is required or the TA never runs; writing `sa` in the bare
  DOUTU event entry wedges init; a DCU-invalidate (STATUS2 `0x400`) must be acked or
  the event storms 16+ and locks the core; the idle `ACTIVE_POWER` raise is needed or
  `SGXOSTimer` HWRs the quiescent EDM; gating `kick` on `SW_EVENT` was wrong
  (GETMISCINFO left unconsumed).
- **CE5310 board, 2026-11-03**: ack TA-finish/3D-end against the snapshot; advance the client
  CCB RO or every TA re-reads kick 1; first-kick gating (re-running per-scene setup
  hangs the TA); the client PD into list 0 wedges the 3D; the `0xa74` RMW must
  preserve `0x208200`; command word 0 is SIZE not flags (do_3d never ran); TPC flush
  before 3D (else ISP hangs, `BIF_FAULT`=0); save CR `0x274` across kicks; dpm_load
  after the TE registers.
- **CE5310 board, 2026-11-04**: a render = rendezvous of `0x40000` + `0x1`; leaving `0x1`
  un-acked lets `free_done` re-raise (else deadlock).
- **CE5300 board, 2026-11-03**: `op0xff`/`op0x100` cache ops give no improvement (10/30) and
  are not the terminate-kick cause; lock/release of the task mutex is HW-neutral; the
  two-task RO race HWRs frames 2+; the CE5300 board p1 fast host delivers kick 2 mid-TA →
  `SC_TAFLIGHT`. DPM log: 25 pages live at TA1 finish, 0 at terminate, no power event
  on 3/5 lockups.
- **CE5310 board, ~frame 40 ("M1 exit"), 2026-11-08**: the `PB_TA_LOADED` per-PB rule;
  `HC_BGBLOCK` reuse of `ui32PerfGroup` is safe; cleanup must not clear the PB markers
  on RT/ctx cleanup; the EDM bgblock pulse faults under secure/no-share at
  `KERNEL_CODE+0x20d0`; a null transfer status write (`0x168`) faults
  (`BIF_FAULT 0x0f`).
- **2026-11-09**: `FRAMEPROF` and TA/3D `OVERLAP` added; no `0x65c=1` on the OOM
  terminate (`r65c`=0 across all 1.04M DDK rows); the OOM-done unpause-first order.

## Also removed

All vendor-binary PC citations (`pc 0x0606`, `pc 0x1765`, `pc 0x17ec`, …), session
attributions (`ta3d-state`, `uapi-stream`, `mesa-gallium`, `m2`), and internal
dump/tool names. DDK-behavior descriptions and every mechanism/field explanation
were kept; `ddk-ref` / `ta3d+0xNN` structure references were retained, as they name
DDK layout, not bring-up provenance.
