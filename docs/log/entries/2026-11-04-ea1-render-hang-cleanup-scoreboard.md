# EA1/EA3 consecutive-frame render hang: root cause (process-exit cleanup) + HW scoreboard

The open SGX545 microkernel renders the probe triangle, but across the 10 separate
frame-dump processes only ~half the frames complete; the rest BIF-fault and HWR. This
entry records the root cause as now understood, every HW variant tried and its rate, and
the one open sub-problem, so the fix can start from facts.

## Root cause: we complete the process-exit cleanup too early

The 10 frame-dumps are separate PROCESSES, each a fresh surface. On exit, RESMAN sends
cleanup commands (render context, PB, ...). The DDK's cleanup handler DEFERS while that
context's render is still in flight (ta3d-state, DDK pc 0x039d: defer while
ctx+0x20|ctx+0x28|ctx+0x14 != 0; PB cleanup pc 0x0475 clears its loaded-PB markers). Ours
set ui32CleanupStatus COMPLETE immediately. So the host freed process N's PB/RT while the
3D and its DPM page-free were still using them, and process N+1's do_ta free-list reload
hit the freed pages → BIF fault.

Confirmed by UK_TASKLOG (each handler stamps its id into the control block at entry): at
every HWR the running task was **do_ta (id 3)**, previous **kick (id 2)**, with BANK0 =
0x101F — i.e. the next process's kick-1 free-list load running while the prior render's
3D/DPM-free was still outstanding. The fault address varies run to run (0x4031F000,
0x40142000, 0xC011F000, 0x000B2000, ...); BIF_FAULT only stores ADDR[31:12], so these are
a garbage runtime pointer into freed memory, not a fixed buffer (ta3d-state).

Related facts established:
- No missing trigger: the DPM 3D free is AUTOMATIC. The DDK 3D-end handler only acks
  0x40000 and reads 0x728; the 3D free-list LOAD (0x664=5/0x680, when the 3D's PB changed)
  arms it, which ours already does.
- The HWR dump's `dpm_3d_free: no` is EVENT_STATUS2 bit 1 (free-LIST-LOAD done), NOT the
  DPM 3D memory-free event (EVENT_STATUS bit 0). It does not show the free failing.
- The DDK completes the render on the DPM-3D-memory-free event (EVENT_STATUS bit 0), after
  pixel-done; ours completes at pixel-done (0x40000). The cleanup deferral is what makes
  the DDK's "skip the reload when the PB is unchanged" safe across processes.

## HW scoreboard (EA1 cold-GPU S50 rig, same frame-dump, srvinit -o)

| variant | rate | notes |
|---|---|---|
| baseline (frame-1 EVENT_TIMER fix 9e48757, reload every frame, complete at 0x40000) | 5/10 | committed |
| sync fix (3622455: render_done clears deps[0] sync1 + do_ta rd[0xE0]=cmd[0x3c] copy) | ~5/10 | DDK-correct (run_ours 12/12), committed; does not touch the hang |
| DPM-completion (render_done moved to the DPM-free event 0x1) | 2/10 | render_done not reached → timeouts; the 0x1-completion dispatch fault is OPEN |
| PB-gate (skip the free-list reload when PB address unchanged) | 1/10 | wrong for separate processes (same PB addr, fresh content); the reload IS needed each frame |
| cleanup-defer step 1 (k_cleanup defers while SC_INFLIGHT != 0) | 1/10 | window too short: SC_INFLIGHT clears at pixel-done, before the page release |
| cleanup + page-release (defer while SC_INFLIGHT or PAGE_RELEASE_PENDING; flag set at pixel-done, cleared on the DPM-free event 0x1) | 1/10 | DEADLOCKED — only 1 HWR, 9 TIMEOUTS (see below); the cleanup stuck on exit |

Data: docs/log/data/2026-11-03-*.log (per-variant captures incl. the (P0) BIF + T3C-E0
task markers).

## The page-release variant DEADLOCKS on exit (do not ship as written)

Result 1/10 but, tellingly, only 1 HWR and 9 TIMEOUTS (not faults): the cleanup stuck on
process exit. ta3d-state's diagnosis: when 0x40000 (pixels done) and 0x1 (3D memory free)
land in the SAME event, the event-entry up-front ack clears BOTH bits, but dispatch runs
render_done first (0x40000) which sets PAGE_RELEASE_PENDING and ends — so the 0x1 bit is
already acked and dpm_free_done never runs. PAGE_RELEASE_PENDING is never cleared, the
cleanup defers forever, and because the 1 kHz EVENT_TIMER keeps the EDM alive there is no
watchdog HWR to re-run init and clear the flag. On the plugless EA1 a frame-dump stuck in
exit means a plain reboot may not take; recover with `sync; reboot -f` only. (The box
recovered via the test's own reboot and is clean.)

Fix for the next attempt (ta3d-state): (a) in the same dispatch, after render_done, check
the snapshot for bit 0 and clear PAGE_RELEASE_PENDING there; OR ack only the bits each
task handles so 0x1 re-raises as its own event; AND (b) bound the cleanup defer (N timer
ticks, then ack + log) so it can never stick regardless.

## Open sub-problem: the 0x1-completion dispatch fault

Moving render_done entirely onto the DPM-3D-memory-free event (0x1, the DDK way) timed out
— render_done was never reached. The event-entry mask already acks bit 0, so bit 0 does
fire; why the dispatch didn't reach render_done is UNRESOLVED. The page-release variant
sidesteps it (completion stays at pixel-done; a tiny dpm_free_done handler on 0x1 only
clears PAGE_RELEASE_PENDING), but a clean cleanup fix will want render_done on 0x1 as the
DDK does, so this needs finding.

## What's solid regardless

- Committed DDK-correct sync fix (3622455): render_done clears the real second destination
  sync via deps[0] (not the out-of-bounds rd+0x240) + the do_ta rd[0xE0]=cmd[0x3c] copy.
- A 2-frame offline oracle: frame-from-scratch's v2x/v2s/v2f traces + run_ours, which
  already caught the rd+0x2a4 RT-data counter and the RT-entry divergences. Fidelity gap:
  fromtrace's contexts()/load-ordering resolve render 2 to render-1 addresses (frame-from-
  scratch's two bugs; fix = latest UBuf match + per-kick load + freed-region poison).
- UK_TASKLOG gated diagnostic (names the faulting handler).

## Next (the real cleanup fix)

1. Defer k_cleanup until the page release is done (this entry's page-release variant, or a
   per-context in-flight check matching the DDK's ctx+0x20|0x28|0x14).
2. On context/PB cleanup, clear the loaded-PB markers (TA_PB_LOADED/D3_PB_LOADED).
3. Re-enable the "skip the reload when the PB is unchanged" gate (safe once (1)+(2) hold).
4. Move render_done to the DPM-free event (0x1), resolving the dispatch fault above.
This is M1's transfers/cleanup work item.

## Update: deadlock-safe cleanup fix (dd5adc34) = 3/10, no deadlock, hang persists

The deadlock-safe cleanup (k_cleanup defers while SC_INFLIGHT|PAGE_RELEASE_PENDING, 0x1
acked by dpm_free_done not up-front, bounded 256 ticks) on the EA1: 3/10, HWR=7, same
do_ta fault (BIF_FAULT 0x4031100F, BANK0=0x101F). NO deadlock (box completed + restored;
the bound worked). Offline it's fully validated (v2x RC cleanup defers-while-releasing +
completes; test_commands cleanup-defer/bound pass). Committed 76f3d9e + a7b4349.

WHY it doesn't fix the hang: the cleanup deferral holds process N's MEMORY (PB/RT) until
its render+page-release finishes, but the fault is the GPU-global DPM FREE-LIST race --
process N+1's do_ta free-list load runs while process N's DPM 3D free is still draining.
That is a GPU-timing race on a single DPM unit; holding memory does not serialise it. The
DDK avoids it structurally by COMPLETING THE RENDER ON THE DPM-FREE EVENT (0x1), so the
client does not see "done" (and the next process does not start its do_ta) until the free
is complete. Ours completes at pixel-done (0x40000), so the client exits early and the
next process races.

So the hang fix is render_done ON 0x1 (the DDK order), which was tried before and timed
out -- but that was BEFORE the ack fix. The deadlock-safe build proves 0x1 now dispatches
reliably (dpm_free_done runs on it: the cleanup completes), so render_done-on-0x1 should
now be reachable. Next: move render_done to the DPM-free event with the ack fix in place,
keep the cleanup deferral (memory correctness) + the bound. The cleanup fix stays (DDK-
correct behaviour) but is not itself the hang fix.

## Update: render_done-on-0x1 implemented (1cccc1d), offline-validated, ready for HW

Why this is NOT a guess this time -- the DDK is an existence proof: ta3d-state's trace has
the DDK completing each render on EVENT_STATUS bit 0 (the DPM 3D-memory-free event, 0x1),
AFTER pixel-done, and the DDK renders 10/10 on this exact EA1. So 0x1 DOES fire reliably
once per render on this HW and completion-on-0x1 works; our earlier 2/10 was our own 0x1
being lost in the up-front ack (now fixed: 0x1 is not acked up-front, it re-raises), not
0x1 failing to fire.

The change (1cccc1d):
- 0x40000 / 0x800000 now run `pixel_done`: route a transfer to xfer_done, else END with
  the render still in flight (SC_INFLIGHT set). No completion here.
- 0x1 runs `render_done`: ack bit 0, then (guarded by SC_INFLIGHT != 0, so a transfer's or
  a spurious free is a no-op) do the full completion -- sync0/sync1/counter/write-ops, the
  render/RT/context teardown, SC_INFLIGHT = 0, host IRQ. The host sees "done" only after
  the free, so the sequential next frame-dump process cannot start its do_ta into a still-
  draining DPM free.
- PAGE_RELEASE_PENDING + dpm_free_done deleted. k_cleanup now defers on SC_INFLIGHT alone
  (render_done clears it on 0x1, i.e. after the free), still bounded (CLEANUP_DEFER_MAX).

Offline (DDK firmware oracle, run_ours --ddk + test_commands):
- [done] completion-write counts match the DDK exactly (v2x 12/12, v2s 20/20, v2f/v3f
  15/15). Removing the PAGE_RELEASE store fixed the one EXTRA the baseline had
  (0x3200306c = ta3d+0x6c); remaining diffs are the known frame-from-scratch oracle gap
  (rd+0x2a4 frame counter, render-2-resolves-to-render-1 addresses), unchanged by this.
- cleanup now defers-while-in-flight + completes on all four traces (SC_INFLIGHT gate), no
  deadlock. test_commands cleanup-defer + cleanup-bound pass.

HW-pending: flash on the EA1 when m2's PRIME slot frees. Expected: the consecutive-frame
rate rises toward 10/10. If render_done is still not reached (rate stays ~2-3/10), enable
WAIT_LOG (render_done stamps stage id 4) to see whether 0x1 dispatches -- but the DDK
existence proof makes a lost-0x1 unlikely now. Awaiting ta3d-state's confirmation of the
exact DDK completion order on 0x1 before spending the slot.
