/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_DEVICE_H
#define PVRSGX_DEVICE_H

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_mm.h>
#include <drm/gpu_scheduler.h>

#include <linux/device.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include "pvrsgx_core.h"
#include "pvrsgx_hw.h"

/* Device-wide ranges for kernel-owned objects (docs/UAPI.md section 5). */
enum pvrsgx_krange {
	PVRSGX_KRANGE_KDATA,	/* firmware only */
	PVRSGX_KRANGE_KCODE,	/* firmware only */
	PVRSGX_KRANGE_SYNC,	/* firmware only */
	PVRSGX_KRANGE_3DPARAMS,	/* firmware and the owner's VM */
	PVRSGX_KRANGE_TADATA,	/* firmware and the owner's VM */
	PVRSGX_KRANGE_COUNT
};

struct firmware;
struct pvrsgx_core_info;
struct gma500_sgx_device;
struct pci_dev;
struct pvrsgx_mmu;
struct pvrsgx_pb;
struct pvrsgx_vm;

/**
 * struct pvrsgx_kmem - Kernel-owned memory at a fixed device address
 *
 * Mapped in the firmware's page directory only, never in a client's. The CPU
 * mapping is write-combined.
 */
struct pvrsgx_kmem {
	/** @mmu: Directory it is mapped in; NULL (the default) is the firmware's. */
	struct pvrsgx_mmu *mmu;
	/** @dev_addr: Device address. */
	u32 dev_addr;
	/** @size: Size in bytes, a whole number of pages. */
	u32 size;
	/** @npages: Number of pages. */
	unsigned int npages;
	/** @pages: The pages, below 4 GiB. */
	struct page **pages;
	/** @cpu: Write-combined kernel mapping. */
	void *cpu;
};

/* Firmware-visible buffers, in the allocation order docs/SRVINIT.md lists. */
enum pvrsgx_fw_buf {
	PVRSGX_FW_CCB,		/* kernel CCB: 256 commands */
	PVRSGX_FW_CCBCTL,	/* its write and read offsets */
	PVRSGX_FW_KICKER,	/* EUR_CR_EVENT_KICKER target */
	PVRSGX_FW_TA3DCTL,	/* the microkernel's control block */
	PVRSGX_FW_HOSTCTL,	/* host control block */
	PVRSGX_FW_MISC,		/* misc info the microkernel may fill */
	PVRSGX_FW_UKERNEL,	/* USSE microkernel */
	PVRSGX_FW_PDS0,		/* PDS programs that enter it */
	PVRSGX_FW_PDS1,
	PVRSGX_FW_PDS2,
	PVRSGX_FW_PDS3,
	PVRSGX_FW_PDS4,
	PVRSGX_FW_NBUF
};

/**
 * struct pvrsgx_fw - The running microkernel and what it reads
 */
struct pvrsgx_fw {
	/** @data: Kernel data heap: control blocks, CCB. */
	struct pvrsgx_kmem data;
	/** @code: Kernel code heap: microkernel and its PDS programs. */
	struct pvrsgx_kmem code;
	/** @addr: Device address of each buffer. */
	u32 addr[PVRSGX_FW_NBUF];
	/** @cpu: CPU address of each buffer. */
	void *cpu[PVRSGX_FW_NBUF];
	/** @cache_control: Invalidates to request with the next command. */
	u32 cache_control;
	/** @interface: DRM_POWERVR_SGX_FW_IF_*. */
	u32 interface;
	/** @name: Firmware file the image came from. */
	const char *name;
	/** @image: The microkernel as loaded, for restarts. */
	u8 *image;
	/** @image_size: Bytes of USSE code in @image. */
	u32 image_size;
	/** @features: PVRSGX_UKFW_* of an open microkernel; 0 for the DDK's. */
	u32 features;
	/** @entries: An open microkernel's entry table (PDS program, offset), or NULL. */
	u32 (*entries)[2];
	/** @n_entries: Entries in @entries. */
	u32 n_entries;
	/** @patches: An open microkernel's patch table (offset, value), or NULL. */
	u32 (*patches)[2];
	/** @n_patches: Entries in @patches. */
	u32 n_patches;
};

/**
 * struct pvrsgx_device - The SGX545
 */
struct pvrsgx_device {
	/** @base: The DRM device. */
	struct drm_device base;
	/** @pdev: The SGX PCI function (CE5300), or NULL behind gma500 (Cedarview). */
	struct pci_dev *pdev;
	/** @dma_parms: DMA parameters of the gma500 auxiliary device. */
	struct device_dma_parameters dma_parms;
	/** @gma_sgx: The gma500 auxiliary device behind which the SGX sits, or NULL. */
	struct gma500_sgx_device *gma_sgx;
	/** @irq_work: The interrupt's bottom half behind gma500, which owns the line. */
	struct work_struct irq_work;

	/** @regs: The core registers (BAR 0). */
	void __iomem *regs;
	/** @irq: Its interrupt. */
	int irq;

	/** @family: The core the bus says is there, set before the probe reads a register. */
	const struct pvrsgx_core_family *family;
	/** @cr: Its registers that move between cores (@family's). */
	const struct pvrsgx_core_regs *cr;
	/** @core: What the driver knows about this part. */
	const struct pvrsgx_core_info *core;
	/** @core_id: SGX_CR_CORE_ID. */
	u32 core_id;
	/** @core_rev: SGX_CR_CORE_REVISION. */
	u32 core_rev;

	/** @kernel_mmu: Page directory the microkernel translates through. */
	struct pvrsgx_mmu *kernel_mmu;
	/** @idle_mmu: Empty directory for list 0 while no job runs. */
	struct pvrsgx_mmu *idle_mmu;
	/** @reset_mmu: Scratch directory that absorbs faults during reset. */
	struct pvrsgx_mmu *reset_mmu;
	/** @reset_page: The page every address of @reset_mmu maps. */
	struct page *reset_page;

	/** @fw: The microkernel and its buffers. */
	struct pvrsgx_fw fw;
	/** @fw_running: The microkernel has started and takes commands. */
	bool fw_running;
	/**
	 * @powered: The SGX is clocked and its registers may be read. Off between
	 * a stop and the next start: a read of an unclocked CE5300 block can hang
	 * the bus, and the interrupt line is shared.
	 */
	bool powered;
	/** @fw_started: The microkernel has run since load; later starts are restarts. */
	bool fw_started;
	/** @fw_gen: Restarts (resets and runtime resumes) since load. */
	u32 fw_gen;
	/** @lost: Firmware failed to restart; every job fails with -EIO. */
	bool lost;

	/** @ccb_lock: Serialises kernel CCB writers. */
	struct mutex ccb_lock;
	/** @ccb_spin: Keeps @ccb_issued and the write offset in step for readers. */
	spinlock_t ccb_spin;
	/** @ccb_issued: Commands written to the kernel CCB since the firmware started. */
	u64 ccb_issued;

	/** @sched: The one scheduler; one job on the GPU at a time. */
	struct drm_gpu_scheduler sched;
	/** @fence_context: Context of the hardware fences. */
	u64 fence_context;
	/** @done_lock: Keeps completions in order while imports are flushed. */
	struct mutex done_lock;
	/** @fence_lock: Protects @fence_seqno and @active. */
	spinlock_t fence_lock;
	/** @fence_seqno: Last hardware fence seqno. */
	u32 fence_seqno;
	/** @active: Jobs handed to the firmware, oldest first. */
	struct list_head active;
	/** @last_fence: Hardware fence of the job last sent to the firmware. */
	struct dma_fence *last_fence;
	/** @poll_work: Checks completions while jobs are out. */
	struct delayed_work poll_work;

	/** @reset_count: GPU resets since load. */
	u32 reset_count;
	/** @reset_logged: @reset_count at the last "started" message (resumes are quiet). */
	u32 reset_logged;

	/** @pb: The parameter buffer every render context shares (pvrsgx_render.c). */
	struct pvrsgx_pb *pb;
	/** @pb_lock: Serializes @pb's creation. */
	struct mutex pb_lock;

	/** @kobj_lock: Protects @krange and @graveyard. */
	struct mutex kobj_lock;
	/** @krange: Allocators of the device-wide kernel object ranges. */
	struct drm_mm krange[PVRSGX_KRANGE_COUNT];
	/** @graveyard: Released kernel objects the firmware may still read. */
	struct list_head graveyard;
	/** @renders_submitted: Render jobs handed to the firmware, for reaping @graveyard. */
	u64 renders_submitted;
	/** @renders_done: Render jobs the firmware has finished. */
	u64 renders_done;
	/** @reap_work: Frees @graveyard entries a later render made safe. */
	struct work_struct reap_work;

	/** @irq_count: SW_EVENT interrupts taken (debugfs). */
	atomic_t irq_count;
};

#define to_pvrsgx_device(drm) container_of_const(drm, struct pvrsgx_device, base)

/**
 * struct pvrsgx_file - Per-open state
 */
struct pvrsgx_file {
	/** @sdev: The device. */
	struct pvrsgx_device *sdev;
	/** @vms: VM contexts by handle. */
	struct xarray vms;
	/** @contexts: Contexts by handle. */
	struct xarray contexts;
	/** @rtds: RT datasets by handle. */
	struct xarray rtds;
};

static inline struct pvrsgx_file *to_pvrsgx_file(struct drm_file *file)
{
	return file->driver_priv;
}

static inline u32 sgx_rd(struct pvrsgx_device *sdev, u32 reg)
{
	return ioread32(sdev->regs + reg);
}

static inline void sgx_wr(struct pvrsgx_device *sdev, u32 reg, u32 val)
{
	iowrite32(val, sdev->regs + reg);
}

/*
 * The microkernel translates through the last directory list; TA, 3D and
 * host requests through list 0, which the microkernel points at the job's
 * page directory.
 */
static inline u32 pvrsgx_dirlist_edm(const struct pvrsgx_device *sdev)
{
	return sdev->family->num_dir_lists - 1;
}

int pvrsgx_kmem_alloc(struct pvrsgx_device *sdev, struct pvrsgx_kmem *km,
		      u32 dev_addr, u32 size, u32 pte_flags);
void pvrsgx_kmem_free(struct pvrsgx_device *sdev, struct pvrsgx_kmem *km);

int pvrsgx_device_init(struct pvrsgx_device *sdev);
void pvrsgx_device_fini(struct pvrsgx_device *sdev);
void pvrsgx_reset(struct pvrsgx_device *sdev, bool recovery);
int pvrsgx_poll(struct pvrsgx_device *sdev, const u32 *addr, u32 mask, u32 val,
		unsigned int timeout_us);

#endif /* PVRSGX_DEVICE_H */
