// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * KUnit tests of the writes to registers a core may lack, included from
 * pvrsgx_fw.c. Registers are a memory buffer filled with a pattern; a core
 * table entry of 0 must leave offset 0 (CLKGATECTL) and the absent registers
 * alone.
 */

#include <kunit/test.h>

#include "../pvrsgx_drv.h"

#define REGS_PATTERN	0xa5a5a5a5u

struct regs_fixture {
	struct pvrsgx_device *sdev;
	u32 *regs;
};

static u32 reg(struct regs_fixture *fx, u32 off)
{
	return fx->regs[off / 4];
}

static void regs_fixture_init(struct kunit *test, struct regs_fixture *fx,
			      const struct pvrsgx_core_family *family)
{
	struct pvrsgx_device *sdev = kunit_kzalloc(test, sizeof(*sdev), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, sdev);
	fx->regs = kunit_kmalloc_array(test, PVRSGX_REGS_MIN / 4, sizeof(u32), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fx->regs);
	memset32(fx->regs, REGS_PATTERN, PVRSGX_REGS_MIN / 4);

	KUNIT_ASSERT_NOT_NULL(test, family);
	sdev->family = family;
	sdev->cr = sdev->family->regs;
	sdev->regs = (void __iomem __force *)fx->regs;
	sdev->kernel_mmu = pvrsgx_mmu_create(sdev);
	sdev->idle_mmu = pvrsgx_mmu_create(sdev);
	KUNIT_ASSERT_FALSE(test, IS_ERR(sdev->kernel_mmu));
	KUNIT_ASSERT_FALSE(test, IS_ERR(sdev->idle_mmu));
	fx->sdev = sdev;
}

static void regs_fixture_fini(struct regs_fixture *fx)
{
	pvrsgx_mmu_destroy(fx->sdev->idle_mmu);
	pvrsgx_mmu_destroy(fx->sdev->kernel_mmu);
}

/* The SGX545 writes its banks, the EDM's list 15, CLKGATECTL2 and EVENT_KICK2. */
static void regs_test_sgx545(struct kunit *test)
{
	struct regs_fixture fx;

	regs_fixture_init(test, &fx, pvrsgx_core_family_get(545));
	pvrsgx_reset(fx.sdev, false);
	KUNIT_EXPECT_EQ(test, reg(&fx, SGX_CR_BIF_BANK_SET), 0);
	KUNIT_EXPECT_EQ(test, reg(&fx, SGX_CR_BIF_BANK0), 15);
	KUNIT_EXPECT_EQ(test, reg(&fx, sgx_cr_bif_dir_list_base(15)),
			pvrsgx_mmu_pd_addr(fx.sdev->kernel_mmu));
	KUNIT_EXPECT_EQ(test, reg(&fx, SGX_CR_BIF_DIR_LIST_BASE0),
			pvrsgx_mmu_pd_addr(fx.sdev->idle_mmu));

	clocks_on(fx.sdev);
	KUNIT_EXPECT_EQ(test, reg(&fx, SGX_CR_CLKGATECTL), 0xa6aa);
	KUNIT_EXPECT_EQ(test, reg(&fx, SGX_CR_CLKGATECTL2), 0xaaaaaa);

	init_kick(fx.sdev);
	KUNIT_EXPECT_EQ(test, reg(&fx, SGX_CR_EVENT_KICK2), SGX_CR_EVENT_KICK2_NOW);
	KUNIT_EXPECT_EQ(test, reg(&fx, SGX_CR_EVENT_KICK), REGS_PATTERN);
	regs_fixture_fini(&fx);
}

/*
 * The SGX530 has none of them: list 0 holds the firmware's directory, and
 * offset 0 holds only CLKGATECTL's own value.
 */
static void regs_test_sgx530(struct kunit *test)
{
	struct regs_fixture fx;

	regs_fixture_init(test, &fx, pvrsgx_core_family_get(530));
	pvrsgx_reset(fx.sdev, false);
	KUNIT_EXPECT_EQ(test, reg(&fx, SGX_CR_BIF_BANK_SET), REGS_PATTERN);
	KUNIT_EXPECT_EQ(test, reg(&fx, SGX_CR_BIF_BANK0), REGS_PATTERN);
	KUNIT_EXPECT_EQ(test, reg(&fx, SGX_CR_BIF_DIR_LIST_BASE1), REGS_PATTERN);
	KUNIT_EXPECT_EQ(test, reg(&fx, sgx_cr_bif_dir_list_base(15)), REGS_PATTERN);
	KUNIT_EXPECT_EQ(test, reg(&fx, SGX_CR_BIF_DIR_LIST_BASE0),
			pvrsgx_mmu_pd_addr(fx.sdev->kernel_mmu));
	KUNIT_EXPECT_EQ(test, reg(&fx, SGX_CR_CLKGATECTL), REGS_PATTERN);

	clocks_on(fx.sdev);
	/* 2D, ISP, TSP, TA and USE automatic, the DPM on. */
	KUNIT_EXPECT_EQ(test, reg(&fx, SGX_CR_CLKGATECTL), 0x00212222);
	KUNIT_EXPECT_EQ(test, reg(&fx, SGX_CR_CLKGATECTL2), REGS_PATTERN);

	init_kick(fx.sdev);
	KUNIT_EXPECT_EQ(test, reg(&fx, SGX_CR_EVENT_KICK), SGX_CR_EVENT_KICK_NOW);
	KUNIT_EXPECT_EQ(test, reg(&fx, SGX_CR_EVENT_KICK2), REGS_PATTERN);
	regs_fixture_fini(&fx);
}

/*
 * An SGX535-shaped core (docs/porting/cores/sgx535.md): the SGX530's low
 * registers and one kick, the SGX545's 16 directory lists and banks. Single
 * kick and single context are separate properties.
 */
static const struct pvrsgx_core_regs sgx535_like_regs = {
	.core_id = 0x0010,
	.core_revision = 0x0014,
	.clkgatectl_units = 0x00333333,
	.clkgatectl_dpm = GENMASK(17, 16),
	.clkgatestatus = 0x0004,
	.clkgatestatus_dpm = BIT(16),
	.soft_reset_bif = BIT(0),
	.soft_reset_cores = GENMASK(6, 1),
	.use_code_base0 = SGX_CR_USE_CODE_BASE_0,
	.pds_exec_base = SGX_CR_PDS_EXEC_BASE,
	.bif_int_stat_fault_req = SGX_CR_BIF_INT_STAT_FAULT_REQ,
	.bif_int_stat_fault_type = SGX_CR_BIF_INT_STAT_FAULT_TYPE,
	.bif_fault_addr = SGX_MMU_ADDR_MASK,
	.bif_bank_set = SGX_CR_BIF_BANK_SET,
	.bif_bank0 = SGX_CR_BIF_BANK0,
	.use_code_base_dm_shift = 24,
	.event_pds_enable = SGX545_CR_EVENT_PDS_ENABLE,
	.event_pds_enable_all = SGX545_CR_EVENT_PDS_ENABLE_ALL,
	.event_pds_base = SGX545_CR_EVENT_PDS_BASE,
	.event_pds_data0 = SGX545_CR_EVENT_PDS_DATA0,
	.event_pds_data1 = SGX545_CR_EVENT_PDS_DATA1,
};

/* With the SGX545's 32-bit layout. */
static struct pvrsgx_core_family sgx535_like(void)
{
	return (struct pvrsgx_core_family){
		.name = "SGX535-like",
		.core = 535,
		.regs = &sgx535_like_regs,
		.features = PVRSGX_FEATURE_MULTIPLE_MEM_CONTEXTS,
		.isa = PVRSGX_ISA_SGX545,
		.num_dir_lists = 16,
		.va_bits = 32,
		.heaps = pvrsgx_core_family_get(545)->heaps,
	};
}

static void regs_test_sgx535_like(struct kunit *test)
{
	struct pvrsgx_core_family *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	struct regs_fixture fx;

	KUNIT_ASSERT_NOT_NULL(test, f);
	*f = sgx535_like();
	KUNIT_EXPECT_TRUE(test, pvrsgx_core_family_usable(f));
	KUNIT_EXPECT_FALSE(test, pvrsgx_core_single_context(f));

	regs_fixture_init(test, &fx, f);
	/* Multi-context: the banks and the EDM's own list, as the SGX545. */
	pvrsgx_reset(fx.sdev, false);
	KUNIT_EXPECT_EQ(test, reg(&fx, SGX_CR_BIF_BANK0), 15);
	KUNIT_EXPECT_EQ(test, reg(&fx, sgx_cr_bif_dir_list_base(15)),
			pvrsgx_mmu_pd_addr(fx.sdev->kernel_mmu));
	KUNIT_EXPECT_EQ(test, reg(&fx, SGX_CR_BIF_DIR_LIST_BASE0),
			pvrsgx_mmu_pd_addr(fx.sdev->idle_mmu));
	/* Single kick and no CLKGATECTL2, as the SGX530. */
	clocks_on(fx.sdev);
	KUNIT_EXPECT_EQ(test, reg(&fx, SGX_CR_CLKGATECTL), 0x00212222);
	KUNIT_EXPECT_EQ(test, reg(&fx, SGX_CR_CLKGATECTL2), REGS_PATTERN);
	init_kick(fx.sdev);
	KUNIT_EXPECT_EQ(test, reg(&fx, SGX_CR_EVENT_KICK), SGX_CR_EVENT_KICK_NOW);
	KUNIT_EXPECT_EQ(test, reg(&fx, SGX_CR_EVENT_KICK2), REGS_PATTERN);
	regs_fixture_fini(&fx);
}

/* The usable check takes each property on its own. */
static void regs_test_usable(struct kunit *test)
{
	struct pvrsgx_core_family f = sgx535_like();

	f.features |= PVRSGX_FEATURE_MULTI_EVENT_KICK;	/* the feature without the register */
	KUNIT_EXPECT_FALSE(test, pvrsgx_core_family_usable(&f));
	f = sgx535_like();
	f.num_dir_lists = 1;				/* multi-context with one list */
	KUNIT_EXPECT_FALSE(test, pvrsgx_core_family_usable(&f));
	f = sgx535_like();
	f.va_bits = 28;					/* a 4 GiB layout in 256 MiB */
	KUNIT_EXPECT_FALSE(test, pvrsgx_core_family_usable(&f));
}

static struct kunit_case pvrsgx_regs_test_cases[] = {
	KUNIT_CASE(regs_test_sgx545),
	KUNIT_CASE(regs_test_sgx530),
	KUNIT_CASE(regs_test_sgx535_like),
	KUNIT_CASE(regs_test_usable),
	{}
};

static struct kunit_suite pvrsgx_regs_test_suite = {
	.name = "powervr_sgx_regs",
	.test_cases = pvrsgx_regs_test_cases,
};

kunit_test_suite(pvrsgx_regs_test_suite);
