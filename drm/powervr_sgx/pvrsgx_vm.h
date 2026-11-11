/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_VM_H
#define PVRSGX_VM_H

#include <drm/drm_gpuvm.h>
#include <drm/drm_mm.h>

#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/types.h>

struct drm_device;
struct drm_file;
struct pvrsgx_device;
struct pvrsgx_file;
struct pvrsgx_pb;

/**
 * struct pvrsgx_vm - A VM context: one page directory
 *
 * Maps only the client heaps, plus (later) the kernel-owned structures the TA
 * and 3D read under it. Never the kernel code or data heaps: that is what keeps
 * a client's tasks away from the firmware's memory.
 */
struct pvrsgx_vm {
	/** @gpuvm: The GPU VA manager. */
	struct drm_gpuvm gpuvm;
	/** @sdev: The device. */
	struct pvrsgx_device *sdev;
	/** @mmu: Its page directory and tables. */
	struct pvrsgx_mmu *mmu;
	/** @params: 3D parameters addresses of this VM's kernel objects (pvrsgx_kobj.c). */
	struct drm_mm params;
	/** @pb: The device's parameter buffer, mapped here by the first render context. */
	struct pvrsgx_pb *pb;
	/** @pb_nodes: @pb's PB and free list in @params. */
	struct drm_mm_node pb_nodes[2];
	/** @lock: Serializes map and unmap, and @pb's mapping. */
	struct mutex lock;
	/** @ref: Held by the handle, its contexts and their jobs. */
	struct kref ref;
	/** @users: The handle and the live contexts; at zero its mappings go. */
	atomic_t users;
};

struct pvrsgx_vm *pvrsgx_vm_create(struct pvrsgx_device *sdev);
struct pvrsgx_vm *pvrsgx_vm_get(struct pvrsgx_vm *vm);
void pvrsgx_vm_put(struct pvrsgx_vm *vm);
void pvrsgx_vm_user_get(struct pvrsgx_vm *vm);
void pvrsgx_vm_user_put(struct pvrsgx_vm *vm);
struct dma_resv *pvrsgx_vm_resv(struct pvrsgx_vm *vm);
struct pvrsgx_vm *pvrsgx_vm_lookup(struct pvrsgx_file *sfile, u32 handle);
void pvrsgx_vm_destroy_all(struct pvrsgx_file *sfile);
int pvrsgx_heap_find(u64 addr, u64 size);

int pvrsgx_ioctl_create_vm_context(struct drm_device *drm, void *data, struct drm_file *file);
int pvrsgx_ioctl_destroy_vm_context(struct drm_device *drm, void *data, struct drm_file *file);
int pvrsgx_ioctl_vm_map(struct drm_device *drm, void *data, struct drm_file *file);
int pvrsgx_ioctl_vm_unmap(struct drm_device *drm, void *data, struct drm_file *file);
int pvrsgx_query_heap_info(struct pvrsgx_device *sdev, void __user *ptr, u32 *size);

#endif /* PVRSGX_VM_H */
