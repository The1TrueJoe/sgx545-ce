// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * The SGX5xx cores this driver knows. A core that is not listed is refused
 * before any register write: on an unknown core the driver can't tell which
 * register writes are safe.
 */

#include <linux/kernel.h>
#include <linux/module.h>

#include <linux/sizes.h>

#include "pvrsgx_core.h"
#include "pvrsgx_fw.h"
#include "pvrsgx_hw.h"

bool pvrsgx_allow_unisolated;
module_param_named(allow_unisolated, pvrsgx_allow_unisolated, bool, 0444);
MODULE_PARM_DESC(allow_unisolated,
		 "Run a single-context core (SGX530), where clients can reach the firmware and each other's commands (default off)");

static const struct pvrsgx_core_regs sgx545_regs = {
	.core_id = SGX_CR_CORE_ID,
	.core_revision = SGX_CR_CORE_REVISION,
	.clkgatectl_units = SGX_CR_CLKGATECTL_UNITS,
	.clkgatectl_dpm = SGX_CR_CLKGATECTL_DPM_CLKG,
	.clkgatectl2 = SGX_CR_CLKGATECTL2,
	.clkgatectl2_units = SGX_CR_CLKGATECTL2_UNITS,
	.clkgatestatus = SGX_CR_CLKGATESTATUS,
	.clkgatestatus_dpm = SGX_CR_CLKGATESTATUS_DPM_CLKS,
	.soft_reset_bif = SGX_CR_SOFT_RESET_BIF_RESET,
	.soft_reset_cores = SGX_CR_SOFT_RESET_DPM_RESET | SGX_CR_SOFT_RESET_TA_RESET |
			    SGX_CR_SOFT_RESET_USE_RESET | SGX_CR_SOFT_RESET_ISP_RESET |
			    SGX_CR_SOFT_RESET_TSP_RESET,
	.event_kick2 = SGX_CR_EVENT_KICK2,
	.event_kick2_now = SGX_CR_EVENT_KICK2_NOW,
	.use_code_base0 = SGX_CR_USE_CODE_BASE_0,
	.pds_exec_base = SGX_CR_PDS_EXEC_BASE,
	.bif_int_stat_fault_req = SGX_CR_BIF_INT_STAT_FAULT_REQ,
	.bif_int_stat_fault_type = SGX_CR_BIF_INT_STAT_FAULT_TYPE,
	.bif_fault_addr = SGX_MMU_ADDR_MASK,
	.bif_bank_set = SGX_CR_BIF_BANK_SET,
	.bif_bank0 = SGX_CR_BIF_BANK0,
	.use_code_base_dm_shift = 25,
	.event_pds_enable = SGX545_CR_EVENT_PDS_ENABLE,
	.event_pds_enable_all = SGX545_CR_EVENT_PDS_ENABLE_ALL,
	.event_pds_base = SGX545_CR_EVENT_PDS_BASE,
	.event_pds_data0 = SGX545_CR_EVENT_PDS_DATA0,
	.event_pds_data1 = SGX545_CR_EVENT_PDS_DATA1,
	.edm_task_reg0 = SGX545_CR_EDM_TASK_REG0,
	.edm_task_reg1 = SGX545_CR_EDM_TASK_REG1,
};

/* docs/UAPI.md section 5: each heap runs to the next one's base. */
static const struct pvrsgx_heap_layout sgx545_heaps = {
	.params_3d = { PVRSGX_HEAP_3D_PARAMS, PVRSGX_HEAP_TA_DATA },
	.ta_data = { PVRSGX_HEAP_TA_DATA, PVRSGX_HEAP_SYNC },
	.sync = { PVRSGX_HEAP_SYNC, PVRSGX_HEAP_PDS_PIXEL },
	.pds_pixel = { PVRSGX_HEAP_PDS_PIXEL, PVRSGX_HEAP_KERNEL_CODE },
	.kernel_code = { PVRSGX_HEAP_KERNEL_CODE, PVRSGX_HEAP_PDS_VERTEX },
	.pds_vertex = { PVRSGX_HEAP_PDS_VERTEX, PVRSGX_HEAP_KERNEL_DATA },
	.kernel_data = { PVRSGX_HEAP_KERNEL_DATA, PVRSGX_HEAP_USSE_PIXEL },
	.usse_pixel = { PVRSGX_HEAP_USSE_PIXEL,
			PVRSGX_HEAP_USSE_PIXEL + PVRSGX_HEAP_USSE_PIXEL_SIZE },
	.usse_vertex = { PVRSGX_HEAP_USSE_VERTEX,
			 PVRSGX_HEAP_USSE_VERTEX + PVRSGX_HEAP_USSE_VERTEX_SIZE },
	.general = { PVRSGX_HEAP_GENERAL, PVRSGX_HEAP_TEXTURE },
	.texture = { PVRSGX_HEAP_TEXTURE, SZ_4G },
	.pb_max_pages = 0x8000,
};

/*
 * Init script part 2, in the vendor script's order. Most of these registers
 * have no name in the GPL header; the values are the vendor script's.
 */
static const struct pvrsgx_core_init_reg sgx545_init_regs[] = {
	{ 0x818, 0x7ff }, { 0x810, 1 }, { 0x814, 0xffff },
	{ SGX_CR_EVENT_HOST_ENABLE, SGX_CR_EVENT_STATUS_SW_EVENT },
	{ SGX_CR_EVENT_HOST_ENABLE2, 0 },
	{ 0x13c, 0 }, { 0xa7c, 0 }, { 0xa80, 0 }, { 0xa74, 0x208200 }, { 0x4e8, 0x100 },
	{ 0xa78, 0x2c }, { 0xaac, 0x45 }, { 0xabc, 0xe }, { 0x800, 0x30 }, { 0x804, 0xffff },
	{ 0xa00, 0x7c000 },
};

/*
 * SGX530 (TI OMAP3630), from sgx530defs.h (docs/porting/cores/sgx530.md).
 * It has no CLKGATECTL2, EVENT_KICK2 or BIF bank registers. The event PDS and
 * EDM task registers aren't in the GPL header: until the SGX530 microkernel
 * names them the family stays identify-only.
 */
static const struct pvrsgx_core_regs sgx530_regs = {
	.core_id = 0x0010,
	.core_revision = 0x0014,
	/* 2D b0-1, ISP b4-5, TSP b8-9, TA b12-13, DPM b16-17, USE b20-21 */
	.clkgatectl_units = 0x00333333,
	.clkgatectl_dpm = GENMASK(17, 16),
	.clkgatestatus = 0x0004,
	.clkgatestatus_dpm = BIT(16),
	.soft_reset_bif = BIT(0),
	/* 2D b1, DPM b2, TA b3, USE b4, ISP b5, TSP b6 */
	.soft_reset_cores = GENMASK(6, 1),
	.use_code_base0 = SGX_CR_USE_CODE_BASE_0,
	.pds_exec_base = SGX_CR_PDS_EXEC_BASE,
	.bif_int_stat_fault_req = GENMASK(13, 0),
	.bif_int_stat_fault_type = BIT(14),	/* read or write */
	.bif_fault_addr = GENMASK(27, 12),
	.use_code_base_dm_shift = 24,
};

/*
 * 256 MiB, every heap on a 4 MiB directory entry (docs/porting/
 * single-context.md section 5).
 */
static const struct pvrsgx_heap_layout sgx530_heaps = {
	.params_3d = { 0x00400000, 0x01400000 },
	.ta_data = { 0x01400000, 0x01c00000 },
	.sync = { 0x01c00000, 0x02000000 },
	.kernel_code = { 0x02000000, 0x02400000 },
	.kernel_data = { 0x02400000, 0x02c00000 },
	.pds_pixel = { 0x02c00000, 0x03000000 },
	.pds_vertex = { 0x03000000, 0x03400000 },
	.usse_pixel = { 0x03400000, 0x03900000 },
	.usse_vertex = { 0x03c00000, 0x03e00000 },
	.general = { 0x04000000, 0x0c000000 },
	.texture = { 0x0c000000, 0x10000000 },
	.pb_max_pages = 0x800,	/* the free list and the rest share the heap */
};

static const struct pvrsgx_core_family families[] = {
	{
		.name = "SGX545",
		.core = 545,
		.core_id = 0x01150000,
		.regs = &sgx545_regs,
		.features = PVRSGX_FEATURE_MULTIPLE_MEM_CONTEXTS |
			    PVRSGX_FEATURE_MULTI_EVENT_KICK |
			    PVRSGX_FEATURE_PDS_DATA_INTERLEAVE_2DWORDS,
		.isa = PVRSGX_ISA_SGX545,
		.num_dir_lists = 16,
		.va_bits = 32,
		.heaps = &sgx545_heaps,
		.init_regs = sgx545_init_regs,
		.num_init_regs = ARRAY_SIZE(sgx545_init_regs),
	},
	{
		/* One memory context, one kick (BRN_22934), 28-bit addresses. */
		.name = "SGX530",
		.core = 530,
		.regs = &sgx530_regs,
		.isa = PVRSGX_ISA_SGX545,
		.num_dir_lists = 1,
		.va_bits = 28,
		.heaps = &sgx530_heaps,
		.quirks = PVRSGX_QUIRK_BRN_28889,
	},
};

static const struct pvrsgx_core_info cores[] = {
	{
		.family = &families[0],
		.name = "Intel CE5300 (SGX545 1.0.14)",
		.core_rev = 0x0001000e,
		.clock_hz = 400000000,
		.fw_name = "powervr_sgx/sgx545_ukernel.bin",
	},
	{
		/*
		 * The same core one revision earlier (docs/UKERNEL.md, "SGX545
		 * core revisions"): same features and instruction set, the same
		 * microkernel. Its clock depends on the part (400 or 640 MHz);
		 * the microkernel's timer only needs the order of magnitude.
		 */
		.family = &families[0],
		.name = "Intel Cedarview (SGX545 1.0.13)",
		.core_rev = 0x0001000d,
		.clock_hz = 400000000,
		.fw_name = "powervr_sgx/sgx545_ukernel.bin",
	},
};

MODULE_FIRMWARE("powervr_sgx/sgx545_ukernel.bin");

static bool heaps_fit(const struct pvrsgx_core_family *f)
{
	const struct pvrsgx_heap_layout *h = f->heaps;
	const struct pvrsgx_va_range *r[] = {
		&h->params_3d, &h->ta_data, &h->sync, &h->pds_pixel, &h->kernel_code,
		&h->pds_vertex, &h->kernel_data, &h->usse_pixel, &h->usse_vertex,
		&h->general, &h->texture,
	};

	for (unsigned int i = 0; i < ARRAY_SIZE(r); i++)
		if (r[i]->base >= r[i]->end || r[i]->end > BIT_ULL(f->va_bits))
			return false;
	return true;
}

/*
 * Whether the driver can run the core, not only identify it: either a
 * directory list per VM, or exactly one; a heap layout within its address
 * space; the event PDS registers that enter the microkernel; and the second
 * kick register exactly when the core has one.
 */
bool pvrsgx_core_family_usable(const struct pvrsgx_core_family *f)
{
	const struct pvrsgx_core_regs *cr = f->regs;
	bool multi = f->features & PVRSGX_FEATURE_MULTIPLE_MEM_CONTEXTS;
	bool kick2 = f->features & PVRSGX_FEATURE_MULTI_EVENT_KICK;

	if (multi ? f->num_dir_lists < 2 || f->num_dir_lists > SGX_BIF_MAX_DIRLISTS :
		    f->num_dir_lists != 1)
		return false;
	if ((f->va_bits != 28 && f->va_bits != 32) || !f->heaps || !heaps_fit(f))
		return false;
	return cr->event_pds_enable && cr->event_pds_base && kick2 == !!cr->event_kick2;
}

/*
 * The core the bus says is there (545 for an SGX545), or NULL if the driver
 * doesn't know where its registers are.
 */
const struct pvrsgx_core_family *pvrsgx_core_family_get(unsigned int core)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(families); i++)
		if (families[i].core == core)
			return &families[i];
	return NULL;
}

/* The entry for this part, or NULL if the driver doesn't know it. */
const struct pvrsgx_core_info *pvrsgx_core_lookup(const struct pvrsgx_core_family *family,
						  u32 core_id, u32 core_rev)
{
	if (core_id != family->core_id)
		return NULL;
	for (unsigned int i = 0; i < ARRAY_SIZE(cores); i++)
		if (cores[i].family == family && cores[i].core_rev == core_rev)
			return &cores[i];
	return NULL;
}

#if IS_ENABLED(CONFIG_DRM_POWERVR_SGX_KUNIT_TEST)
#include "tests/pvrsgx_core_test.c"
#endif
