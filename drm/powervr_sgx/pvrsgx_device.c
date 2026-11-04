// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

#include <drm/drm_print.h>

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include <asm/set_memory.h>

#include "pvrsgx_ccb.h"
#include "pvrsgx_device.h"
#include "pvrsgx_fw.h"
#include "pvrsgx_job.h"
#include "pvrsgx_kobj.h"
#include "pvrsgx_mmu.h"
#include "pvrsgx_render.h"

/*
 * The firmware's own heaps, each a whole number of 4 MiB directory entries: the
 * kernel's directory maps them, and with share_kernel_heaps every VM's too.
 */
const struct pvrsgx_krange_span pvrsgx_kernel_heaps[3] = {
	{ PVRSGX_HEAP_SYNC, PVRSGX_HEAP_PDS_PIXEL },
	{ PVRSGX_HEAP_KERNEL_CODE, PVRSGX_HEAP_PDS_VERTEX },
	{ PVRSGX_HEAP_KERNEL_DATA, PVRSGX_HEAP_USSE_PIXEL },
};

static bool idle_kernel_pd;
module_param(idle_kernel_pd, bool, 0444);
MODULE_PARM_DESC(idle_kernel_pd,
		 "Point directory list 0 at the firmware's page directory while idle (debug; default: an empty one)");

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
 * directory. Pages below 4 GiB, write-combined in every CPU mapping.
 */
int pvrsgx_kmem_alloc(struct pvrsgx_device *sdev, struct pvrsgx_kmem *km,
		      u32 dev_addr, u32 size, u32 pte_flags)
{
	unsigned int i;
	int err = -ENOMEM;

	km->dev_addr = dev_addr;
	km->size = PAGE_ALIGN(size);
	km->npages = km->size >> PAGE_SHIFT;
	km->pages = kvcalloc(km->npages, sizeof(*km->pages), GFP_KERNEL);
	if (!km->pages)
		return -ENOMEM;
	for (i = 0; i < km->npages; i++) {
		km->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO | __GFP_DMA32);
		if (!km->pages[i])
			goto err_free;
	}
	if (set_pages_array_wc(km->pages, km->npages))
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
	set_pages_array_wb(km->pages, km->npages);
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
	vunmap(km->cpu);
	set_pages_array_wb(km->pages, km->npages);
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
	       SGX_CR_SOFT_RESET_CORES | (bif ? SGX_CR_SOFT_RESET_BIF : 0));
}

static void inval_dc(struct pvrsgx_device *sdev)
{
	sgx_wr(sdev, SGX_CR_BIF_CTRL, SGX_CR_BIF_CTRL_INVALDC);
	sgx_wr(sdev, SGX_CR_BIF_CTRL, 0);
	reset_sleep(sdev);
}

/*
 * SGXReset for a single-core SGX545: reset everything, clear the directory
 * lists, take the cores out of reset with list 0 on a scratch directory that
 * absorbs faults left over from before, then give the microkernel its own
 * directory on list 15.
 */
void pvrsgx_reset(struct pvrsgx_device *sdev, bool recovery)
{
	soft_reset(sdev, true);
	reset_sleep(sdev);

	sgx_wr(sdev, SGX_CR_BIF_CTRL, 0);
	sgx_wr(sdev, SGX_CR_BIF_BANK_SET, 0);
	sgx_wr(sdev, SGX_CR_BIF_BANK0, 0);
	for (unsigned int i = 0; i < SGX_BIF_NUM_DIRLISTS; i++)
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

			if (!(stat & SGX_CR_BIF_INT_STAT_FAULT_REQ))
				break;
			addr = sgx_rd(sdev, SGX_CR_BIF_FAULT) & SGX_MMU_ADDR_MASK;
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

	sgx_wr(sdev, SGX_CR_BIF_BANK0,
	       FIELD_PREP(SGX_CR_BIF_BANK0_INDEX_EDM, SGX_BIF_DIRLIST_EDM));
	sgx_wr(sdev, sgx_cr_bif_dir_list_base(SGX_BIF_DIRLIST_EDM),
	       pvrsgx_mmu_pd_addr(sdev->kernel_mmu));
	sgx_wr(sdev, sgx_cr_bif_dir_list_base(0),
	       pvrsgx_mmu_pd_addr(idle_kernel_pd ? sdev->kernel_mmu : sdev->idle_mmu));
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
	sgx_wr(sdev, SGX_CR_EVENT_HOST_CLEAR,
	       SGX_CR_EVENT_HOST_CLEAR_SW_EVENT | SGX_CR_EVENT_HOST_CLEAR_MASTER_INTERRUPT);
	atomic_inc(&sdev->irq_count);
	return IRQ_WAKE_THREAD;
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

	pvrsgx_job_process_done(sdev);
	return IRQ_HANDLED;
}

int pvrsgx_device_init(struct pvrsgx_device *sdev)
{
	struct pci_dev *pdev = sdev->pdev;
	int err;

	mutex_init(&sdev->ccb_lock);
	mutex_init(&sdev->cleanup_lock);
	mutex_init(&sdev->last_lock);
	spin_lock_init(&sdev->ccb_spin);

	sdev->kernel_mmu = pvrsgx_mmu_create(sdev);
	sdev->idle_mmu = pvrsgx_mmu_create(sdev);
	sdev->reset_mmu = pvrsgx_mmu_create(sdev);
	sdev->reset_page = alloc_page(GFP_KERNEL | __GFP_ZERO | __GFP_DMA32);
	if (IS_ERR(sdev->kernel_mmu) || IS_ERR(sdev->idle_mmu) ||
	    IS_ERR(sdev->reset_mmu) || !sdev->reset_page) {
		err = -ENOMEM;
		goto err_mmu;
	}

	for (int i = 0; i < ARRAY_SIZE(pvrsgx_kernel_heaps); i++) {
		err = pvrsgx_mmu_prealloc(sdev->kernel_mmu, pvrsgx_kernel_heaps[i].start,
					  pvrsgx_kernel_heaps[i].end);
		if (err)
			goto err_mmu;
	}

	/* Safe on a core in any state. */
	sdev->core_id = sgx_rd(sdev, SGX_CR_CORE_ID);
	sdev->core_rev = sgx_rd(sdev, SGX_CR_CORE_REVISION);
	drm_info(&sdev->base, "SGX core id 0x%08x revision 0x%08x\n",
		 sdev->core_id, sdev->core_rev);

	pvrsgx_kobj_init(sdev);
	err = pvrsgx_fw_init(sdev);
	if (err)
		goto err_fw;

	err = pvrsgx_job_init(sdev);
	if (err)
		goto err_fw;

	sdev->irq = pdev->irq;
	err = request_threaded_irq(sdev->irq, pvrsgx_irq, pvrsgx_irq_thread, IRQF_SHARED,
				   "powervr_sgx", sdev);
	if (err)
		goto err_job;

	err = pvrsgx_fw_start(sdev);
	if (err)
		goto err_irq;
	return 0;

err_irq:
	pvrsgx_fw_stop(sdev);
	free_irq(sdev->irq, sdev);
err_job:
	pvrsgx_job_fini(sdev);
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

void pvrsgx_device_fini(struct pvrsgx_device *sdev)
{
	pvrsgx_job_fini(sdev);
	pvrsgx_rtds_put(sdev->last_rtds);
	sdev->last_rtds = NULL;
	kvfree(sdev->render_snap);
	pvrsgx_fw_stop(sdev);
	free_irq(sdev->irq, sdev);
	pvrsgx_kobj_fini(sdev);
	pvrsgx_fw_fini(sdev);
	__free_page(sdev->reset_page);
	/* The reset directory still names the scratch page's tables, never the page. */
	pvrsgx_mmu_destroy(sdev->reset_mmu);
	pvrsgx_mmu_destroy(sdev->idle_mmu);
	pvrsgx_mmu_destroy(sdev->kernel_mmu);
}

/*
 * Runtime PM. The SGX function has no PCI PM capability, so the PCI core keeps
 * it in D0: suspend is the microkernel's power-off (pvrsgx_fw_stop), resume
 * the same full start a reset does. A job on the GPU holds a reference from
 * run_job until it leaves the active list.
 */
static bool runtime_pm = true;
module_param(runtime_pm, bool, 0444);
MODULE_PARM_DESC(runtime_pm, "Power the microkernel off when idle (default on; autosuspend: the device's power/autosuspend_delay_ms)");

#define AUTOSUSPEND_MS	500

int pvrsgx_power_get(struct pvrsgx_device *sdev)
{
	return runtime_pm ? pm_runtime_resume_and_get(&sdev->pdev->dev) : 0;
}

void pvrsgx_power_put(struct pvrsgx_device *sdev, u32 n)
{
	if (!runtime_pm || !n)
		return;
	pm_runtime_mark_last_busy(&sdev->pdev->dev);
	while (n--)
		pm_runtime_put_autosuspend(&sdev->pdev->dev);
}

/* After probe: drop the reference the PCI core takes for it, and allow suspend. */
void pvrsgx_power_init(struct pvrsgx_device *sdev)
{
	struct device *dev = &sdev->pdev->dev;

	if (!runtime_pm)
		return;
	pm_runtime_set_autosuspend_delay(dev, AUTOSUSPEND_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	pm_runtime_allow(dev);
}

/* Before remove (the PCI core has resumed the device): undo pvrsgx_power_init. */
void pvrsgx_power_fini(struct pvrsgx_device *sdev)
{
	struct device *dev = &sdev->pdev->dev;

	if (!runtime_pm)
		return;
	pm_runtime_forbid(dev);
	pm_runtime_dont_use_autosuspend(dev);
	pm_runtime_get_noresume(dev);
}

int pvrsgx_runtime_suspend(struct device *dev)
{
	struct pvrsgx_device *sdev = pci_get_drvdata(to_pci_dev(dev));

	if (!sdev->lost)
		pvrsgx_fw_stop(sdev);
	return 0;
}

int pvrsgx_runtime_resume(struct device *dev)
{
	struct pvrsgx_device *sdev = pci_get_drvdata(to_pci_dev(dev));

	/* A failed start loses the device: run_job then fails every job with -EIO. */
	if (!sdev->lost && pvrsgx_fw_start(sdev)) {
		drm_err(&sdev->base, "microkernel did not start on resume: device lost\n");
		sdev->lost = true;
	}
	return 0;
}
