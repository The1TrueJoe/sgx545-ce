// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

#include <drm/drm_debugfs.h>
#include <drm/drm_file.h>
#include <drm/drm_print.h>

#include <linux/debugfs.h>
#include <linux/seq_buf.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include "pvrsgx_debugfs.h"
#include "pvrsgx_device.h"
#include "pvrsgx_dump.h"
#include "pvrsgx_render.h"

static int status_show(struct seq_file *m, void *unused)
{
	struct drm_debugfs_entry *entry = m->private;
	struct seq_buf sb;
	char *buf = kmalloc(SZ_4K, GFP_KERNEL);

	if (!buf)
		return -ENOMEM;
	seq_buf_init(&sb, buf, SZ_4K);
	pvrsgx_dump_status(&sb, to_pvrsgx_device(entry->dev));
	seq_puts(m, seq_buf_str(&sb));
	kfree(buf);
	return 0;
}

static int render_show(struct seq_file *m, void *unused)
{
	struct drm_debugfs_entry *entry = m->private;

	pvrsgx_render_debugfs_show(m, to_pvrsgx_device(entry->dev), false);
	return 0;
}

static int render_before_show(struct seq_file *m, void *unused)
{
	struct drm_debugfs_entry *entry = m->private;

	pvrsgx_render_debugfs_show(m, to_pvrsgx_device(entry->dev), true);
	return 0;
}

static const struct drm_debugfs_info pvrsgx_debugfs_list[] = {
	{ "status", status_show, 0 },
	{ "render", render_show, 0 },
	{ "render_before", render_before_show, 0 },
};

void pvrsgx_debugfs_init(struct drm_minor *minor)
{
	drm_debugfs_add_files(minor->dev, pvrsgx_debugfs_list, ARRAY_SIZE(pvrsgx_debugfs_list));
}
