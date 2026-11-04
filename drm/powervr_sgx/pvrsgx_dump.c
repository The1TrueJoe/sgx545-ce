// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * Device state as text: debugfs "status", and the devcoredump taken when a
 * job hangs (/sys/class/devcoredump/devcd<N>/data; write to it to free it).
 */

#include <drm/drm_print.h>

#include <linux/devcoredump.h>
#include <linux/seq_buf.h>
#include <linux/vmalloc.h>

#include <uapi/drm/powervr_sgx_drm.h>

#include "pvrsgx_ccb.h"
#include "pvrsgx_device.h"
#include "pvrsgx_dump.h"
#include "pvrsgx_fw.h"
#include "pvrsgx_job.h"
#include "pvrsgx_mmu.h"
#include "pvrsgx_render.h"

/* Registers safe to read while the clocks are on. */
static const struct {
	const char *name;
	u32 reg;
} regs[] = {
	{ "CLKGATECTL", SGX_CR_CLKGATECTL },
	{ "CLKGATECTL2", SGX_CR_CLKGATECTL2 },
	{ "CLKGATESTATUS", SGX_CR_CLKGATESTATUS },
	{ "CORE_ID", SGX_CR_CORE_ID },
	{ "CORE_REVISION", SGX_CR_CORE_REVISION },
	{ "SOFT_RESET", SGX_CR_SOFT_RESET },
	{ "EVENT_STATUS", SGX_CR_EVENT_STATUS },
	{ "EVENT_STATUS2", SGX_CR_EVENT_STATUS2 },
	{ "EVENT_HOST_ENABLE", SGX_CR_EVENT_HOST_ENABLE },
	{ "EVENT_HOST_ENABLE2", SGX_CR_EVENT_HOST_ENABLE2 },
	{ "BIF_CTRL", SGX_CR_BIF_CTRL },
	{ "BIF_INT_STAT", SGX_CR_BIF_INT_STAT },
	{ "BIF_FAULT", SGX_CR_BIF_FAULT },
	{ "BIF_BANK0", SGX_CR_BIF_BANK0 },
	{ "BIF_DIR_LIST_BASE0", SGX_CR_BIF_DIR_LIST_BASE(0) },
	{ "BIF_DIR_LIST_BASE15", SGX_CR_BIF_DIR_LIST_BASE(15) },
	{ "EDM_TASK_REG0", 0xb14 },
	{ "EDM_TASK_REG1", 0xb20 },
};

void pvrsgx_dump_status(struct seq_buf *sb, struct pvrsgx_device *sdev)
{
	u32 *ctl = sdev->fw.cpu[PVRSGX_FW_CCBCTL];

	seq_buf_printf(sb, "firmware running: %d lost: %d resets: %u irqs: %d\n", sdev->fw_running,
		       sdev->lost, sdev->reset_count, atomic_read(&sdev->irq_count));
	seq_buf_printf(sb, "kernel PD 0x%08x idle PD 0x%08x\n", pvrsgx_mmu_pd_addr(sdev->kernel_mmu),
		       pvrsgx_mmu_pd_addr(sdev->idle_mmu));
	seq_buf_printf(sb, "ccb wo %u ro %u issued %llu consumed %llu\n", READ_ONCE(ctl[0]),
		       READ_ONCE(ctl[1]), sdev->ccb_issued, pvrsgx_ccb_consumed(sdev));
	seq_buf_printf(sb, "hostctl init 0x%x power 0x%x cleanup 0x%x uk-lockups %u int 0x%x/0x%x assert 0x%x\n",
		       pvrsgx_fw_hostctl(sdev, HOSTCTL_INIT_STATUS),
		       pvrsgx_fw_hostctl(sdev, HOSTCTL_POWER_STATUS),
		       pvrsgx_fw_hostctl(sdev, HOSTCTL_CLEANUP_STATUS),
		       pvrsgx_fw_hostctl(sdev, HOSTCTL_UK_LOCKUPS),
		       pvrsgx_fw_hostctl(sdev, HOSTCTL_INTERRUPT_FLAGS),
		       pvrsgx_fw_hostctl(sdev, HOSTCTL_INTERRUPT_CLEAR),
		       pvrsgx_fw_hostctl(sdev, HOSTCTL_ASSERT_FAIL));
	seq_buf_printf(sb, "powered: %d\n", READ_ONCE(sdev->powered));
	/* After a failed start the clocks are still on; after a stop they are not. */
	if (!READ_ONCE(sdev->powered))
		return;
	for (int i = 0; i < ARRAY_SIZE(regs); i++)
		seq_buf_printf(sb, "%-20s 0x%04x 0x%08x\n", regs[i].name, regs[i].reg,
			       sgx_rd(sdev, regs[i].reg));
}

#define COREDUMP_BYTES	SZ_128K

/*
 * Called from the timeout handler with the scheduler stopped, before the
 * restart: what the microkernel and the hung job left behind.
 */
void pvrsgx_coredump(struct pvrsgx_device *sdev, struct pvrsgx_job *bad)
{
	static const char *const types[] = {
		[DRM_POWERVR_SGX_JOB_TYPE_RENDER] = "render",
		[DRM_POWERVR_SGX_JOB_TYPE_TRANSFER] = "transfer",
		[DRM_POWERVR_SGX_JOB_TYPE_NULL] = "null",
	};
	struct seq_buf sb;
	char *buf;

	buf = __vmalloc(COREDUMP_BYTES, GFP_KERNEL | __GFP_NOWARN | __GFP_NORETRY);
	if (!buf)
		return;
	seq_buf_init(&sb, buf, COREDUMP_BYTES);
	seq_buf_printf(&sb, "powervr_sgx hang: %s job, ccb %llu, reset %u\n",
		       bad->type < ARRAY_SIZE(types) ? types[bad->type] : "?", bad->ccb_seq,
		       sdev->reset_count + 1);
	pvrsgx_dump_status(&sb, sdev);
	pvrsgx_fw_dump(&sb, sdev, PVRSGX_FW_HOSTCTL, "hostctl");
	pvrsgx_fw_dump(&sb, sdev, PVRSGX_FW_TA3DCTL, "ta3dctl");
	pvrsgx_fw_dump(&sb, sdev, PVRSGX_FW_CCBCTL, "ccbctl");
	pvrsgx_fw_dump(&sb, sdev, PVRSGX_FW_CCB, "ccb");
	pvrsgx_job_dump(&sb, bad);
	if (seq_buf_has_overflowed(&sb))
		drm_warn(&sdev->base, "coredump truncated\n");
	dev_coredumpv(sdev->base.dev, buf, seq_buf_used(&sb), GFP_KERNEL);
}
