# SGX530 board kick: submit accepted, render produces nothing (545-shaped PDS on 530)

**2026-11-13 ~08:25 UTC. Our OMAP3630 test board, kernel 7.3.3. Under lock. Clean, recovered.**

Re-run after the sgx530 command-def fix (727486ce), both microkernel images, 64x64 render.

## The cmd-def fix works; the render does not

The -95 is gone — `SUBMIT_JOBS` is accepted and the job runs. But both images fail the
render identically:

```
# stock (fa361aad) and edm (47e4282a), same result:
EDM init: event status 0x0, BIF int 0x0, timer 0x0
microkernel started
*ERROR* job timed out (ccb 3, consumed 2); resetting
microkernel started again           # HWR re-init
frame 0: render FAILED, centre 00000000 (want ff00ff00), corner 00000000 (want ff000080)
```

- `consumed 2`: the microkernel read **both** TA kicks (CCB read offset advanced), i.e. the
  host-kick dispatch and do_ta/do_3d ran. But no completion event arrived → job watchdog
  timeout → hardware recovery.
- Pixels all zero (out.bgra, 16 KB of 0x00).

## Stock == edm ⇒ not the completion path, it's the render

stock runs the full WAIT_3D/WAIT_FREE rendezvous in pixel_done; edm reduces completion to one
SW_EVENT. They fail **identically** (same timeout, same consumed count, same black frame), so
the failure is upstream of completion: the TA/3D render produces no output. The microkernel
consumes the kicks and programs the cores, but nothing renders.

## Cause: the 545-shaped PDS/USSE programs don't execute on the 530

The frame client is sgxgen built for the 545 (no 530 core option). The 530 lacks
PDS_DATA_INTERLEAVE_2DWORDS and lays DS0/DS1 out in 8-word blocks (PDS.md §"vendor-setup" §2;
the event DOUTU takes 3 rows on the 530 vs 1 on the 545). So a PDS program assembled for the
545's 2-dword interleave has its DS1 source words in the wrong memory slots on the 530 — the
vertex/pixel task launch reads zero/garbage, the TA emits no tiles, the 3D renders nothing.
This is the PDS-interleave risk flagged before the run; the command-def fix made it reachable,
and the board confirms it.

**Next blocker: a 530 PDS codegen path in sgxgen** (the 8-word DS0/DS1 interleave), from
PDS.md. That is the coordinator's original "530-correct frame" concern, now with board evidence
that the command layer is fine and the PDS layer is where the 545 shape breaks.

## Not yet captured (to localize the PDS failure)

The region headers / tail pointers (the "did the TA emit tiles" signal) weren't captured:
powervr_sgx_frame.c uses its own flush, not sgxgen's sgx_flush, so SGXGEN_DUMP_AFTER didn't
fire; and m2-drm has no render/render_before debugfs. A frame-test tweak to read back the
mapped RT structures (or a render debugfs) would say whether the vertex PDS ran (region
headers written) or not (all zero) — narrowing the 530 PDS work to vertex vs pixel.

## Board

Clean throughout; HWR recovered the GPU after each timeout, driver unloaded, lock released,
left on its normal boot. No power cycle needed.
