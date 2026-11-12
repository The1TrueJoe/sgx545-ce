/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_KOBJ_H
#define PVRSGX_KOBJ_H

#include <drm/drm_mm.h>

#include <linux/list.h>
#include <linux/types.h>

#include "pvrsgx_ccb.h"
#include "pvrsgx_device.h"

struct pvrsgx_vm;

/**
 * struct pvrsgx_kobj - A kernel-owned object the firmware or the TA/3D reads
 *
 * Its address is unique across the device and the firmware's directory maps it,
 * and a VM also maps the ones the TA and 3D use under its directory. The 3D
 * parameters range is the exception: per VM, from the heap base, mapped only in
 * that VM's directory, as the DDK's per-context heap (each context's PB starts
 * at 0x10000000). A client can never map anything in these ranges itself.
 */
struct pvrsgx_kobj {
	/** @km: Its memory. */
	struct pvrsgx_kmem km;
	/** @node: Its place in @mm. */
	struct drm_mm_node node;
	/** @mm: The range allocator @node came from. */
	struct drm_mm *mm;
	/** @range: The range it was allocated in. */
	enum pvrsgx_krange range;
	/** @vm: Also mapped in this VM's directory, or NULL. */
	struct pvrsgx_vm *vm;
	/** @until_restart: Once released, kept until the next microkernel start. */
	bool until_restart;
	/** @grave: In pvrsgx_device.graveyard once released. */
	struct list_head grave;
	/** @grave_seq: pvrsgx_device.renders_submitted when it was released. */
	u64 grave_seq;
	/** @inval: Once unmapped, waits for the BIF invalidate before it is freed. */
	struct pvrsgx_inval_wait inval;
	/** @sdev: The device, for the free after the invalidate. */
	struct pvrsgx_device *sdev;
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

struct seq_buf;
struct pvrsgx_vm;

int pvrsgx_kobj_new(struct pvrsgx_device *sdev, struct pvrsgx_kobj **dst,
		    enum pvrsgx_krange range, u32 size, u32 min, struct pvrsgx_vm *vm);
void pvrsgx_kobj_drop(struct pvrsgx_device *sdev, struct pvrsgx_kobj **o);
void pvrsgx_kobj_dump(struct seq_buf *sb, const char *name, struct pvrsgx_kobj *o, u32 off,
		      u32 bytes);

#endif /* PVRSGX_KOBJ_H */
