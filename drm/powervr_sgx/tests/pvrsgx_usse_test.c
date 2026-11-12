// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/* KUnit tests of the USSE instruction check, included from pvrsgx_usse.c. */

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
	/* Number bit 6: a flag only where the format makes it one. */
	{ W(0x50021000, 0xc0000001), "and r0, i0, r1", true },
	{ W(0x50021000, 0xc0002001), "and r0, i64, r1", false },
	{ W(0x00420000, 0xc0006103), "fmad r0, r1, i2.f16, r3 [fmt]", true },
	{ W(0x00020000, 0xc0006103), "fmad r0, r1, i66, r3", false },
	{ W(0x9183f802, 0xf0002041), "sopwm pa0, i0.c10, i1.c10", true },
	{ W(0xfd100000, 0x00000100), "setfc #0, #1", true },
	{ W(0xfd100000, 0x00000101), "setfc #1, #1", false },
};

static void usse_test_words(struct kunit *test)
{
	for (int i = 0; i < ARRAY_SIZE(validate_words); i++) {
		const u64 *w = &validate_words[i].w;
		bool abs = false;
		s64 bad = pvrsgx_usse_scan(PVRSGX_ISA_SGX545, w, 1, 2, 512, 0x100, &abs, NULL);

		KUNIT_EXPECT_EQ_MSG(test, bad, validate_words[i].ok ? -1 : 0, "%s",
				    validate_words[i].what);
	}
}

/* phas.i with a target needs a place, and says it had one. */
static void usse_test_phas_place(struct kunit *test)
{
	u64 w = W(0xfa440000, 0x00000102);
	enum pvrsgx_usse_refusal why;
	bool abs = false;

	KUNIT_EXPECT_EQ(test, pvrsgx_usse_scan(PVRSGX_ISA_SGX545, &w, 1, 0, 512,
					       PVRSGX_USSE_PLACE_NONE, &abs, &why), 0);
	KUNIT_EXPECT_EQ(test, why, PVRSGX_USSE_REFUSAL_PHAS);
	KUNIT_EXPECT_EQ(test, pvrsgx_usse_scan(PVRSGX_ISA_SGX545, &w, 1, 0, 512, 0x100, &abs,
					       &why), -1);
	KUNIT_EXPECT_EQ(test, why, PVRSGX_USSE_REFUSAL_NONE);
	KUNIT_EXPECT_TRUE(test, abs);

	/* An object with a target must not cross into the next window. */
	w = W(0xfa440000, PVRSGX_USSE_WINDOW_INSTS - 0x100 + 2);
	KUNIT_EXPECT_EQ(test, pvrsgx_usse_scan(PVRSGX_ISA_SGX545, &w, 1, 0, 0x100,
					       PVRSGX_USSE_WINDOW_INSTS - 0x100, &abs, &why), -1);
	KUNIT_EXPECT_EQ(test, pvrsgx_usse_scan(PVRSGX_ISA_SGX545, &w, 1, 0, 0x101,
					       PVRSGX_USSE_WINDOW_INSTS - 0x100, &abs, &why), 0);
	KUNIT_EXPECT_EQ(test, why, PVRSGX_USSE_REFUSAL_PHAS);
}

/* Each refusal says why. */
static void usse_test_reasons(struct kunit *test)
{
	static const struct {
		u64 w;
		enum pvrsgx_usse_refusal why;
	} cases[] = {
		{ W(0xffffffff, 0xffffffff), PVRSGX_USSE_REFUSAL_FORM },
		{ W(0x28030000, 0x60003900), PVRSGX_USSE_REFUSAL_OPERAND },	/* mov r0, g50 */
		{ W(0xfe210000, 0x2000001c), PVRSGX_USSE_REFUSAL_KIND },	/* ldr */
		{ W(0xf8000040, 0x000001fe), PVRSGX_USSE_REFUSAL_BRANCH },	/* br #510 */
		{ W(0xfa440000, 0x000000ff), PVRSGX_USSE_REFUSAL_PHAS },	/* phas.i #0xff */
	};

	for (int i = 0; i < ARRAY_SIZE(cases); i++) {
		enum pvrsgx_usse_refusal why;
		bool abs = false;

		KUNIT_EXPECT_EQ(test, pvrsgx_usse_scan(PVRSGX_ISA_SGX545, &cases[i].w, 1, 2, 512,
						       0x100, &abs, &why), 0);
		KUNIT_EXPECT_EQ(test, why, cases[i].why);
	}
}

/*
 * A core's policy refuses each st and emit form that the table alone
 * accepts, and nothing else: each form's own match value, all operands 0.
 */
static void usse_test_policy(struct kunit *test)
{
	const u32 all = PVRSGX_USSE_NO_STORE | PVRSGX_USSE_NO_EMIT;
	unsigned int stores = 0, emits = 0;

	for (int i = 0; i < ARRAY_SIZE(pvrsgx_usse_forms); i++) {
		const struct pvrsgx_usse_form *f = &pvrsgx_usse_forms[i];
		u32 own = f->kind == USSE_STORE ? PVRSGX_USSE_NO_STORE :
			  f->kind == USSE_EMIT ? PVRSGX_USSE_NO_EMIT : 0;
		enum pvrsgx_usse_refusal why0, why;
		u64 w = f->match;
		bool abs = false;
		s64 bad0, bad;

		bad0 = pvrsgx_usse_scan_policy(PVRSGX_ISA_SGX545, 0, &w, 1, 2, 512, 0x100,
					       &abs, &why0);
		bad = pvrsgx_usse_scan_policy(PVRSGX_ISA_SGX545, all, &w, 1, 2, 512, 0x100,
					      &abs, &why);
		KUNIT_EXPECT_NE(test, why0, PVRSGX_USSE_REFUSAL_POLICY);
		if (!own || bad0 >= 0) {
			/* Refused by the table anyway, or not a form the policy names. */
			KUNIT_EXPECT_EQ(test, bad, bad0);
			KUNIT_EXPECT_EQ(test, why, why0);
			continue;
		}
		KUNIT_EXPECT_EQ(test, bad, 0);
		KUNIT_EXPECT_EQ(test, why, PVRSGX_USSE_REFUSAL_POLICY);
		/* The other flag alone leaves it alone. */
		KUNIT_EXPECT_EQ(test, pvrsgx_usse_scan_policy(PVRSGX_ISA_SGX545, all & ~own, &w, 1,
							      2, 512, 0x100, &abs, &why), -1);
		stores += own == PVRSGX_USSE_NO_STORE;
		emits += own == PVRSGX_USSE_NO_EMIT;
	}
	KUNIT_EXPECT_GT(test, stores, 0);
	KUNIT_EXPECT_GT(test, emits, 0);
}

/* Every slot set holds slots the checker knows, with pieces that fit a word. */
static void usse_test_table(struct kunit *test)
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

static struct kunit_case pvrsgx_usse_test_cases[] = {
	KUNIT_CASE(usse_test_words),
	KUNIT_CASE(usse_test_phas_place),
	KUNIT_CASE(usse_test_reasons),
	KUNIT_CASE(usse_test_table),
	KUNIT_CASE(usse_test_policy),
	{}
};

static struct kunit_suite pvrsgx_usse_test_suite = {
	.name = "powervr_sgx_usse",
	.test_cases = pvrsgx_usse_test_cases,
};

kunit_test_suite(pvrsgx_usse_test_suite);
