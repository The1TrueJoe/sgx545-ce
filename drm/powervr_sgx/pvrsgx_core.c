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

#include "pvrsgx_cmd_defs.h"
#include "pvrsgx_core.h"
#include "pvrsgx_fw.h"
#include "pvrsgx_hw.h"

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
	.use_code_base_addr_shift = 7,
	.vendor_b30 = 0x0b30,
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

static const struct pvrsgx_core_family families[] = {
	{
		.name = "SGX545",
		.core = 545,
		.core_id = 0x01150000,
		.regs = &sgx545_regs,
		.features = PVRSGX_FEATURE_MULTIPLE_MEM_CONTEXTS |
			    PVRSGX_FEATURE_MULTI_EVENT_KICK |
			    PVRSGX_FEATURE_PDS_DATA_INTERLEAVE_2DWORDS |
			    PVRSGX_FEATURE_EDM_VERTEX_PDSADDR_FULL_RANGE,
		.isa = PVRSGX_ISA_SGX545,
		.num_dir_lists = 16,
		.va_bits = 32,
		.heaps = &sgx545_heaps,
		.cmds = &pvrsgx_cmd_core_sgx545,
		.init_regs = sgx545_init_regs,
		.num_init_regs = ARRAY_SIZE(sgx545_init_regs),
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
		 * The same core one revision earlier: same features and instruction set, the same
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

static bool family_usable(const struct pvrsgx_core_family *f)
{
	return (f->features & PVRSGX_FEATURES_REQUIRED) == PVRSGX_FEATURES_REQUIRED &&
	       f->va_bits >= PVRSGX_VA_BITS_REQUIRED && f->num_dir_lists >= 2 &&
	       f->num_dir_lists <= SGX_BIF_MAX_DIRLISTS;
}

/* The core the bus says is there (545 for an SGX545), or NULL if the driver can't drive it. */
const struct pvrsgx_core_family *pvrsgx_core_family_get(unsigned int core)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(families); i++)
		if (families[i].core == core && family_usable(&families[i]))
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
