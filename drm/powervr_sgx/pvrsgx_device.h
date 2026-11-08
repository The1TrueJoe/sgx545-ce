/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_DEVICE_H
#define PVRSGX_DEVICE_H

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_mm.h>
#include <drm/gpu_scheduler.h>

#include <linux/io.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include "pvrsgx_regs.h"

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
struct gma500_sgx_device;
struct pci_dev;
struct pvrsgx_mmu;
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
	PVRSGX_FW_MISC,		/* misc info, filled by GETMISCINFO */
	PVRSGX_FW_HWPERF,	/* HWPerf circular buffer */
	PVRSGX_FW_TASIG,	/* TA signature register list */
	PVRSGX_FW_3DSIG,	/* 3D signature register list */
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
	/** @host_kick: Microkernel entry of each SGXMKIF command type. */
	u32 host_kick[11];
	/** @cache_control: Invalidates to request with the next command. */
	u32 cache_control;
	/** @interface: DRM_POWERVR_SGX_FW_IF_*. */
	u32 interface;
	/** @ddk: The DDK's microkernel, which runs under the job's directory. */
	bool ddk;
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
	/** @ddk_version: Version the microkernel reports through GETMISCINFO. */
	u32 ddk_version;
	/** @ddk_build: Build number it reports with it. */
	u32 ddk_build;
};

/**
 * struct pvrsgx_device - The SGX545
 */
struct pvrsgx_device {
	/** @base: The DRM device. */
	struct drm_device base;
	/** @pdev: The SGX PCI function (CE5300), or NULL behind gma500 (Cedarview). */
	struct pci_dev *pdev;
	/** @gma_sgx: The gma500 auxiliary device behind which the SGX sits, or NULL. */
	struct gma500_sgx_device *gma_sgx;
	/** @irq_work: The interrupt's bottom half behind gma500, which owns the line. */
	struct work_struct irq_work;
	/** @parent_held: A runtime-PM reference on gma500 held while bound (runtime_pm=0). */
	bool parent_held;

	/** @regs: The core registers (BAR 0). */
	void __iomem *regs;
	/** @irq: Its interrupt. */
	int irq;

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

	/** @cleanup_lock: One cleanup command in flight (one status bit). */
	struct mutex cleanup_lock;
	/** @cleanup_seq: Sequence number of the last cleanup v2 command. */
	u32 cleanup_seq;
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
	/** @poll_work: Checks completions while jobs are out. */
	struct delayed_work poll_work;

	/** @reset_count: GPU resets since load. */
	u32 reset_count;
	/** @reset_logged: @reset_count at the last "started" message (resumes are quiet). */
	u32 reset_logged;

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

	/** @last_lock: Protects @last_rtds and @last_cmd_off. */
	struct mutex last_lock;
	/** @last_rtds: RT dataset of the last render job, for debugfs. */
	struct pvrsgx_rt_dataset *last_rtds;
	/** @last_cmd_off: Its command's offset in the render context CCB. */
	u32 last_cmd_off;
	/** @render_snap: Text dump taken as a last kick was written (debugfs). */
	char *render_snap;

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

struct pvrsgx_krange_span {
	u32 start, end;
};

extern const struct pvrsgx_krange_span pvrsgx_kernel_heaps[3];

int pvrsgx_kmem_alloc(struct pvrsgx_device *sdev, struct pvrsgx_kmem *km,
		      u32 dev_addr, u32 size, u32 pte_flags);
void pvrsgx_kmem_free(struct pvrsgx_device *sdev, struct pvrsgx_kmem *km);

int pvrsgx_device_init(struct pvrsgx_device *sdev);
void pvrsgx_device_fini(struct pvrsgx_device *sdev);
int pvrsgx_power_get(struct pvrsgx_device *sdev);
void pvrsgx_power_put(struct pvrsgx_device *sdev, u32 n);
void pvrsgx_power_init(struct pvrsgx_device *sdev);
void pvrsgx_power_fini(struct pvrsgx_device *sdev);
int pvrsgx_runtime_suspend(struct device *dev);
int pvrsgx_runtime_resume(struct device *dev);
void pvrsgx_reset(struct pvrsgx_device *sdev, bool recovery);
int pvrsgx_poll(struct pvrsgx_device *sdev, const u32 *addr, u32 mask, u32 val,
		unsigned int timeout_us);

#endif /* PVRSGX_DEVICE_H */
