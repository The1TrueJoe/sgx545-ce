// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * KUnit tests of the microkernel file's checks,
 * included from pvrsgx_fw.c so they reach its static functions.
 */

#include <kunit/test.h>

#define T_IMAGE		256
#define T_ENTRIES	2
#define T_PATCHES	2
#define T_SIZE		(UKFW_HEADER_MIN + 8 * (T_ENTRIES + T_PATCHES) + T_IMAGE)

struct ukfw_test_file {
	struct ukfw_header h;
	struct ukfw_pair entries[T_ENTRIES];
	struct ukfw_pair patches[T_PATCHES];
	u8 image[T_IMAGE];
};

static_assert(sizeof(struct ukfw_header) == UKFW_HEADER_MIN);
static_assert(sizeof(struct ukfw_test_file) == T_SIZE);

/* A good file: event entry at 0, two patches, the image a counting pattern. */
static struct ukfw_test_file *ukfw_good(struct kunit *test)
{
	struct ukfw_test_file *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, t);
	for (u32 i = 0; i < T_IMAGE; i++)
		t->image[i] = i;
	t->h.magic[0] = cpu_to_le32(UKFW_MAGIC0);
	t->h.magic[1] = cpu_to_le32(UKFW_MAGIC1);
	t->h.header_size = cpu_to_le32(offsetof(struct ukfw_test_file, image));
	t->h.interface = cpu_to_le32(1 << 16);
	t->h.features = cpu_to_le32(PVRSGX_UKFW_CLEANUP_V2);
	t->h.image_size = cpu_to_le32(T_IMAGE);
	t->h.n_entries = cpu_to_le32(T_ENTRIES);
	t->h.entries_off = cpu_to_le32(offsetof(struct ukfw_test_file, entries));
	t->h.n_patches = cpu_to_le32(T_PATCHES);
	t->h.patches_off = cpu_to_le32(offsetof(struct ukfw_test_file, patches));
	t->entries[0] = (struct ukfw_pair){ cpu_to_le32(0), cpu_to_le32(0) };
	t->entries[1] = (struct ukfw_pair){ cpu_to_le32(2), cpu_to_le32(0x40) };
	t->patches[0] = (struct ukfw_pair){ cpu_to_le32(8), cpu_to_le32(UKFW_TA3DCTL) };
	t->patches[1] = (struct ukfw_pair){ cpu_to_le32(0x48), cpu_to_le32(UKFW_HOSTCTL) };
	return t;
}

static void ukfw_seal(struct ukfw_test_file *t)
{
	t->h.image_crc32 = cpu_to_le32(~crc32_le(~0, t->image, T_IMAGE));
}

static int ukfw_try(struct kunit *test, struct ukfw_test_file *t, size_t size)
{
	struct pvrsgx_fw f = {};
	const char *why = "";
	int err = ukfw_parse((const u8 *)t, size, &f, &why);

	kunit_info(test, "%d %s\n", err, err == -EINVAL ? why : "");
	if (!err) {
		KUNIT_EXPECT_EQ(test, f.image_size, T_IMAGE);
		KUNIT_EXPECT_EQ(test, f.features, PVRSGX_UKFW_CLEANUP_V2);
		KUNIT_EXPECT_EQ(test, f.n_entries, T_ENTRIES);
		KUNIT_EXPECT_EQ(test, f.entries[1][0], 2);
		KUNIT_EXPECT_EQ(test, f.entries[1][1], 0x40);
		KUNIT_EXPECT_EQ(test, f.patches[1][1], UKFW_HOSTCTL);
		KUNIT_EXPECT_MEMEQ(test, f.image, t->image, T_IMAGE);
	}
	kfree(f.entries);
	kfree(f.patches);
	kfree(f.image);
	return err;
}

static void ukfw_test_good(struct kunit *test)
{
	struct ukfw_test_file *t = ukfw_good(test);

	ukfw_seal(t);
	KUNIT_EXPECT_EQ(test, ukfw_try(test, t, T_SIZE), 0);
}

static void ukfw_test_not_ours(struct kunit *test)
{
	struct ukfw_test_file *t = ukfw_good(test);

	ukfw_seal(t);
	t->h.magic[1] = 0;
	KUNIT_EXPECT_EQ(test, ukfw_try(test, t, T_SIZE), -ENOENT);
	KUNIT_EXPECT_EQ(test, ukfw_try(test, t, UKFW_HEADER_MIN - 1), -ENOENT);
}

static void ukfw_test_crc(struct kunit *test)
{
	struct ukfw_test_file *t = ukfw_good(test);

	ukfw_seal(t);
	t->image[100] ^= 1;
	KUNIT_EXPECT_EQ(test, ukfw_try(test, t, T_SIZE), -EINVAL);
}

static void ukfw_test_header(struct kunit *test)
{
	static const struct { u32 off, val; } bad[] = {
		{ offsetof(struct ukfw_header, interface), 2 << 16 },	/* unknown major */
		{ offsetof(struct ukfw_header, features), BIT(7) },	/* unknown feature */
		{ offsetof(struct ukfw_header, header_size), 0x58 },	/* below the minimum */
		{ offsetof(struct ukfw_header, header_size), 0x84 },	/* not 8-aligned */
		{ offsetof(struct ukfw_header, image_size), 0 },
		{ offsetof(struct ukfw_header, image_size), T_IMAGE + 8 },	/* past the end */
		{ offsetof(struct ukfw_header, image_size), 0x1001 },	/* not 8-aligned */
		{ offsetof(struct ukfw_header, n_entries), 0 },		/* no event entry */
		{ offsetof(struct ukfw_header, n_entries), 65 },
		{ offsetof(struct ukfw_header, entries_off), T_SIZE - 8 },	/* past the end */
		{ offsetof(struct ukfw_header, patches_off), 2 },	/* misaligned table */
	};

	for (u32 i = 0; i < ARRAY_SIZE(bad); i++) {
		struct ukfw_test_file *t = ukfw_good(test);

		*(__le32 *)((u8 *)&t->h + bad[i].off) = cpu_to_le32(bad[i].val);
		ukfw_seal(t);
		KUNIT_EXPECT_EQ_MSG(test, ukfw_try(test, t, T_SIZE), -EINVAL, "case %u", i);
	}
}

static void ukfw_test_tables(struct kunit *test)
{
	static const struct { bool entry; u32 idx, a, b; } bad[] = {
		{ true, 0, 1, 0 },		/* no program 0 left */
		{ true, 1, 5, 0x40 },		/* program out of range */
		{ true, 1, 2, T_IMAGE },	/* entry past the image */
		{ true, 1, 2, 0x44 },		/* entry not on an instruction */
		{ false, 0, T_IMAGE - 4, UKFW_TA3DCTL },	/* patch past the image */
		{ false, 0, 0xc, UKFW_TA3DCTL },	/* patch not on an instruction */
		{ false, 1, 0x48, 0 },		/* value 0 */
		{ false, 1, 0x48, UKFW_MISC + 1 },	/* unknown value */
	};

	for (u32 i = 0; i < ARRAY_SIZE(bad); i++) {
		struct ukfw_test_file *t = ukfw_good(test);
		struct ukfw_pair *p = bad[i].entry ? &t->entries[bad[i].idx] :
						     &t->patches[bad[i].idx];

		*p = (struct ukfw_pair){ cpu_to_le32(bad[i].a), cpu_to_le32(bad[i].b) };
		ukfw_seal(t);
		KUNIT_EXPECT_EQ_MSG(test, ukfw_try(test, t, T_SIZE), -EINVAL, "case %u", i);
	}
}

static struct kunit_case pvrsgx_fw_test_cases[] = {
	KUNIT_CASE(ukfw_test_good),
	KUNIT_CASE(ukfw_test_not_ours),
	KUNIT_CASE(ukfw_test_crc),
	KUNIT_CASE(ukfw_test_header),
	KUNIT_CASE(ukfw_test_tables),
	{}
};

static struct kunit_suite pvrsgx_fw_test_suite = {
	.name = "powervr_sgx_fw",
	.test_cases = pvrsgx_fw_test_cases,
};

kunit_test_suite(pvrsgx_fw_test_suite);
