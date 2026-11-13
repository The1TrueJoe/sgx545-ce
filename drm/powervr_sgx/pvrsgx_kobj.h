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
 * Its address is unique across the device and the firmware's directory maps it,
 * and a VM also maps the ones the TA and 3D use under its directory. The 3D
 * parameters range is the exception: per VM, from the heap base, mapped only in
 * that VM's directory. A client can never map anything in these ranges
 * itself.
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
	/** @transfer: A transfer context's: it waits for a transfer, not a render. */
	bool transfer;
	/** @grave: In pvrsgx_device.graveyard once released. */
	struct list_head grave;
	/** @grave_seq: Renders (or transfers) submitted when it was released. */
	u64 grave_seq;
	/** @flush_seq: Once unmapped, the kernel CCB command whose cache control frees it. */
	u64 flush_seq;
	/** @grave_restarts: pvrsgx_device.restarts when it was released. */
	u32 grave_restarts;
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
void pvrsgx_kobj_free_unused(struct pvrsgx_device *sdev, struct pvrsgx_kobj **o);
void pvrsgx_kobj_reap(struct pvrsgx_device *sdev, u64 renders_done, u64 transfers_done);

/* Objects and pages waiting in the graveyard, and unmapped ones waiting for the invalidate. */
struct pvrsgx_kobj_counts {
	u32 graveyard, unmapped;
	u64 graveyard_pages, unmapped_pages;
};

bool pvrsgx_kobj_pending(struct pvrsgx_device *sdev, struct pvrsgx_kobj_counts *c);

struct seq_buf;
struct pvrsgx_vm;

int pvrsgx_kobj_new(struct pvrsgx_device *sdev, struct pvrsgx_kobj **dst,
		    enum pvrsgx_krange range, u32 size, u32 min, struct pvrsgx_vm *vm);
void pvrsgx_kobj_drop(struct pvrsgx_device *sdev, struct pvrsgx_kobj **o);
void pvrsgx_kobj_drop_sent(struct pvrsgx_device *sdev, struct pvrsgx_kobj **o, bool sent);
void pvrsgx_kobj_dump(struct seq_buf *sb, const char *name, struct pvrsgx_kobj *o, u32 off,
		      u32 bytes);

#endif /* PVRSGX_KOBJ_H */
