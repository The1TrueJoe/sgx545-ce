// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * KUnit tests of the writes to registers a core may lack, included from
 * pvrsgx_fw.c. Registers are a memory buffer filled with a pattern; a core
 * table entry of 0 must leave offset 0 (CLKGATECTL) and the absent registers
 * alone.
 */

#include <kunit/test.h>

#define REGS_PATTERN	0xa5a5a5a5u

struct regs_fixture {
	struct pvrsgx_device *sdev;
	u32 *regs;
};

static u32 reg(struct regs_fixture *fx, u32 off)
{
	return fx->regs[off / 4];
}

static void regs_fixture_init(struct kunit *test, struct regs_fixture *fx, unsigned int core)
{
	struct pvrsgx_device *sdev = kunit_kzalloc(test, sizeof(*sdev), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, sdev);
	fx->regs = kunit_kmalloc_array(test, PVRSGX_REGS_MIN / 4, sizeof(u32), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fx->regs);
	memset32(fx->regs, REGS_PATTERN, PVRSGX_REGS_MIN / 4);

	sdev->family = pvrsgx_core_family_get(core);
	KUNIT_ASSERT_NOT_NULL(test, sdev->family);
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

	regs_fixture_init(test, &fx, 545);
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

	regs_fixture_init(test, &fx, 530);
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

static struct kunit_case pvrsgx_regs_test_cases[] = {
	KUNIT_CASE(regs_test_sgx545),
	KUNIT_CASE(regs_test_sgx530),
	{}
};

static struct kunit_suite pvrsgx_regs_test_suite = {
	.name = "powervr_sgx_regs",
	.test_cases = pvrsgx_regs_test_cases,
};

kunit_test_suite(pvrsgx_regs_test_suite);
