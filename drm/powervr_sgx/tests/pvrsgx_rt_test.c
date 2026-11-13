// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * KUnit tests of a render target's tiling numbers, included from
 * pvrsgx_render.c: 720x480 at 1 and 4 samples, against the numbers that
 * render that window correctly.
 */

#include <kunit/test.h>

static void rt_test_720x480_1x(struct kunit *test)
{
	struct pvrsgx_rt_numbers n;

	rt_numbers(&n, 720, 480, 1);
	KUNIT_EXPECT_EQ(test, n.te_aa, 0x20000000);
	KUNIT_EXPECT_EQ(test, n.mtile1, 0x82010018);
	KUNIT_EXPECT_EQ(test, n.mtile2, 0x02010018);
	KUNIT_EXPECT_EQ(test, n.te_screen, 0x0001d016);
	KUNIT_EXPECT_EQ(test, n.mt_tiles, 0x40);
	KUNIT_EXPECT_EQ(test, n.rt_flags, 0x88);
	KUNIT_EXPECT_EQ(test, n.padx, 0x20);
	KUNIT_EXPECT_EQ(test, n.rt_mt_tiles >> 4, 4);
	KUNIT_EXPECT_EQ(test, n.rt_mt_tiles * 0xc, 0x300);
	KUNIT_EXPECT_EQ(test, n.region_bytes, 0x3000);
	KUNIT_EXPECT_EQ(test, n.tailptr_bytes, 0x1000);
	/* ZLS control 0x001710b4 with the client's bits 0x00171004 */
	KUNIT_EXPECT_EQ(test, (n.zls_xt - 1) << 3, 0xb0);
}

static void rt_test_720x480_4x(struct kunit *test)
{
	struct pvrsgx_rt_numbers n;

	rt_numbers(&n, 720, 480, 4);
	KUNIT_EXPECT_EQ(test, n.te_aa, 0xe0000000);
	KUNIT_EXPECT_EQ(test, n.mtile1, 0x8180c012);
	KUNIT_EXPECT_EQ(test, n.mtile2, 0x02010018);
	KUNIT_EXPECT_EQ(test, n.te_screen, 0x0001d016);
	KUNIT_EXPECT_EQ(test, n.mt_tiles, 0x30);
	KUNIT_EXPECT_EQ(test, n.rt_flags, 0xeaa26e26);
	KUNIT_EXPECT_EQ(test, n.padx, 0x30);
	KUNIT_EXPECT_EQ(test, n.rt_mt_tiles >> 4, 0xc);
	KUNIT_EXPECT_EQ(test, n.rt_mt_tiles * 0xc, 0x900);
	KUNIT_EXPECT_EQ(test, n.region_bytes, 0x9000);
	KUNIT_EXPECT_EQ(test, n.tailptr_bytes, 0x4000);
	/* ZLS control 0x0017116c */
	KUNIT_EXPECT_EQ(test, (n.zls_xt - 1) << 3, 0x168);
}

#if IS_ENABLED(CONFIG_DEBUG_FS)
/* debugfs tracking: a swap takes a reference and hands the old one back to be put. */
static void rt_test_dbg_track(struct kunit *test)
{
	struct pvrsgx_rt_dataset *a = kunit_kzalloc(test, sizeof(*a), GFP_KERNEL);
	struct pvrsgx_rt_dataset *b = kunit_kzalloc(test, sizeof(*b), GFP_KERNEL);
	struct pvrsgx_render_dbg d = {};

	KUNIT_ASSERT_NOT_NULL(test, a);
	KUNIT_ASSERT_NOT_NULL(test, b);
	spin_lock_init(&d.lock);
	kref_init(&a->ref);
	kref_init(&b->ref);
	KUNIT_EXPECT_NULL(test, dbg_track(&d, a));
	KUNIT_EXPECT_EQ(test, kref_read(&a->ref), 2);
	dbg_kicked(&d, a, 0x2c0, 1);
	KUNIT_EXPECT_PTR_EQ(test, d.kicked, (const void *)a);
	/* the same dataset again: one more reference, its kick kept */
	KUNIT_EXPECT_PTR_EQ(test, dbg_track(&d, a), a);
	KUNIT_EXPECT_EQ(test, kref_read(&a->ref), 3);
	KUNIT_EXPECT_PTR_EQ(test, d.kicked, (const void *)a);
	/* another dataset: the old one comes back, the kick is forgotten */
	KUNIT_EXPECT_PTR_EQ(test, dbg_track(&d, b), a);
	KUNIT_EXPECT_EQ(test, kref_read(&b->ref), 2);
	KUNIT_EXPECT_NULL(test, d.kicked);
	KUNIT_EXPECT_PTR_EQ(test, d.rtds, b);
}
#endif

static struct kunit_case pvrsgx_rt_test_cases[] = {
	KUNIT_CASE(rt_test_720x480_1x),
	KUNIT_CASE(rt_test_720x480_4x),
#if IS_ENABLED(CONFIG_DEBUG_FS)
	KUNIT_CASE(rt_test_dbg_track),
#endif
	{}
};

static struct kunit_suite pvrsgx_rt_test_suite = {
	.name = "powervr_sgx_rt",
	.test_cases = pvrsgx_rt_test_cases,
};

kunit_test_suite(pvrsgx_rt_test_suite);
