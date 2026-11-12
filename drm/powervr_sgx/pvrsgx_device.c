// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

#include <drm/drm_managed.h>
#include <drm/drm_print.h>
#if IS_ENABLED(CONFIG_DRM_POWERVR_SGX_GMA500)
#include <drm/gma500_sgx.h>
#endif

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "pvrsgx_ccb.h"
#include "pvrsgx_core.h"
#include "pvrsgx_device.h"
#include "pvrsgx_fw.h"
#include "pvrsgx_job.h"
#include "pvrsgx_trace.h"
#include "pvrsgx_kobj.h"
#include "pvrsgx_mmu.h"
#include "pvrsgx_render.h"

int pvrsgx_poll(struct pvrsgx_device *sdev, const u32 *addr, u32 mask, u32 val,
		unsigned int timeout_us)
{
	ktime_t end = ktime_add_us(ktime_get(), timeout_us);

	for (;;) {
		if ((READ_ONCE(*addr) & mask) == val)
			return 0;
		if (ktime_after(ktime_get(), end))
			return (READ_ONCE(*addr) & mask) == val ? 0 : -ETIMEDOUT;
		usleep_range(20, 50);
	}
}

/*
 * Kernel-owned memory at a fixed device address, mapped in the firmware's
 * directory. Pages below 4 GiB, write-combined in every CPU mapping. gfp adds
 * to GFP_KERNEL: __GFP_ACCOUNT for what a client's ioctl allocates.
 */
int pvrsgx_kmem_alloc(struct pvrsgx_device *sdev, struct pvrsgx_kmem *km,
		      u32 dev_addr, u32 size, u32 pte_flags, gfp_t gfp)
{
	unsigned int i;
	int err = -ENOMEM;

	km->dev_addr = dev_addr;
	km->size = PAGE_ALIGN(size);
	km->npages = km->size >> PAGE_SHIFT;
	km->pages = kvcalloc(km->npages, sizeof(*km->pages), GFP_KERNEL | gfp);
	if (!km->pages)
		return -ENOMEM;
	for (i = 0; i < km->npages; i++) {
		km->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO | __GFP_DMA32 | gfp);
		if (!km->pages[i])
			goto err_free;
	}
	if (pvrsgx_pages_wc(km->pages, km->npages))
		goto err_free;
	km->cpu = vmap(km->pages, km->npages, VM_MAP, pgprot_writecombine(PAGE_KERNEL));
	if (!km->cpu)
		goto err_wb;
	if (!km->mmu)
		km->mmu = sdev->kernel_mmu;
	err = pvrsgx_mmu_map_pages(km->mmu, dev_addr, km->pages, km->npages, pte_flags);
	if (err)
		goto err_unmap;
	return 0;

err_unmap:
	vunmap(km->cpu);
err_wb:
	pvrsgx_pages_wb(km->pages, km->npages);
err_free:
	while (i--)
		__free_page(km->pages[i]);
	kvfree(km->pages);
	km->pages = NULL;
	return err;
}

void pvrsgx_kmem_free(struct pvrsgx_device *sdev, struct pvrsgx_kmem *km)
{
	if (!km->pages)
		return;
	pvrsgx_mmu_unmap(km->mmu, km->dev_addr, km->size);
	pvrsgx_kmem_release(km);
}

/* The memory of kmem the caller has already unmapped from the GPU. */
void pvrsgx_kmem_release(struct pvrsgx_kmem *km)
{
	if (!km->pages)
		return;
	vunmap(km->cpu);
	pvrsgx_pages_wb(km->pages, km->npages);
	for (unsigned int i = 0; i < km->npages; i++)
		__free_page(km->pages[i]);
	kvfree(km->pages);
	km->pages = NULL;
}

/* A few core clocks for a register write to take effect. */
static void reset_sleep(struct pvrsgx_device *sdev)
{
	sgx_rd(sdev, SGX_CR_SOFT_RESET);
	udelay(1);
}

static void soft_reset(struct pvrsgx_device *sdev, bool bif)
{
	sgx_wr(sdev, SGX_CR_SOFT_RESET,
	       sdev->cr->soft_reset_cores | (bif ? sdev->cr->soft_reset_bif : 0));
}

/* BIF_CTRL is 0 whenever the microkernel runs: its invalidates pulse a bit, then write 0. */
static void inval_dc(struct pvrsgx_device *sdev)
{
	sgx_wr(sdev, SGX_CR_BIF_CTRL, SGX_CR_BIF_CTRL_INVALDC);
	sgx_wr(sdev, SGX_CR_BIF_CTRL, 0);
	reset_sleep(sdev);
}

/*
 * The reset sequence for a single-core SGX5xx: reset everything, clear the directory
 * lists, take the cores out of reset with list 0 on a scratch directory that
 * absorbs faults left over from before, then give the microkernel its own
 * directory on the last list (list 15 on the SGX545).
 */
void pvrsgx_reset(struct pvrsgx_device *sdev, bool recovery)
{
	soft_reset(sdev, true);
	reset_sleep(sdev);

	sgx_wr(sdev, SGX_CR_BIF_CTRL, 0);
	sgx_wr_opt(sdev, sdev->cr->bif_bank_set, 0);
	sgx_wr_opt(sdev, sdev->cr->bif_bank0, 0);
	for (unsigned int i = 0; i < sdev->family->num_dir_lists; i++)
		sgx_wr(sdev, sgx_cr_bif_dir_list_base(i), 0);

	if (recovery) {
		sgx_wr(sdev, sgx_cr_bif_dir_list_base(0), pvrsgx_mmu_pd_addr(sdev->reset_mmu));
		reset_sleep(sdev);
		soft_reset(sdev, false);
		reset_sleep(sdev);
		inval_dc(sdev);

		/* Let each faulting request complete on a scratch page. */
		for (int n = 0; n < 32; n++) {
			u32 stat = sgx_rd(sdev, SGX_CR_BIF_INT_STAT), addr;

			if (!(stat & sdev->cr->bif_int_stat_fault_req))
				break;
			addr = sgx_rd(sdev, SGX_CR_BIF_FAULT) & sdev->cr->bif_fault_addr;
			drm_warn(&sdev->base, "reset: page fault 0x%x at 0x%08x\n", stat, addr);
			soft_reset(sdev, true);
			pvrsgx_mmu_map_pages(sdev->reset_mmu, addr, &sdev->reset_page, 1, 0);
			sgx_wr(sdev, SGX_CR_EVENT_HOST_CLEAR, sgx_rd(sdev, SGX_CR_EVENT_STATUS));
			sgx_wr(sdev, SGX_CR_EVENT_HOST_CLEAR2, sgx_rd(sdev, SGX_CR_EVENT_STATUS2));
			/* The reset page's PTE is in memory before the cores leave reset. */
			wmb();
			reset_sleep(sdev);
			soft_reset(sdev, false);
			reset_sleep(sdev);
			inval_dc(sdev);
			pvrsgx_mmu_unmap(sdev->reset_mmu, addr, SGX_MMU_PAGE_SIZE);
		}
	} else {
		soft_reset(sdev, false);
		reset_sleep(sdev);
	}

	sgx_wr_opt(sdev, sdev->cr->bif_bank0,
		   FIELD_PREP(SGX_CR_BIF_BANK0_INDEX_EDM, pvrsgx_dirlist_edm(sdev)));
	/* With one directory list the EDM's is list 0, set below. */
	if (pvrsgx_dirlist_edm(sdev))
		sgx_wr(sdev, sgx_cr_bif_dir_list_base(pvrsgx_dirlist_edm(sdev)),
		       pvrsgx_mmu_pd_addr(sdev->kernel_mmu));
	sgx_wr(sdev, sgx_cr_bif_dir_list_base(0),
	       pvrsgx_mmu_pd_addr(sdev->idle_mmu));
	inval_dc(sdev);

	sgx_wr(sdev, SGX_CR_SOFT_RESET, 0);
	reset_sleep(sdev);
}

/* The microkernel raises SW_EVENT when it wants the host. */
static irqreturn_t pvrsgx_irq(int irq, void *arg)
{
	struct pvrsgx_device *sdev = arg;
	u32 status;

	if (!READ_ONCE(sdev->powered))
		return IRQ_NONE;
	status = sgx_rd(sdev, SGX_CR_EVENT_STATUS) & sgx_rd(sdev, SGX_CR_EVENT_HOST_ENABLE);
	if (!(status & SGX_CR_EVENT_STATUS_SW_EVENT))
		return IRQ_NONE;
	trace_pvrsgx_irq(status);
	sgx_wr(sdev, SGX_CR_EVENT_HOST_CLEAR,
	       SGX_CR_EVENT_HOST_CLEAR_SW_EVENT | SGX_CR_EVENT_HOST_CLEAR_MASTER_INTERRUPT);
	atomic_inc(&sdev->irq_count);
	/*
	 * The vendor microkernel interrupts at TA and 3D completion too: wake the
	 * thread only for a finished job or a lockup report.
	 */
	if (!sdev->fw_running || pvrsgx_job_oldest_done(sdev) ||
	    (pvrsgx_fw_hostctl(sdev, HOSTCTL_INTERRUPT_FLAGS) & HOSTCTL_INTERRUPT_HWR))
		return IRQ_WAKE_THREAD;
	return IRQ_HANDLED;
}

static irqreturn_t pvrsgx_irq_thread(int irq, void *arg)
{
	struct pvrsgx_device *sdev = arg;
	u32 flags, clear;

	if (!sdev->fw_running)
		return IRQ_HANDLED;

	flags = pvrsgx_fw_hostctl(sdev, HOSTCTL_INTERRUPT_FLAGS);
	clear = pvrsgx_fw_hostctl(sdev, HOSTCTL_INTERRUPT_CLEAR);
	if ((flags & HOSTCTL_INTERRUPT_HWR) && !(clear & HOSTCTL_INTERRUPT_HWR))
		drm_err_ratelimited(&sdev->base, "microkernel reports a lockup (%u)\n",
				    pvrsgx_fw_hostctl(sdev, HOSTCTL_UK_LOCKUPS));
	/*
	 * The microkernel's idle notice: acknowledge it, as the vendor's GPL
	 * kernel services do (sgxutils.c, SGXTestActivePowerEvent).
	 * Powering down is runtime PM's business, so nothing else happens here.
	 */
	if ((flags & ~clear) & HOSTCTL_INTERRUPT_ACTIVE_POWER)
		pvrsgx_fw_hostctl_set(sdev, HOSTCTL_INTERRUPT_CLEAR,
				      clear | HOSTCTL_INTERRUPT_ACTIVE_POWER);

	pvrsgx_job_process_done(sdev);
	return IRQ_HANDLED;
}

/*
 * Which core this is (pvrsgx_core.c). Anything else is refused before the
 * driver writes a register: its errata, clocks and microkernel are unknown.
 */
static int identify(struct pvrsgx_device *sdev)
{
	if (!sdev->family) {
		drm_err(&sdev->base, "not an SGX core this driver knows; not touching it\n");
		return -ENODEV;
	}
	sdev->cr = sdev->family->regs;

	/* Safe on a core in any state. */
	sdev->core_id = sgx_rd(sdev, sdev->cr->core_id);
	sdev->core_rev = sgx_rd(sdev, sdev->cr->core_revision);

	sdev->core = pvrsgx_core_lookup(sdev->family, sdev->core_id, sdev->core_rev);
	if (!sdev->core) {
		drm_err(&sdev->base,
			"%s core id 0x%08x revision 0x%08x is not one this driver knows; not touching it. Please report these two values.\n",
			sdev->family->name, sdev->core_id, sdev->core_rev);
		return -ENODEV;
	}
	drm_info(&sdev->base, "SGX core id 0x%08x revision 0x%08x: %s\n",
		 sdev->core_id, sdev->core_rev, sdev->core->name);
	return 0;
}

#if IS_ENABLED(CONFIG_DRM_POWERVR_SGX_GMA500)
/* Behind gma500 the hard handler runs from gma500's; its thread is a work item. */
static irqreturn_t pvrsgx_gma_irq(void *data)
{
	struct pvrsgx_device *sdev = data;
	irqreturn_t ret = pvrsgx_irq(sdev->irq, sdev);

	if (ret == IRQ_WAKE_THREAD) {
		queue_work(system_highpri_wq, &sdev->irq_work);
		return IRQ_HANDLED;
	}
	return ret;
}

static void pvrsgx_gma_irq_work(struct work_struct *work)
{
	struct pvrsgx_device *sdev = container_of(work, struct pvrsgx_device, irq_work);

	pvrsgx_irq_thread(sdev->irq, sdev);
}
#endif

static int irq_setup(struct pvrsgx_device *sdev)
{
#if IS_ENABLED(CONFIG_DRM_POWERVR_SGX_GMA500)
	if (sdev->gma_sgx) {
		INIT_WORK(&sdev->irq_work, pvrsgx_gma_irq_work);
		return sdev->gma_sgx->set_irq_handler(sdev->gma_sgx, pvrsgx_gma_irq, sdev);
	}
#endif
	return request_threaded_irq(sdev->irq, pvrsgx_irq, pvrsgx_irq_thread, IRQF_SHARED,
				    "powervr_sgx", sdev);
}

static void irq_teardown(struct pvrsgx_device *sdev)
{
#if IS_ENABLED(CONFIG_DRM_POWERVR_SGX_GMA500)
	if (sdev->gma_sgx) {
		sdev->gma_sgx->set_irq_handler(sdev->gma_sgx, NULL, NULL);
		cancel_work_sync(&sdev->irq_work);
		return;
	}
#endif
	free_irq(sdev->irq, sdev);
}

/*
 * The software state, at the DRM device's release: after unbind, and after the
 * last file is closed, so nothing a file can reach goes before the file does.
 */
static void pvrsgx_device_release(struct drm_device *drm, void *arg)
{
	struct pvrsgx_device *sdev = arg;

	pvrsgx_job_fini(sdev);
	pvrsgx_ccb_inval_reap(sdev, true);
	pvrsgx_pb_put(sdev->pb);
	mutex_destroy(&sdev->pb_lock);
	pvrsgx_kobj_fini(sdev);
	pvrsgx_fw_fini(sdev);
	__free_page(sdev->reset_page);
	/* The reset directory still names the scratch page's tables, never the page. */
	pvrsgx_mmu_destroy(sdev->reset_mmu);
	pvrsgx_mmu_destroy(sdev->idle_mmu);
	pvrsgx_mmu_destroy(sdev->kernel_mmu);
}

int pvrsgx_device_init(struct pvrsgx_device *sdev)
{
	int err;

	mutex_init(&sdev->ccb_lock);
	mutex_init(&sdev->done_lock);
	spin_lock_init(&sdev->ccb_spin);
	spin_lock_init(&sdev->inval_lock);
	INIT_LIST_HEAD(&sdev->inval_waits);

	sdev->kernel_mmu = pvrsgx_mmu_create(sdev);
	sdev->idle_mmu = pvrsgx_mmu_create(sdev);
	sdev->reset_mmu = pvrsgx_mmu_create(sdev);
	sdev->reset_page = alloc_page(GFP_KERNEL | __GFP_ZERO | __GFP_DMA32);
	if (IS_ERR(sdev->kernel_mmu) || IS_ERR(sdev->idle_mmu) ||
	    IS_ERR(sdev->reset_mmu) || !sdev->reset_page) {
		err = -ENOMEM;
		goto err_mmu;
	}

	err = identify(sdev);
	if (err)
		goto err_mmu;

	pvrsgx_kobj_init(sdev);
	mutex_init(&sdev->pb_lock);
	err = pvrsgx_fw_init(sdev);
	if (err)
		goto err_fw;

	err = pvrsgx_job_init(sdev);
	if (err)
		goto err_fw;

	/* From here the release frees the software state, on success or failure. */
	err = drmm_add_action_or_reset(&sdev->base, pvrsgx_device_release, sdev);
	if (err)
		return err;

	err = irq_setup(sdev);
	if (err)
		return err;

	err = pvrsgx_fw_start(sdev);
	if (err) {
		pvrsgx_fw_stop(sdev);
		irq_teardown(sdev);
	}
	return err;

err_fw:
	pvrsgx_fw_fini(sdev);
	pvrsgx_kobj_fini(sdev);
err_mmu:
	if (sdev->reset_page)
		__free_page(sdev->reset_page);
	pvrsgx_mmu_destroy(sdev->reset_mmu);
	pvrsgx_mmu_destroy(sdev->idle_mmu);
	pvrsgx_mmu_destroy(sdev->kernel_mmu);
	return err;
}

/*
 * The hardware, at unbind or when registration fails: no job reaches the GPU
 * again, the firmware stops, the interrupt goes. Open files keep working
 * against the software state until they close.
 */
void pvrsgx_device_unplug(struct pvrsgx_device *sdev)
{
	pvrsgx_job_unplug(sdev);
	irq_teardown(sdev);
}
