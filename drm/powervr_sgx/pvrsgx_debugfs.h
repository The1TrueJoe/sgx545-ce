/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_DEBUGFS_H
#define PVRSGX_DEBUGFS_H

struct drm_minor;

#if defined(CONFIG_DEBUG_FS)
void pvrsgx_debugfs_init(struct drm_minor *minor);
#endif

#endif /* PVRSGX_DEBUGFS_H */
