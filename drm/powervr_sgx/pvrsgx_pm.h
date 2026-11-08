/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_PM_H
#define PVRSGX_PM_H

#include <linux/kconfig.h>
#include <linux/pm.h>
#include <linux/types.h>

struct pvrsgx_device;

/* Referenced through pm_ptr(), so declared either way. */
extern const struct dev_pm_ops pvrsgx_pm_ops;

#if IS_ENABLED(CONFIG_PM)

int pvrsgx_power_get(struct pvrsgx_device *sdev);
void pvrsgx_power_put(struct pvrsgx_device *sdev, u32 n);
void pvrsgx_power_init(struct pvrsgx_device *sdev);
void pvrsgx_power_fini(struct pvrsgx_device *sdev);
#else
/* Without runtime PM the GPU stays on from probe to remove. */
static inline int pvrsgx_power_get(struct pvrsgx_device *sdev) { return 0; }
static inline void pvrsgx_power_put(struct pvrsgx_device *sdev, u32 n) { }
static inline void pvrsgx_power_init(struct pvrsgx_device *sdev) { }
static inline void pvrsgx_power_fini(struct pvrsgx_device *sdev) { }
#endif

#endif /* PVRSGX_PM_H */
