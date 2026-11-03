/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_VM_H
#define PVRSGX_VM_H

#include <drm/drm_gpuvm.h>

#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/types.h>

struct drm_device;
struct drm_file;
struct pvrsgx_device;
struct pvrsgx_file;

/**
 * struct pvrsgx_vm - A VM context: one page directory
 *
 * Maps only the client heaps, plus (later) the kernel-owned structures the TA
 * and 3D read under it. Never the kernel code or data heaps: that is what keeps
 * a client's tasks away from the firmware's memory (docs/UAPI.md 7.2, P2/P3).
 */
struct pvrsgx_vm {
	struct drm_gpuvm gpuvm;
	struct pvrsgx_device *sdev;
	struct pvrsgx_mmu *mmu;
	/** @lock: Serialises map and unmap. */
	struct mutex lock;
	struct kref ref;
};

struct pvrsgx_vm *pvrsgx_vm_get(struct pvrsgx_vm *vm);
void pvrsgx_vm_put(struct pvrsgx_vm *vm);
struct dma_resv *pvrsgx_vm_resv(struct pvrsgx_vm *vm);
struct pvrsgx_vm *pvrsgx_vm_lookup(struct pvrsgx_file *sfile, u32 handle);
void pvrsgx_vm_destroy_all(struct pvrsgx_file *sfile);
int pvrsgx_heap_find(u64 addr, u64 size);
bool pvrsgx_vm_shares_kernel_heaps(struct pvrsgx_device *sdev);

int pvrsgx_ioctl_create_vm_context(struct drm_device *drm, void *data, struct drm_file *file);
int pvrsgx_ioctl_destroy_vm_context(struct drm_device *drm, void *data, struct drm_file *file);
int pvrsgx_ioctl_vm_map(struct drm_device *drm, void *data, struct drm_file *file);
int pvrsgx_ioctl_vm_unmap(struct drm_device *drm, void *data, struct drm_file *file);
int pvrsgx_query_heap_info(struct pvrsgx_device *sdev, void __user *ptr, u32 *size);

#endif /* PVRSGX_VM_H */
