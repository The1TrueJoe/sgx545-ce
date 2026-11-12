// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * Host-kick: driving the TA and 3D cores from the CPU, with no EDM task.
 *
 * On a single-context core (the SGX530) the EDM microkernel task can't retire
 * writes (stage 1, gate 1), so it can't orchestrate a render. But a render's
 * vertex and pixel tasks are launched by the TA/VDM and ISP data masters, not
 * by the EDM, so the pipelines run without it. This file moves the EDM's
 * orchestration — register programming, event waits, sync-object writes,
 * teardown — onto the CPU, which can make those writes normally.
 *
 * The sequences come from docs/porting/cores/sgx530-host-kick.md (built from
 * UKERNEL.md §3/§5/§6/§7 and STATE.md). The host-side control registers
 * (BIF, SOFT_RESET, EVENT_*, the code bases) are in the GPL header and are
 * confirmed (C): the host already writes them in the init path. The per-pass
 * TA/3D/DPM/ISP register offsets are gap 13 — the 545 offsets are the
 * hypothesis, confirmed on the SGX530 one at a time (sgx530-gap13-registers.md)
 * before a sequence is enabled. Until a sequence's offsets are confirmed, the
 * entry points return -EOPNOTSUPP rather than program unconfirmed offsets, so
 * nothing runs on hardware that could wedge the GPU.
 */

#include <drm/drm_print.h>

#include "pvrsgx_device.h"
#include "pvrsgx_host.h"

/*
 * The per-pass TA/3D/DPM/ISP register offsets this path needs are not in
 * either core's GPL header (gap 13). None is confirmed on the SGX530 yet, so
 * the render and transfer sequences below are not enabled. Each is confirmed
 * on the board the stage-1 way (the 545 sequence as the template), and this
 * flag flips per sequence as its offsets land.
 */
#define PVRSGX_HOST_GAP13_CONFIRMED	0

/*
 * HOST-mode init (sgx530-host-kick.md §8 step 1): the reset and init script
 * part 2, without the event program and without kicking or waiting on the
 * microkernel's init status (a task write that can't commit). The register
 * writes are the host-side control block, all confirmed (C). The driver
 * considers the GPU initialised once they're done.
 *
 * This reuses pvrsgx_fw_start's reset and init_script_part2; the HOST branch
 * there skips the event-program build, the kick and the init-status poll. So
 * this entry stays a thin marker until that branch is split out; for now a
 * HOST-mode device initialises through the fw path's HOST branch.
 */
int pvrsgx_host_init(struct pvrsgx_device *sdev)
{
	/*
	 * The init sequence itself lives in pvrsgx_fw.c (the reset and the init
	 * script are shared with the UKERNEL path); its HOST branch skips the
	 * task. Nothing core-specific is needed here yet.
	 */
	return 0;
}

/*
 * A render, driven from the CPU: for each TA kick program the TA cores and
 * kick the VDM (§3), poll TA_FINISHED (§5); then program the 3D/ISP/PBE cores
 * and kick the 3D (§4), poll PIXELBE_END_RENDER and DPM_3D_MEM_FREE (§5);
 * then write the HW device sync list and signal the fence (§5, CPU DRAM).
 *
 * The TA and 3D register sequences depend on the gap-13 offsets. Until they're
 * confirmed on the SGX530, this refuses rather than program unconfirmed
 * registers.
 */
int pvrsgx_host_render(struct pvrsgx_device *sdev, struct pvrsgx_job *job)
{
	if (!PVRSGX_HOST_GAP13_CONFIRMED) {
		drm_warn_once(&sdev->base,
			      "host-kick render: SGX530 offsets unconfirmed (gap 13)\n");
		return -EOPNOTSUPP;
	}

	/*
	 * TODO(gap 13), per sgx530-host-kick.md §3-§5, once the offsets are
	 * confirmed. For each TA kick: point the BIF at the context (§3.1, C),
	 * reset the TA (§3.2, C: SOFT_RESET TA = 0x8 on the 530), honour the
	 * kick's cache bits (§3.3), the DPM clear (§3.4), the TA free-list load
	 * on the first PB kick (§3.5; C: STATUS2 b0 exists), the layout
	 * registers (§3.6), the code bases (§3.7, C: USE_CODE_BASE >>8) and kick
	 * the VDM (§3.7); poll TA_FINISHED (§5). Then the 3D: the 3D free-list
	 * load on the first PB kick (§4.2; C: STATUS2 b1), save the TA's
	 * counters (§4.3), arm the PB release (§4.4), the ISP/PBE registers
	 * (§4.5), write the background block (§4.6, C: CPU DRAM) and kick the 3D
	 * (§4.7); poll PIXELBE_END_RENDER and DPM_3D_MEM_FREE (§5). Then
	 * completion (§5, all CPU DRAM): write the HW device sync list
	 * (ui32WriteOpsComplete += 1) and tear the render down. The caller
	 * signals the job's fence.
	 */
	return -EOPNOTSUPP;
}

/* A transfer (a 3D-only pass), driven from the CPU (§4, the 3D half). */
int pvrsgx_host_transfer(struct pvrsgx_device *sdev, struct pvrsgx_job *job)
{
	if (!PVRSGX_HOST_GAP13_CONFIRMED) {
		drm_warn_once(&sdev->base,
			      "host-kick transfer: SGX530 offsets unconfirmed (gap 13)\n");
		return -EOPNOTSUPP;
	}

	/*
	 * TODO(gap 13): the 3D-start sequence of §4 with the transfer command's
	 * ISP/PBE words (UKERNEL.md §3 "Transfer"), then poll and complete as in
	 * pvrsgx_host_render().
	 */
	return -EOPNOTSUPP;
}
