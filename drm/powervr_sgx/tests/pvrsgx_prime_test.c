// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/* KUnit tests of a job's cache ranges (rev3-commands 8.2), included from pvrsgx_prime.c. */

#include <kunit/test.h>

#include <linux/err.h>
#include <linux/mman.h>
#include <linux/uaccess.h>

#define R(f, o, sz, st, n) ((struct drm_powervr_sgx_cache_range){ .handle = 1, .flags = (f), \
	.offset = (o), .size = (sz), .stride = (st), .rows = (n) })
#define RD DRM_POWERVR_SGX_CACHE_RANGE_FLAG_READ
#define WR DRM_POWERVR_SGX_CACHE_RANGE_FLAG_WRITE

static void prime_test_range_accepts(struct kunit *test)
{
	const u64 obj = 2944 * 480;
	struct drm_powervr_sgx_cache_range r;

	/* a linear range, the whole object, rows 0 and 1 alike */
	r = R(RD, 0, obj, 0, 0);
	KUNIT_EXPECT_EQ(test, cache_range_check(&r, obj), 0);
	r = R(WR, 0, obj, 0, 1);
	KUNIT_EXPECT_EQ(test, cache_range_check(&r, obj), 0);
	/* a 64x16 damage rect at (100, 200) of a 720x480 XRGB buffer, pitch 2944 */
	r = R(RD | WR, 200 * 2944 + 100 * 4, 64 * 4, 2944, 16);
	KUNIT_EXPECT_EQ(test, cache_range_check(&r, obj), 0);
	/* the last row ends at the object's end exactly */
	r = R(WR, 0, 720 * 4, 2944, 480);
	KUNIT_EXPECT_EQ(test, cache_range_check(&r, obj), 0);
	r = R(WR, 2944 - 720 * 4, 720 * 4, 2944, 480);
	KUNIT_EXPECT_EQ(test, cache_range_check(&r, obj), 0);
	/* rows exactly stride apart are one span */
	r = R(WR, 0, 2944, 2944, 480);
	KUNIT_EXPECT_EQ(test, cache_range_check(&r, obj), 0);
}

static void prime_test_range_refuses(struct kunit *test)
{
	const u64 obj = 2944 * 480;
	struct drm_powervr_sgx_cache_range r;

	/* flags: none, or an unknown bit */
	r = R(0, 0, 64, 0, 1);
	KUNIT_EXPECT_EQ(test, cache_range_check(&r, obj), -EINVAL);
	r = R(RD | BIT(2), 0, 64, 0, 1);
	KUNIT_EXPECT_EQ(test, cache_range_check(&r, obj), -EINVAL);
	/* an empty row */
	r = R(RD, 0, 0, 0, 1);
	KUNIT_EXPECT_EQ(test, cache_range_check(&r, obj), -EINVAL);
	/* rows overlapping: stride below size */
	r = R(RD, 0, 64, 32, 2);
	KUNIT_EXPECT_EQ(test, cache_range_check(&r, obj), -EINVAL);
	/* one byte past the end, linear and in the last row */
	r = R(RD, 1, obj, 0, 1);
	KUNIT_EXPECT_EQ(test, cache_range_check(&r, obj), -EINVAL);
	r = R(WR, 2944 - 720 * 4 + 1, 720 * 4, 2944, 480);
	KUNIT_EXPECT_EQ(test, cache_range_check(&r, obj), -EINVAL);
	/* one row too many */
	r = R(WR, 0, 720 * 4, 2944, 481);
	KUNIT_EXPECT_EQ(test, cache_range_check(&r, obj), -EINVAL);
	/* overflow in offset + size, and in rows * stride */
	r = R(RD, U64_MAX - 7, 16, 0, 1);
	KUNIT_EXPECT_EQ(test, cache_range_check(&r, obj), -EINVAL);
	r = R(RD, 0, U64_MAX, 0, 1);
	KUNIT_EXPECT_EQ(test, cache_range_check(&r, obj), -EINVAL);
	r = R(RD, U64_MAX - (u64)U32_MAX * 2, U32_MAX, U32_MAX, U32_MAX);
	KUNIT_EXPECT_EQ(test, cache_range_check(&r, U64_MAX), -EINVAL);
}

/* The array checks come before any copy from userspace or handle lookup. */
static void prime_test_range_array(struct kunit *test)
{
	struct drm_powervr_sgx_obj_array arr = { .stride = sizeof(struct drm_powervr_sgx_cache_range) };
	struct pvrsgx_job job = {};

	KUNIT_EXPECT_EQ(test, pvrsgx_prime_ranges(NULL, &arr, &job), 0);
	KUNIT_EXPECT_EQ(test, job.nranges, 0);
	arr.count = PVRSGX_MAX_CACHE_RANGES + 1;
	KUNIT_EXPECT_EQ(test, pvrsgx_prime_ranges(NULL, &arr, &job), -EINVAL);
	arr.count = 1;
	arr.stride = 0;
	KUNIT_EXPECT_EQ(test, pvrsgx_prime_ranges(NULL, &arr, &job), -EINVAL);
	KUNIT_EXPECT_NULL(test, job.ranges);
}

/* An element longer than the kernel's with a non-zero tail: -E2BIG, before any lookup. */
static void prime_test_range_tail(struct kunit *test)
{
	struct {
		struct drm_powervr_sgx_cache_range r;
		u64 tail;
	} big = { .r = R(RD, 0, 64, 0, 1), .tail = 1 };
	struct drm_powervr_sgx_obj_array arr = { .stride = sizeof(big), .count = 1 };
	struct pvrsgx_job job = {};
	unsigned long user;

	user = kunit_vm_mmap(test, NULL, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_ANONYMOUS | MAP_PRIVATE, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR_VALUE(user));
	KUNIT_ASSERT_NE(test, user, 0);
	KUNIT_ASSERT_EQ(test, copy_to_user((void __user *)user, &big, sizeof(big)), 0);
	arr.array = user;
	KUNIT_EXPECT_EQ(test, pvrsgx_prime_ranges(NULL, &arr, &job), -E2BIG);
	KUNIT_EXPECT_EQ(test, job.nranges, 0);
}

static struct kunit_case pvrsgx_prime_test_cases[] = {
	KUNIT_CASE(prime_test_range_array),
	KUNIT_CASE(prime_test_range_tail),
	KUNIT_CASE(prime_test_range_accepts),
	KUNIT_CASE(prime_test_range_refuses),
	{}
};

static struct kunit_suite pvrsgx_prime_test_suite = {
	.name = "powervr_sgx_prime",
	.test_cases = pvrsgx_prime_test_cases,
};

kunit_test_suite(pvrsgx_prime_test_suite);
