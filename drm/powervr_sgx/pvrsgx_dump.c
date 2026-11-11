// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * Device state as text: debugfs "status", and the devcoredump taken when a
 * job hangs (/sys/class/devcoredump/devcd<N>/data; write to it to free it).
 */

#include <drm/drm_print.h>

#include <linux/bitfield.h>
#include <linux/devcoredump.h>
#include <linux/seq_buf.h>
#include <linux/vmalloc.h>

#include <uapi/drm/powervr_sgx_drm.h>

#include "pvrsgx_ccb.h"
#include "pvrsgx_device.h"
#include "pvrsgx_dump.h"
#include "pvrsgx_fw.h"
#include "pvrsgx_kobj.h"
#include "pvrsgx_job.h"
#include "pvrsgx_mmu.h"
#include "pvrsgx_render.h"
#include "pvrsgx_vm.h"

/* Registers safe to read while the clocks are on. */
static void dump_regs(struct seq_buf *sb, struct pvrsgx_device *sdev)
{
	const struct pvrsgx_core_regs *cr = sdev->cr;
	u32 edm = pvrsgx_dirlist_edm(sdev);
	const struct {
		const char *name;
		u32 reg;
	} regs[] = {
		{ "CLKGATECTL", SGX_CR_CLKGATECTL },
		{ "CLKGATECTL2", cr->clkgatectl2 },
		{ "CLKGATESTATUS", cr->clkgatestatus },
		{ "CORE_ID", cr->core_id },
		{ "CORE_REVISION", cr->core_revision },
		{ "SOFT_RESET", SGX_CR_SOFT_RESET },
		{ "EVENT_STATUS", SGX_CR_EVENT_STATUS },
		{ "EVENT_STATUS2", SGX_CR_EVENT_STATUS2 },
		{ "EVENT_HOST_ENABLE", SGX_CR_EVENT_HOST_ENABLE },
		{ "EVENT_HOST_ENABLE2", SGX_CR_EVENT_HOST_ENABLE2 },
		{ "BIF_CTRL", SGX_CR_BIF_CTRL },
		{ "BIF_INT_STAT", SGX_CR_BIF_INT_STAT },
		{ "BIF_FAULT", SGX_CR_BIF_FAULT },
		{ "BIF_BANK0", SGX_CR_BIF_BANK0 },
		{ "BIF_DIR_LIST_BASE0", SGX_CR_BIF_DIR_LIST_BASE0 },
		{ "BIF_DIR_LIST_BASE1", SGX_CR_BIF_DIR_LIST_BASE1 },
		{ "BIF_DIR_LIST_BASE2", SGX_CR_BIF_DIR_LIST_BASE1 + 4 },
		{ NULL, sgx_cr_bif_dir_list_base(edm) },
		{ "EDM_TASK_REG0", cr->edm_task_reg0 },
		{ "EDM_TASK_REG1", cr->edm_task_reg1 },
	};

	for (int i = 0; i < ARRAY_SIZE(regs); i++) {
		char name[24];

		/* 0 is a register this core lacks; only CLKGATECTL, the first, sits there. */
		if (i && !regs[i].reg)
			continue;
		if (regs[i].name)
			strscpy(name, regs[i].name);
		else
			snprintf(name, sizeof(name), "BIF_DIR_LIST_BASE%u", edm);
		seq_buf_printf(sb, "%-20s 0x%04x 0x%08x\n", name, regs[i].reg,
			       sgx_rd(sdev, regs[i].reg));
	}
}

void pvrsgx_dump_status(struct seq_buf *sb, struct pvrsgx_device *sdev, bool regs)
{
	u32 *ctl = sdev->fw.cpu[PVRSGX_FW_CCBCTL];
	struct pvrsgx_kobj_counts kc;

	seq_buf_printf(sb, "firmware running: %d lost: %d resets: %u irqs: %d\n", sdev->fw_running,
		       sdev->lost, sdev->reset_count, atomic_read(&sdev->irq_count));
	seq_buf_printf(sb, "kernel PD 0x%08x idle PD 0x%08x\n",
		       pvrsgx_mmu_pd_addr(sdev->kernel_mmu), pvrsgx_mmu_pd_addr(sdev->idle_mmu));
	seq_buf_printf(sb, "ccb wo %u ro %u issued %llu consumed %llu\n", READ_ONCE(ctl[0]),
		       READ_ONCE(ctl[1]), sdev->ccb_issued, pvrsgx_ccb_consumed(sdev));
	seq_buf_printf(sb, "hostctl init 0x%x power 0x%x uk-lockups %u ",
		       pvrsgx_fw_hostctl(sdev, HOSTCTL_INIT_STATUS),
		       pvrsgx_fw_hostctl(sdev, HOSTCTL_POWER_STATUS),
		       pvrsgx_fw_hostctl(sdev, HOSTCTL_UK_LOCKUPS));
	seq_buf_printf(sb, "int 0x%x/0x%x assert 0x%x\n",
		       pvrsgx_fw_hostctl(sdev, HOSTCTL_INTERRUPT_FLAGS),
		       pvrsgx_fw_hostctl(sdev, HOSTCTL_INTERRUPT_CLEAR),
		       pvrsgx_fw_hostctl(sdev, HOSTCTL_ASSERT_FAIL));
	seq_buf_printf(sb, "powered: %d\n", READ_ONCE(sdev->powered));
	if (pvrsgx_kobj_pending(sdev, &kc))
		seq_buf_printf(sb, "kobjs graveyard %u (%llu pages) unmapped %u (%llu pages)\n",
			       kc.graveyard, kc.graveyard_pages, kc.unmapped, kc.unmapped_pages);
	/* After a failed start the clocks are still on; after a stop they are not. */
	if (!regs || !READ_ONCE(sdev->powered))
		return;
	dump_regs(sb, sdev);
}

#define COREDUMP_BYTES	SZ_128K

static void dump_lookup(struct seq_buf *sb, const char *name, struct pvrsgx_mmu *mmu, u32 addr)
{
	u32 pde, pte;

	if (!mmu)
		return;
	if (pvrsgx_mmu_lookup(mmu, addr, &pde, &pte))
		seq_buf_printf(sb, "  %s directory 0x%08x: pde 0x%08x pte 0x%08x\n", name,
			       pvrsgx_mmu_pd_addr(mmu), pde, pte);
	else
		seq_buf_printf(sb, "  %s directory 0x%08x: busy\n", name, pvrsgx_mmu_pd_addr(mmu));
}

/*
 * What the BIF was set to, and how each directory the job could have used
 * translates the faulting address. After a start, so every register is safe
 * to read.
 */
static void dump_bif(struct seq_buf *sb, struct pvrsgx_device *sdev, struct pvrsgx_job *bad)
{
	const struct pvrsgx_core_regs *cr = sdev->cr;
	u32 stat = sgx_rd(sdev, SGX_CR_BIF_INT_STAT);
	u32 addr = sgx_rd(sdev, SGX_CR_BIF_FAULT) & SGX_MMU_ADDR_MASK;

	seq_buf_printf(sb, "BIF: bank set 0x%08x bank0 0x%08x mem req 0x%08x\n",
		       sgx_rd(sdev, SGX_CR_BIF_BANK_SET), sgx_rd(sdev, SGX_CR_BIF_BANK0),
		       sgx_rd(sdev, SGX_CR_BIF_MEM_REQ_STAT));
	for (unsigned int i = 0; i < sdev->family->num_dir_lists; i++)
		seq_buf_printf(sb, "  dir list %2u: 0x%08x\n", i,
			       sgx_rd(sdev, sgx_cr_bif_dir_list_base(i)));
	for (unsigned int i = 0; i < 16; i++)
		seq_buf_printf(sb, "  use code base %2u: 0x%08x\n", i,
			       sgx_rd(sdev, cr->use_code_base0 + 4 * i));
	seq_buf_printf(sb, "  pds exec base: 0x%08x\n", sgx_rd(sdev, cr->pds_exec_base));
	if (!(stat & cr->bif_int_stat_fault_req))
		return;
	seq_buf_printf(sb, "fault at 0x%08x: requestor 0x%04x type %u\n", addr,
		       field_get(cr->bif_int_stat_fault_req, stat),
		       field_get(cr->bif_int_stat_fault_type, stat));
	dump_lookup(sb, "job's", bad->ctx->vm->mmu, addr);
	dump_lookup(sb, "firmware's", sdev->kernel_mmu, addr);
	dump_lookup(sb, "idle", sdev->idle_mmu, addr);
}

/*
 * Called from the timeout handler with the scheduler stopped, before the
 * restart: what the microkernel and the hung job left behind.
 */
void pvrsgx_coredump(struct pvrsgx_device *sdev, struct pvrsgx_job *bad)
{
	static const char *const types[] = {
		[DRM_POWERVR_SGX_JOB_TYPE_RENDER] = "render",
		[DRM_POWERVR_SGX_JOB_TYPE_TRANSFER] = "transfer",
		[DRM_POWERVR_SGX_JOB_TYPE_NULL] = "null",
	};
	unsigned long flags;
	struct seq_buf sb;
	char *buf;

	/* The timeout handler signals fences: no reclaim, so a dump can be lost but never waits. */
	buf = __vmalloc(COREDUMP_BYTES, GFP_NOWAIT | __GFP_NOWARN);
	if (!buf)
		return;
	seq_buf_init(&sb, buf, COREDUMP_BYTES);
	seq_buf_printf(&sb, "powervr_sgx hang: %s job, ccb %llu, reset %u\n",
		       bad->type < ARRAY_SIZE(types) ? types[bad->type] : "?", bad->ccb_seq,
		       sdev->reset_count + 1);
	pvrsgx_dump_status(&sb, sdev, true);
	if (sdev->fw_running)
		dump_bif(&sb, sdev, bad);
	pvrsgx_fw_dump(&sb, sdev, PVRSGX_FW_HOSTCTL, "hostctl");
	pvrsgx_fw_dump(&sb, sdev, PVRSGX_FW_TA3DCTL, "ta3dctl");
	pvrsgx_fw_dump(&sb, sdev, PVRSGX_FW_CCBCTL, "ccbctl");
	pvrsgx_fw_dump(&sb, sdev, PVRSGX_FW_CCB, "ccb");
	pvrsgx_job_dump(&sb, bad);
	if (seq_buf_has_overflowed(&sb))
		drm_warn(&sdev->base, "coredump truncated\n");
	/* devcoredump registers a device, which allocates: from a worker. */
	spin_lock_irqsave(&sdev->fence_lock, flags);
	if (!sdev->coredump) {
		sdev->coredump = buf;
		sdev->coredump_len = seq_buf_used(&sb);
		buf = NULL;
	}
	spin_unlock_irqrestore(&sdev->fence_lock, flags);
	vfree(buf);
	queue_work(system_dfl_wq, &sdev->coredump_work);
}

static void coredump_work_fn(struct work_struct *work)
{
	struct pvrsgx_device *sdev = container_of(work, struct pvrsgx_device, coredump_work);
	unsigned long flags;
	size_t len;
	void *buf;

	spin_lock_irqsave(&sdev->fence_lock, flags);
	buf = sdev->coredump;
	len = sdev->coredump_len;
	sdev->coredump = NULL;
	spin_unlock_irqrestore(&sdev->fence_lock, flags);
	if (buf)
		dev_coredumpv(sdev->base.dev, buf, len, GFP_KERNEL);
}

void pvrsgx_coredump_init(struct pvrsgx_device *sdev)
{
	INIT_WORK(&sdev->coredump_work, coredump_work_fn);
}

void pvrsgx_coredump_fini(struct pvrsgx_device *sdev)
{
	cancel_work_sync(&sdev->coredump_work);
	vfree(sdev->coredump);
	sdev->coredump = NULL;
}
