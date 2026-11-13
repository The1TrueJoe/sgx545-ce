// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * KUnit tests of what a GEM object tells fdinfo's memory stats, included from
 * pvrsgx_gem.c: resident, and its resident size, once it has pages.
 */

#include <kunit/test.h>

static void gem_test_status(struct kunit *test)
{
	struct pvrsgx_bo *bo = kunit_kzalloc(test, sizeof(*bo), GFP_KERNEL);
	struct page *page = NULL;
	struct drm_gem_object *obj;

	KUNIT_ASSERT_NOT_NULL(test, bo);
	obj = &bo->base.base;
	obj->size = 3 * PAGE_SIZE;

	/* No pages yet: not resident, nothing counted. */
	KUNIT_EXPECT_EQ(test, pvrsgx_gem_status(obj), 0);
	KUNIT_EXPECT_EQ(test, pvrsgx_gem_rss(obj), 0);

	/* With pages: resident, its whole size; never purgeable. */
	bo->base.pages = &page;
	KUNIT_EXPECT_EQ(test, pvrsgx_gem_status(obj), DRM_GEM_OBJECT_RESIDENT);
	KUNIT_EXPECT_EQ(test, pvrsgx_gem_rss(obj), 3 * PAGE_SIZE);
	bo->base.pages = NULL;
}

static struct kunit_case pvrsgx_gem_test_cases[] = {
	KUNIT_CASE(gem_test_status),
	{}
};

static struct kunit_suite pvrsgx_gem_test_suite = {
	.name = "powervr_sgx_gem",
	.test_cases = pvrsgx_gem_test_cases,
};

kunit_test_suite(pvrsgx_gem_test_suite);
