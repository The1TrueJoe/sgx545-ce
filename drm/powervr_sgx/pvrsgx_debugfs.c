// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

#include <drm/drm_debugfs.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_print.h>

#include <linux/debugfs.h>
#include <linux/ktime.h>
#include <linux/pm_runtime.h>
#include <linux/seq_buf.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include "pvrsgx_debugfs.h"
#include "pvrsgx_device.h"
#include "pvrsgx_dump.h"
#include "pvrsgx_fw.h"

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

static const struct drm_debugfs_info pvrsgx_debugfs_list[] = {
	{ "status", status_show, 0 },
	{ "ctlblk", ctlblk_show, 0 },
};

void pvrsgx_debugfs_init(struct drm_minor *minor)
{
	drm_debugfs_add_files(minor->dev, pvrsgx_debugfs_list, ARRAY_SIZE(pvrsgx_debugfs_list));
}
