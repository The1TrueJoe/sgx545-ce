/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_KOBJ_H
#define PVRSGX_KOBJ_H

#include <drm/drm_mm.h>

#include <linux/list.h>
#include <linux/types.h>

#include "pvrsgx_device.h"

struct pvrsgx_vm;

/**
 * struct pvrsgx_kobj - A kernel-owned object the firmware or the TA/3D reads
 *
 * Its address is unique across the device: the firmware's directory maps every
 * one, and a VM maps the ones the TA and 3D use under its directory. A client
 * can never map anything in these ranges itself.
 */
struct pvrsgx_kobj {
	struct pvrsgx_kmem km;
	struct drm_mm_node node;
	enum pvrsgx_krange range;
	/** @vm: Also mapped in this VM's directory, or NULL. */
	struct pvrsgx_vm *vm;
	struct list_head grave;
	u64 grave_seq;
};

static inline u32 *kobj_cpu(struct pvrsgx_kobj *o)
{
	return o->km.cpu;
}

static inline u32 kobj_va(struct pvrsgx_kobj *o)
{
	return o->km.dev_addr;
}

int pvrsgx_kobj_init(struct pvrsgx_device *sdev);
void pvrsgx_kobj_fini(struct pvrsgx_device *sdev);
struct pvrsgx_kobj *pvrsgx_kobj_create(struct pvrsgx_device *sdev, enum pvrsgx_krange range,
				       u32 size, u32 min_addr, struct pvrsgx_vm *vm);
void pvrsgx_kobj_release(struct pvrsgx_device *sdev, struct pvrsgx_kobj *o);
void pvrsgx_kobj_reap(struct pvrsgx_device *sdev, u64 renders_done);

#endif /* PVRSGX_KOBJ_H */
