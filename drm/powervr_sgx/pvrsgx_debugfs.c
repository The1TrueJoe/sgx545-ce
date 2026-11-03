// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

#include <drm/drm_debugfs.h>
#include <drm/drm_file.h>
#include <drm/drm_print.h>

#include <linux/debugfs.h>
#include <linux/seq_file.h>

#include "pvrsgx_ccb.h"
#include "pvrsgx_debugfs.h"
#include "pvrsgx_device.h"
#include "pvrsgx_fw.h"
#include "pvrsgx_mmu.h"
#include "pvrsgx_render.h"

/* Registers safe to read once the firmware is running. */
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

static int status_show(struct seq_file *m, void *unused)
{
	struct drm_debugfs_entry *entry = m->private;
	struct pvrsgx_device *sdev = to_pvrsgx_device(entry->dev);
	u32 *ctl = sdev->fw.cpu[PVRSGX_FW_CCBCTL];

	seq_printf(m, "firmware running: %d lost: %d resets: %u irqs: %d\n", sdev->fw_running,
		   sdev->lost, sdev->reset_count, atomic_read(&sdev->irq_count));
	seq_printf(m, "kernel PD 0x%08x idle PD 0x%08x\n", pvrsgx_mmu_pd_addr(sdev->kernel_mmu),
		   pvrsgx_mmu_pd_addr(sdev->idle_mmu));
	seq_printf(m, "ccb wo %u ro %u issued %llu consumed %llu\n", READ_ONCE(ctl[0]),
		   READ_ONCE(ctl[1]), sdev->ccb_issued, pvrsgx_ccb_consumed(sdev));
	seq_printf(m, "hostctl init 0x%x power 0x%x cleanup 0x%x uk-lockups %u int 0x%x/0x%x assert 0x%x\n",
		   pvrsgx_fw_hostctl(sdev, HOSTCTL_INIT_STATUS),
		   pvrsgx_fw_hostctl(sdev, HOSTCTL_POWER_STATUS),
		   pvrsgx_fw_hostctl(sdev, HOSTCTL_CLEANUP_STATUS),
		   pvrsgx_fw_hostctl(sdev, HOSTCTL_UK_LOCKUPS),
		   pvrsgx_fw_hostctl(sdev, HOSTCTL_INTERRUPT_FLAGS),
		   pvrsgx_fw_hostctl(sdev, HOSTCTL_INTERRUPT_CLEAR),
		   pvrsgx_fw_hostctl(sdev, HOSTCTL_ASSERT_FAIL));
	if (!sdev->fw_running)
		return 0;
	for (int i = 0; i < ARRAY_SIZE(regs); i++)
		seq_printf(m, "%-20s 0x%04x 0x%08x\n", regs[i].name, regs[i].reg,
			   sgx_rd(sdev, regs[i].reg));
	return 0;
}

static int render_show(struct seq_file *m, void *unused)
{
	struct drm_debugfs_entry *entry = m->private;

	pvrsgx_render_debugfs_show(m, to_pvrsgx_device(entry->dev));
	return 0;
}

static const struct drm_debugfs_info pvrsgx_debugfs_list[] = {
	{ "status", status_show, 0 },
	{ "render", render_show, 0 },
};

void pvrsgx_debugfs_init(struct drm_minor *minor)
{
	drm_debugfs_add_files(minor->dev, pvrsgx_debugfs_list, ARRAY_SIZE(pvrsgx_debugfs_list));
}
