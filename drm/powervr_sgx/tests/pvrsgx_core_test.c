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

/* Every entry is one the driver can drive, with the registers it reads first. */
static void core_test_table(struct kunit *test)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(families); i++) {
		const struct pvrsgx_core_family *f = &families[i];

		KUNIT_EXPECT_TRUE(test, pvrsgx_core_family_usable(f));
		KUNIT_EXPECT_NE(test, f->regs->core_id, 0);
		KUNIT_EXPECT_NE(test, f->regs->core_revision, 0);
		KUNIT_EXPECT_EQ(test, !!(f->features & PVRSGX_FEATURE_MULTI_EVENT_KICK),
				!!f->regs->event_kick2);
		KUNIT_EXPECT_NE(test, f->regs->use_code_base0, 0);
		KUNIT_EXPECT_GT(test, f->num_init_regs, 0);
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(cores); i++) {
		KUNIT_EXPECT_NOT_NULL(test, cores[i].family);
		KUNIT_EXPECT_NOT_NULL(test, cores[i].fw_name);
		KUNIT_EXPECT_GT(test, cores[i].clock_hz, 0);
	}
}

/*
 * The SGX545's table holds what the driver used before the table had it:
 * the heap constants, the shifts and the registers.
 */
static void core_test_sgx545_values(struct kunit *test)
{
	const struct pvrsgx_core_family *f = pvrsgx_core_family_get(545);
	const struct pvrsgx_heap_layout *h;

	KUNIT_ASSERT_NOT_NULL(test, f);
	h = f->heaps;
	KUNIT_ASSERT_NOT_NULL(test, h);
	KUNIT_EXPECT_EQ(test, h->params_3d.base, PVRSGX_HEAP_3D_PARAMS);
	KUNIT_EXPECT_EQ(test, h->ta_data.base, PVRSGX_HEAP_TA_DATA);
	KUNIT_EXPECT_EQ(test, h->sync.base, PVRSGX_HEAP_SYNC);
	KUNIT_EXPECT_EQ(test, h->pds_pixel.base, PVRSGX_HEAP_PDS_PIXEL);
	KUNIT_EXPECT_EQ(test, h->kernel_code.base, PVRSGX_HEAP_KERNEL_CODE);
	KUNIT_EXPECT_EQ(test, h->pds_vertex.base, PVRSGX_HEAP_PDS_VERTEX);
	KUNIT_EXPECT_EQ(test, h->kernel_data.base, PVRSGX_HEAP_KERNEL_DATA);
	KUNIT_EXPECT_EQ(test, h->usse_pixel.base, PVRSGX_HEAP_USSE_PIXEL);
	KUNIT_EXPECT_EQ(test, h->usse_pixel.end - h->usse_pixel.base, PVRSGX_HEAP_USSE_PIXEL_SIZE);
	KUNIT_EXPECT_EQ(test, h->usse_vertex.base, PVRSGX_HEAP_USSE_VERTEX);
	KUNIT_EXPECT_EQ(test, h->usse_vertex.end - h->usse_vertex.base,
			PVRSGX_HEAP_USSE_VERTEX_SIZE);
	KUNIT_EXPECT_EQ(test, h->general.base, PVRSGX_HEAP_GENERAL);
	KUNIT_EXPECT_EQ(test, h->texture.base, PVRSGX_HEAP_TEXTURE);
	KUNIT_EXPECT_EQ(test, h->texture.end, SZ_4G);
	KUNIT_EXPECT_EQ(test, h->pb_max_pages, 0x8000);
	KUNIT_EXPECT_EQ(test, f->quirks, 0);
	KUNIT_EXPECT_EQ(test, f->regs->bif_fault_addr, SGX_MMU_ADDR_MASK);
	KUNIT_EXPECT_EQ(test, f->regs->bif_bank_set, SGX_CR_BIF_BANK_SET);
	KUNIT_EXPECT_EQ(test, f->regs->bif_bank0, SGX_CR_BIF_BANK0);
	KUNIT_EXPECT_EQ(test, f->regs->use_code_base_addr_shift, 7);
	KUNIT_EXPECT_EQ(test, f->regs->use_code_base_dm_shift, 25);
	KUNIT_EXPECT_EQ(test, f->regs->cr_b30, 0x0b30);
}

/* The SGX530: one directory list, 28 bits, BRN_28889, a layout the EDM reaches. */
static void core_test_sgx530(struct kunit *test)
{
	const struct pvrsgx_core_family *f = pvrsgx_core_family_get(530);

	KUNIT_ASSERT_NOT_NULL(test, f);
	KUNIT_EXPECT_TRUE(test, pvrsgx_core_single_context(f));
	KUNIT_EXPECT_EQ(test, f->num_dir_lists, 1);
	KUNIT_EXPECT_EQ(test, f->va_bits, 28);
	KUNIT_EXPECT_EQ(test, f->quirks, PVRSGX_QUIRK_BRN_28889);
	KUNIT_EXPECT_EQ(test, f->regs->event_kick2, 0);
	KUNIT_EXPECT_EQ(test, f->regs->bif_bank0, 0);
	KUNIT_EXPECT_EQ(test, f->regs->clkgatectl2, 0);
	KUNIT_EXPECT_TRUE(test, pvrsgx_core_family_usable(f));
	KUNIT_EXPECT_FALSE(test, pvrsgx_core_single_context(pvrsgx_core_family_get(545)));
	KUNIT_EXPECT_STREQ(test, pvrsgx_core_lookup(f, 0x01120000, 0x00010205)->name,
			   "TI OMAP3630 (SGX530 1.2.5)");
}

static struct kunit_case pvrsgx_core_test_cases[] = {
	KUNIT_CASE(core_test_lookup),
	KUNIT_CASE(core_test_unknown),
	KUNIT_CASE(core_test_table),
	KUNIT_CASE(core_test_sgx545_values),
	KUNIT_CASE(core_test_sgx530),
	{}
};

static struct kunit_suite pvrsgx_core_test_suite = {
	.name = "powervr_sgx_core",
	.test_cases = pvrsgx_core_test_cases,
};

kunit_test_suite(pvrsgx_core_test_suite);
