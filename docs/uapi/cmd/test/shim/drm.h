/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */

/* Host build of the command checker: what the uAPI header takes from drm.h. */
#ifndef SHIM_DRM_H
#define SHIM_DRM_H
#include <linux/types.h>
#define DRM_COMMAND_BASE 0x40
#define DRM_IOW(nr, type) (nr)
#define DRM_IOWR(nr, type) (nr)
#endif
