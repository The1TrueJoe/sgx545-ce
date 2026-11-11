// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/* KUnit tests of the core table, included from pvrsgx_core.c. */

#include <kunit/test.h>

static void core_test_lookup(struct kunit *test)
{
	const struct pvrsgx_core_family *f = pvrsgx_core_family_get(545);

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_EXPECT_STREQ(test, pvrsgx_core_lookup(f, 0x01150000, 0x0001000e)->name,
			   "Intel CE5300 (SGX545 1.0.14)");
	KUNIT_EXPECT_STREQ(test, pvrsgx_core_lookup(f, 0x01150000, 0x0001000d)->name,
			   "Intel Cedarview (SGX545 1.0.13)");
	KUNIT_EXPECT_NULL(test, pvrsgx_core_lookup(f, 0x01150000, 0x0001000c));
	KUNIT_EXPECT_NULL(test, pvrsgx_core_lookup(f, 0x01120000, 0x0001000e));
}

static void core_test_unknown(struct kunit *test)
{
	KUNIT_EXPECT_NULL(test, pvrsgx_core_family_get(0));
	KUNIT_EXPECT_NULL(test, pvrsgx_core_family_get(535));
	KUNIT_EXPECT_NULL(test, pvrsgx_core_family_get(544));
}

/* A core the driver only identifies: listed, with its ID registers, not run. */
static void core_test_identify_only(struct kunit *test)
{
	const struct pvrsgx_core_family *f = pvrsgx_core_family_get(530);

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_EXPECT_FALSE(test, pvrsgx_core_family_usable(f));
	KUNIT_EXPECT_EQ(test, f->regs->core_id, 0x10);
	KUNIT_EXPECT_EQ(test, f->regs->core_revision, 0x14);
	KUNIT_EXPECT_TRUE(test, pvrsgx_core_family_usable(pvrsgx_core_family_get(545)));
}

/*
 * Every entry has the registers the driver reads first; every one it can
 * drive has the rest, and a part.
 */
static void core_test_table(struct kunit *test)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(families); i++) {
		const struct pvrsgx_core_family *f = &families[i];

		KUNIT_EXPECT_NE(test, f->regs->core_id, 0);
		KUNIT_EXPECT_NE(test, f->regs->core_revision, 0);
		if (!pvrsgx_core_family_usable(f))
			continue;
		KUNIT_EXPECT_NE(test, f->regs->event_kick2, 0);
		KUNIT_EXPECT_NE(test, f->regs->use_code_base0, 0);
		KUNIT_EXPECT_GT(test, f->num_init_regs, 0);
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(cores); i++) {
		KUNIT_EXPECT_NOT_NULL(test, cores[i].family);
		KUNIT_EXPECT_TRUE(test, pvrsgx_core_family_usable(cores[i].family));
		KUNIT_EXPECT_NOT_NULL(test, cores[i].fw_name);
		KUNIT_EXPECT_GT(test, cores[i].clock_hz, 0);
	}
}

/* The SGX545's layout is the fixed one of docs/UAPI.md section 5; the SGX530's fits 28 bits. */
static void core_test_heaps(struct kunit *test)
{
	const struct pvrsgx_core_family *f545 = pvrsgx_core_family_get(545);
	const struct pvrsgx_core_family *f530 = pvrsgx_core_family_get(530);

	KUNIT_ASSERT_NOT_NULL(test, f545);
	KUNIT_ASSERT_NOT_NULL(test, f530);
	KUNIT_EXPECT_FALSE(test, pvrsgx_core_single_context(f545));
	KUNIT_EXPECT_EQ(test, f545->heaps->kernel_code.base, PVRSGX_HEAP_KERNEL_CODE);
	KUNIT_EXPECT_EQ(test, f545->heaps->kernel_data.base, PVRSGX_HEAP_KERNEL_DATA);
	KUNIT_EXPECT_EQ(test, f545->heaps->pds_pixel.base, PVRSGX_HEAP_PDS_PIXEL);
	KUNIT_EXPECT_EQ(test, f545->heaps->params_3d.base, PVRSGX_HEAP_3D_PARAMS);
	KUNIT_EXPECT_EQ(test, f545->heaps->texture.end, SZ_4G);
	KUNIT_EXPECT_EQ(test, f545->heaps->pb_max_pages, 0x8000);
	KUNIT_EXPECT_EQ(test, f545->quirks, 0);

	KUNIT_EXPECT_TRUE(test, pvrsgx_core_single_context(f530));
	KUNIT_EXPECT_EQ(test, f530->num_dir_lists, 1);
	KUNIT_EXPECT_EQ(test, f530->va_bits, 28);
	KUNIT_EXPECT_TRUE(test, heaps_fit(f530));
	KUNIT_EXPECT_TRUE(test, f530->quirks & PVRSGX_QUIRK_BRN_28889);
	/* The shared kernel heaps are whole 4 MiB directory entries. */
	KUNIT_EXPECT_EQ(test, f530->heaps->kernel_code.base % SZ_4M, 0);
	KUNIT_EXPECT_EQ(test, f530->heaps->kernel_data.end % SZ_4M, 0);
	KUNIT_EXPECT_EQ(test, f530->heaps->sync.base % SZ_4M, 0);
	KUNIT_EXPECT_EQ(test, f530->regs->bif_bank0, 0);
	KUNIT_EXPECT_EQ(test, f530->regs->clkgatectl2, 0);
	KUNIT_EXPECT_EQ(test, f530->regs->event_kick2, 0);
	/* No register the SGX530's header doesn't name: the vendor script's 0xb30 is the 545's. */
	KUNIT_EXPECT_EQ(test, f530->regs->vendor_b30, 0);
	KUNIT_EXPECT_EQ(test, f545->regs->vendor_b30, 0xb30);
}

static struct kunit_case pvrsgx_core_test_cases[] = {
	KUNIT_CASE(core_test_lookup),
	KUNIT_CASE(core_test_unknown),
	KUNIT_CASE(core_test_identify_only),
	KUNIT_CASE(core_test_heaps),
	KUNIT_CASE(core_test_table),
	{}
};

static struct kunit_suite pvrsgx_core_test_suite = {
	.name = "powervr_sgx_core",
	.test_cases = pvrsgx_core_test_cases,
};

kunit_test_suite(pvrsgx_core_test_suite);
