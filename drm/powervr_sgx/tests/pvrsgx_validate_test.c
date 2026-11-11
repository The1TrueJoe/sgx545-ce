// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/* KUnit tests of the USSE validator, included from pvrsgx_validate.c. */

#include <kunit/test.h>

#define W(hi, lo)	((u64)(hi) << 32 | (lo))

/* Each word, alone at instruction 2 of a 512-instruction object placed at 0x100. */
static const struct {
	u64 w;
	const char *what;
	bool ok;
} validate_words[] = {
	{ W(0xfe210000, 0x2000001c), "ldr r0, #28", false },
	{ W(0xfe290000, 0x20064080), "str #3200, r1", false },
	{ W(0xf80000c0, 0x00000180), "setl r3", false },
	{ W(0xf8000100, 0x00a00000), "savl r5", false },
	{ W(0xf8000000, 0x00001077), "ba #4215", false },
	{ W(0xf8000080, 0x00000000), "lapc", false },
	{ W(0xf8000240, 0x00000003), "br #3 [link]", false },
	{ W(0xf8000040, 0x0003fffe), "br #-2", true },
	{ W(0xf8000040, 0x0003fffd), "br #-3", false },
	{ W(0xf8000040, 0x000001fd), "br #509", true },
	{ W(0xf8000040, 0x000001fe), "br #510", false },
	{ W(0xfa440700, 0x00000000), "phas.i #0, #0 [wait=7]", true },
	{ W(0xfa440000, 0x00000102), "phas.i #0x102 (in the object)", true },
	{ W(0xfa440000, 0x000000ff), "phas.i #0xff (before it)", false },
	{ W(0xfa440000, 0x00000300), "phas.i #0x300 (past it)", false },
	{ W(0xfd200000, 0x00000010), "lock #1", false },
	{ W(0xfb100010, 0x02003004), "smbo #1, #2, #3, #4", false },
	{ W(0xdc2c23c5, 0xa0008001), "fdual.fssq.mov", false },
	{ W(0xff200000, 0x00000001), "op0xd6 #1", false },
	{ W(0xff200000, 0x00000000), "op0xd6 #0", true },
	{ W(0x00080001, 0x08a04103), "fmad g5, r1, r2, r3", false },
	{ W(0x28090001, 0x20600000), "mov c3, r0", false },
	{ W(0x28030000, 0x60003900), "mov r0, g50", false },
	{ W(0x28030000, 0x60002a00), "mov r0, g20", true },
	{ W(0x28030000, 0xe0000200), "mov r0, i4", false },
	{ W(0x28090003, 0x20800000), "mov i4, r0", false },
	{ W(0x28a33001, 0x20003000), "mov with an indexed source", true },
	{ W(0x00000000, 0xffffffff), "fmad with every operand bit set", true },
	{ W(0x00000000, 0x00000000), "zero", true },
	{ W(0xffffffff, 0xffffffff), "all ones", false },
};

static void validate_test_words(struct kunit *test)
{
	for (int i = 0; i < ARRAY_SIZE(validate_words); i++) {
		const u64 *w = &validate_words[i].w;
		bool abs = false;
		s64 bad = usse_scan(PVRSGX_ISA_SGX545, w, 1, 2, 512, 0x100, &abs);

		KUNIT_EXPECT_EQ_MSG(test, bad, validate_words[i].ok ? -1 : 0, "%s",
				    validate_words[i].what);
	}
}

/* phas.i with a target needs a place, and says it had one. */
static void validate_test_phas_place(struct kunit *test)
{
	u64 w = W(0xfa440000, 0x00000102);
	bool abs = false;

	KUNIT_EXPECT_EQ(test, usse_scan(PVRSGX_ISA_SGX545, &w, 1, 0, 512, CODE_PLACE_NONE, &abs),
			0);
	KUNIT_EXPECT_EQ(test, usse_scan(PVRSGX_ISA_SGX545, &w, 1, 0, 512, 0x100, &abs), -1);
	KUNIT_EXPECT_TRUE(test, abs);
	KUNIT_EXPECT_EQ(test, code_place(PVRSGX_HEAP_USSE_PIXEL + 0x800, 0), 0x100);
	KUNIT_EXPECT_EQ(test, code_place(PVRSGX_HEAP_USSE_VERTEX + 0x1000, 0x800), 0x100);
}

/* Every slot set holds slots the checker knows, with pieces that fit a word. */
static void validate_test_table(struct kunit *test)
{
	for (int i = 0; i < ARRAY_SIZE(pvrsgx_usse_forms); i++)
		KUNIT_EXPECT_LT(test, pvrsgx_usse_forms[i].slots, ARRAY_SIZE(pvrsgx_usse_slots));
	for (int i = 0; i < ARRAY_SIZE(pvrsgx_usse_slots); i++) {
		for (int k = 0; k < USSE_MAX_SLOTS; k++) {
			const struct pvrsgx_usse_slot *s = &pvrsgx_usse_slots[i][k];

			KUNIT_EXPECT_LE(test, s->kind, USSE_SLOT_SRC0);
			for (int p = 0; p < USSE_MAX_PIECES; p++)
				KUNIT_EXPECT_LE(test, s->p[p].lo + s->p[p].width, 64);
		}
	}
}

static struct kunit_case pvrsgx_validate_test_cases[] = {
	KUNIT_CASE(validate_test_words),
	KUNIT_CASE(validate_test_phas_place),
	KUNIT_CASE(validate_test_table),
	{}
};

static struct kunit_suite pvrsgx_validate_test_suite = {
	.name = "powervr_sgx_validate",
	.test_cases = pvrsgx_validate_test_cases,
};

kunit_test_suite(pvrsgx_validate_test_suite);
