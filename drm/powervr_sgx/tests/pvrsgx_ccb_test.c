// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * KUnit tests of the kernel CCB's cache control, included from
 * pvrsgx_ccb.c. The ring, its control words and the registers are memory;
 * no microkernel takes commands, so the read offset never moves.
 */

#include <kunit/test.h>

#include "../pvrsgx_drv.h"

/* A device on the SGX545's table, with quirks replaced. */
static struct pvrsgx_device *ccb_test_device(struct kunit *test, u32 quirks)
{
	struct pvrsgx_device *sdev = kunit_kzalloc(test, sizeof(*sdev), GFP_KERNEL);
	struct pvrsgx_core_family *family = kunit_kzalloc(test, sizeof(*family), GFP_KERNEL);
	void *regs = kunit_kzalloc(test, PVRSGX_REGS_MIN, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, sdev);
	KUNIT_ASSERT_NOT_NULL(test, family);
	KUNIT_ASSERT_NOT_NULL(test, regs);
	KUNIT_ASSERT_NOT_NULL(test, pvrsgx_core_family_get(545));
	*family = *pvrsgx_core_family_get(545);
	family->quirks = quirks;
	sdev->family = family;
	sdev->cr = family->regs;
	sdev->regs = (void __iomem __force *)regs;
	sdev->fw.cpu[PVRSGX_FW_CCB] = kunit_kzalloc(test, PVRSGX_CCB_SLOTS *
						    PVRSGX_CCB_CMD_WORDS * 4, GFP_KERNEL);
	sdev->fw.cpu[PVRSGX_FW_CCBCTL] = kunit_kzalloc(test, 8, GFP_KERNEL);
	sdev->fw.cpu[PVRSGX_FW_KICKER] = kunit_kzalloc(test, 4, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, sdev->fw.cpu[PVRSGX_FW_CCB]);
	KUNIT_ASSERT_NOT_NULL(test, sdev->fw.cpu[PVRSGX_FW_CCBCTL]);
	KUNIT_ASSERT_NOT_NULL(test, sdev->fw.cpu[PVRSGX_FW_KICKER]);
	mutex_init(&sdev->ccb_lock);
	spin_lock_init(&sdev->ccb_spin);
	return sdev;
}

static const u32 *ccb_test_cmd(struct pvrsgx_device *sdev, unsigned int slot)
{
	return (u32 *)sdev->fw.cpu[PVRSGX_FW_CCB] + slot * PVRSGX_CCB_CMD_WORDS;
}

/* Without the quirk (the SGX545) a BIF and a data invalidate go together, as before. */
static void ccb_test_inval_together(struct kunit *test)
{
	struct pvrsgx_device *sdev = ccb_test_device(test, 0);
	static const u32 data[6] = { 1, 2, 3, 4, 5, 6 };
	u64 seq;

	sdev->fw.cache_control = PVRSGX_CC_INVAL_BIF_PD | PVRSGX_CC_INVAL_DATA;
	KUNIT_ASSERT_EQ(test, pvrsgx_ccb_send(sdev, PVRSGX_KCMD_TA, data, &seq), 0);
	KUNIT_EXPECT_EQ(test, seq, 1);
	KUNIT_EXPECT_EQ(test, ccb_test_cmd(sdev, 0)[0], PVRSGX_KCMD_TA);
	KUNIT_EXPECT_EQ(test, ccb_test_cmd(sdev, 0)[1],
			PVRSGX_CC_INVAL_BIF_PD | PVRSGX_CC_INVAL_DATA);
	KUNIT_EXPECT_EQ(test, ccb_test_cmd(sdev, 0)[2], 1);
	KUNIT_EXPECT_EQ(test, sdev->fw.cache_control, 0);
}

/*
 * BRN_28889: the BIF part goes alone in a PROCESS_QUEUES, and the command
 * with the data part waits until the microkernel has taken it. Nothing takes
 * it here, so the send gives up, bounded, with the job's command unwritten
 * and the invalidates kept for the next try.
 */
static void ccb_test_inval_split(struct kunit *test)
{
	struct pvrsgx_device *sdev = ccb_test_device(test, PVRSGX_QUIRK_BRN_28889);
	static const u32 data[6] = { 1, 2, 3, 4, 5, 6 };
	u64 seq = 0;

	sdev->fw.cache_control = PVRSGX_CC_INVAL_BIF_PD | PVRSGX_CC_INVAL_DATA;
	KUNIT_EXPECT_EQ(test, pvrsgx_ccb_send(sdev, PVRSGX_KCMD_TA, data, &seq), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, ccb_test_cmd(sdev, 0)[0], PVRSGX_KCMD_PROCESS_QUEUES);
	KUNIT_EXPECT_EQ(test, ccb_test_cmd(sdev, 0)[1], PVRSGX_CC_INVAL_BIF_PD);
	KUNIT_EXPECT_EQ(test, ccb_test_cmd(sdev, 0)[2], 0);
	KUNIT_EXPECT_EQ(test, ccb_test_cmd(sdev, 1)[0], 0);
	KUNIT_EXPECT_EQ(test, seq, 0);
	KUNIT_EXPECT_EQ(test, sdev->fw.cache_control,
			PVRSGX_CC_INVAL_BIF_PD | PVRSGX_CC_INVAL_DATA);
}

/* One kind of invalidate alone needs no split, with the quirk or without. */
static void ccb_test_inval_alone(struct kunit *test)
{
	struct pvrsgx_device *sdev = ccb_test_device(test, PVRSGX_QUIRK_BRN_28889);
	static const u32 data[6];

	sdev->fw.cache_control = PVRSGX_CC_INVAL_DATA;
	KUNIT_ASSERT_EQ(test, pvrsgx_ccb_send(sdev, PVRSGX_KCMD_TA, data, NULL), 0);
	KUNIT_EXPECT_EQ(test, ccb_test_cmd(sdev, 0)[0], PVRSGX_KCMD_TA);
	KUNIT_EXPECT_EQ(test, ccb_test_cmd(sdev, 0)[1], PVRSGX_CC_INVAL_DATA);
}

static struct kunit_case pvrsgx_ccb_test_cases[] = {
	KUNIT_CASE(ccb_test_inval_together),
	KUNIT_CASE(ccb_test_inval_split),
	KUNIT_CASE(ccb_test_inval_alone),
	{}
};

static struct kunit_suite pvrsgx_ccb_test_suite = {
	.name = "powervr_sgx_ccb",
	.test_cases = pvrsgx_ccb_test_cases,
};

kunit_test_suite(pvrsgx_ccb_test_suite);
