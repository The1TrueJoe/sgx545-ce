// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/* KUnit tests of code placement, included from pvrsgx_validate.c. */

#include <kunit/test.h>

/* Where a mapping puts a code object, in instructions from its heap's base. */
static void validate_test_code_place(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, code_place(PVRSGX_HEAP_USSE_PIXEL + 0x800, 0), 0x100);
	KUNIT_EXPECT_EQ(test, code_place(PVRSGX_HEAP_USSE_VERTEX + 0x1000, 0x800), 0x100);
	/* From the window, not the heap: window 1 starts at +512 KiB. */
	KUNIT_EXPECT_EQ(test, code_place(PVRSGX_HEAP_USSE_PIXEL + 0x80800, 0), 0x100);
	KUNIT_EXPECT_EQ(test, code_place(PVRSGX_HEAP_USSE_PIXEL + 0x80800, 0x1000), 0xff00);
	KUNIT_EXPECT_EQ(test, code_place(PVRSGX_HEAP_USSE_PIXEL + 0x400, 0x800),
			PVRSGX_USSE_PLACE_NONE);
}

static struct kunit_case pvrsgx_validate_test_cases[] = {
	KUNIT_CASE(validate_test_code_place),
	{}
};

static struct kunit_suite pvrsgx_validate_test_suite = {
	.name = "powervr_sgx_validate",
	.test_cases = pvrsgx_validate_test_cases,
};

kunit_test_suite(pvrsgx_validate_test_suite);
