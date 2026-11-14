// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

#include <drm/drm_debugfs.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_print.h>

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/ktime.h>
#include <linux/pm_runtime.h>
#include <linux/seq_buf.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include "pvrsgx_debugfs.h"
#include "pvrsgx_device.h"
#include "pvrsgx_dump.h"
#include "pvrsgx_fw.h"
#include "pvrsgx_regs.h"

/*
 * The registers only while the device is still bound and already awake:
 * reading never resumes the GPU, and a suspend can't start under the read.
 */
static int status_show(struct seq_file *m, void *unused)
{
	struct drm_debugfs_entry *entry = m->private;
	struct pvrsgx_device *sdev = to_pvrsgx_device(entry->dev);
	struct seq_buf sb;
	char *buf = kmalloc(SZ_4K, GFP_KERNEL);
	int idx, awake;

	if (!buf)
		return -ENOMEM;
	if (!drm_dev_enter(entry->dev, &idx)) {
		kfree(buf);
		return -ENODEV;
	}
	/* -EINVAL: runtime PM is off and the device always on. */
	awake = pm_runtime_get_if_active(sdev->base.dev);
	seq_buf_init(&sb, buf, SZ_4K);
	pvrsgx_dump_status(&sb, sdev, awake > 0 || awake == -EINVAL);
	if (awake > 0)
		pm_runtime_put(sdev->base.dev);
	drm_dev_exit(idx);
	seq_puts(m, seq_buf_str(&sb));
	kfree(buf);
	return 0;
}

/* The microkernel's control blocks, raw, with a timestamp: memory only, so safe to poll. */
static int ctlblk_show(struct seq_file *m, void *unused)
{
	struct drm_debugfs_entry *entry = m->private;
	struct pvrsgx_device *sdev = to_pvrsgx_device(entry->dev);
	struct seq_buf sb;
	char *buf = kmalloc(SZ_2K, GFP_KERNEL);

	if (!buf)
		return -ENOMEM;
	seq_buf_init(&sb, buf, SZ_2K);
	seq_buf_printf(&sb, "t %lld gen %u\n", ktime_get_ns(), READ_ONCE(sdev->fw_gen));
	pvrsgx_fw_dump(&sb, sdev, PVRSGX_FW_TA3DCTL, "ta3dctl");
	pvrsgx_fw_dump(&sb, sdev, PVRSGX_FW_HOSTCTL, "hostctl");
	seq_puts(m, seq_buf_str(&sb));
	kfree(buf);
	return 0;
}

/*
 * Bring-up probe (off unless inval_probe=1): which event bits a cache
 * invalidate raises. With the GPU awake and idle, each named PDS invalidate
 * register in turn is written 1, then EVENT_STATUS and EVENT_STATUS2 are read
 * before, after up to 1 ms, and after clearing whatever rose. Named registers
 * only. Reading the file runs the probe.
 */
static bool pvrsgx_inval_probe;
module_param_named(inval_probe, pvrsgx_inval_probe, bool, 0444);
MODULE_PARM_DESC(inval_probe, "Bring-up: debugfs inval_probe writes the PDS invalidates (default off)");

static int inval_probe_show(struct seq_file *m, void *unused)
{
	struct drm_debugfs_entry *entry = m->private;
	struct pvrsgx_device *sdev = to_pvrsgx_device(entry->dev);
	const struct pvrsgx_core_regs *cr = sdev->cr;
	const struct {
		const char *name;
		u32 reg;
	} inv[] = {
		{ "PDS_INV0", SGX_CR_PDS_INV0 },
		{ "PDS_INV1", SGX_CR_PDS_INV1 },
		{ "PDS_INV2", cr->pds_inv2 },
		{ "PDS_INV3", cr->pds_inv3 },
		{ "PDS_INV_CSC", SGX_CR_PDS_INV_CSC },
	};
	int idx, ret;

	if (!pvrsgx_inval_probe)
		return -EPERM;
	if (!drm_dev_enter(entry->dev, &idx))
		return -ENODEV;
	ret = pm_runtime_resume_and_get(sdev->base.dev);
	if (ret < 0) {
		drm_dev_exit(idx);
		return ret;
	}
	seq_printf(m, "start: CLKGATESTATUS 0x%08x EVENT_STATUS 0x%08x EVENT_STATUS2 0x%08x\n",
		   sgx_rd(sdev, cr->clkgatestatus), sgx_rd(sdev, SGX_CR_EVENT_STATUS),
		   sgx_rd(sdev, SGX_CR_EVENT_STATUS2));
	for (int i = 0; i < ARRAY_SIZE(inv); i++) {
		u32 s0, s20, s1 = 0, s21 = 0, us;

		if (!inv[i].reg)
			continue;
		s0 = sgx_rd(sdev, SGX_CR_EVENT_STATUS);
		s20 = sgx_rd(sdev, SGX_CR_EVENT_STATUS2);
		sgx_wr(sdev, inv[i].reg, 1);
		for (us = 0; us < 1000; us += 10) {
			s1 = sgx_rd(sdev, SGX_CR_EVENT_STATUS);
			s21 = sgx_rd(sdev, SGX_CR_EVENT_STATUS2);
			if ((s1 & ~s0) || (s21 & ~s20))
				break;
			udelay(10);
		}
		seq_printf(m, "%-11s 0x%04x: STATUS 0x%08x -> 0x%08x  STATUS2 0x%08x -> 0x%08x  after %u us",
			   inv[i].name, inv[i].reg, s0, s1, s20, s21, us);
		if (s1 & ~s0)
			sgx_wr(sdev, SGX_CR_EVENT_HOST_CLEAR, s1 & ~s0);
		if (s21 & ~s20)
			sgx_wr(sdev, SGX_CR_EVENT_HOST_CLEAR2, s21 & ~s20);
		seq_printf(m, "  cleared: 0x%08x 0x%08x\n", sgx_rd(sdev, SGX_CR_EVENT_STATUS),
			   sgx_rd(sdev, SGX_CR_EVENT_STATUS2));
	}
	pm_runtime_put(sdev->base.dev);
	drm_dev_exit(idx);
	return 0;
}

static const struct drm_debugfs_info pvrsgx_debugfs_list[] = {
	{ "status", status_show, 0 },
	{ "ctlblk", ctlblk_show, 0 },
	{ "inval_probe", inval_probe_show, 0 },
};

void pvrsgx_debugfs_init(struct drm_minor *minor)
{
	drm_debugfs_add_files(minor->dev, pvrsgx_debugfs_list, ARRAY_SIZE(pvrsgx_debugfs_list));
}
