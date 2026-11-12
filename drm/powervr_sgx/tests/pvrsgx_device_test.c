// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * KUnit tests of the probe's core check and the VA limits, included from
 * pvrsgx_device.c so they reach identify(). The registers are a memory
 * buffer holding an SGX530's identity.
 */

#include <kunit/test.h>

#include <linux/sizes.h>

#include <uapi/drm/powervr_sgx_drm.h>

#include "../pvrsgx_drv.h"
#include "../pvrsgx_vm.h"

static struct pvrsgx_device *device_test_sgx530(struct kunit *test)
{
	struct pvrsgx_device *sdev = kunit_kzalloc(test, sizeof(*sdev), GFP_KERNEL);
	u32 *regs = kunit_kzalloc(test, PVRSGX_REGS_MIN, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, sdev);
	KUNIT_ASSERT_NOT_NULL(test, regs);
	/* As read on our OMAP3630 test board. */
	regs[0x10 / 4] = 0x01120000;
	regs[0x14 / 4] = 0x00010205;
	sdev->regs = (void __iomem __force *)regs;
	sdev->family = pvrsgx_core_family_get(530);
	KUNIT_ASSERT_NOT_NULL(test, sdev->family);
	return sdev;
}

/* Without allow_unisolated a single-context core is refused; with it, it runs. */
static void device_test_unisolated_gate(struct kunit *test)
{
	bool was = pvrsgx_allow_unisolated;
	struct pvrsgx_device *sdev = device_test_sgx530(test);

	pvrsgx_allow_unisolated = false;
	KUNIT_EXPECT_EQ(test, identify(sdev), -ENODEV);
	KUNIT_EXPECT_NULL(test, sdev->core);
	KUNIT_EXPECT_EQ(test, sdev->core_id, 0x01120000);

	pvrsgx_allow_unisolated = true;
	KUNIT_EXPECT_EQ(test, identify(sdev), 0);
	KUNIT_ASSERT_NOT_NULL(test, sdev->core);
	KUNIT_EXPECT_STREQ(test, sdev->core->name, "TI OMAP3630 (SGX530 1.2.5)");
	pvrsgx_allow_unisolated = was;
}

/* A part the table doesn't list is refused even with the parameter. */
static void device_test_unknown_part(struct kunit *test)
{
	bool was = pvrsgx_allow_unisolated;
	struct pvrsgx_device *sdev = device_test_sgx530(test);

	/* An OMAP3430's SGX530, rev 121. */
	((u32 __force *)sdev->regs)[0x14 / 4] = 0x00010201;
	pvrsgx_allow_unisolated = true;
	KUNIT_EXPECT_EQ(test, identify(sdev), -ENODEV);
	KUNIT_EXPECT_NULL(test, sdev->core);
	pvrsgx_allow_unisolated = was;
}

/* VM_MAP's heap check: nothing at or above 2^28 on the SGX530, the 4 GiB layout on the 545. */
static void device_test_va_limit(struct kunit *test)
{
	struct pvrsgx_device *s530 = device_test_sgx530(test);
	struct pvrsgx_device *s545 = kunit_kzalloc(test, sizeof(*s545), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, s545);
	s545->family = pvrsgx_core_family_get(545);

	KUNIT_EXPECT_EQ(test, pvrsgx_heap_find(s530, 0x04000000, SZ_4K),
			DRM_POWERVR_SGX_HEAP_GENERAL);
	KUNIT_EXPECT_EQ(test, pvrsgx_heap_find(s530, 0x083ff000, SZ_4K),
			DRM_POWERVR_SGX_HEAP_TEXTURE);
	KUNIT_EXPECT_EQ(test, pvrsgx_heap_find(s530, 0x083ff000, SZ_8K), -1);
	KUNIT_EXPECT_EQ(test, pvrsgx_heap_find(s530, 0x0fdff000, SZ_4K),
			DRM_POWERVR_SGX_HEAP_USSE_VERTEX);
	KUNIT_EXPECT_EQ(test, pvrsgx_heap_find(s530, 0x10000000, SZ_4K), -1);
	KUNIT_EXPECT_EQ(test, pvrsgx_heap_find(s530, 0xf0000000, SZ_4K), -1);
	/* The kernel heaps are no client's. */
	KUNIT_EXPECT_EQ(test, pvrsgx_heap_find(s530, 0x0ec00000, SZ_4K), -1);
	KUNIT_EXPECT_EQ(test, pvrsgx_heap_find(s530, 0x0f000000, SZ_4K), -1);
	KUNIT_EXPECT_EQ(test, pvrsgx_heap_find(s530, 0x0d400000, SZ_4K), -1);

	KUNIT_EXPECT_EQ(test, pvrsgx_heap_find(s545, 0xf0000000, SZ_4K),
			DRM_POWERVR_SGX_HEAP_TEXTURE);
	KUNIT_EXPECT_EQ(test, pvrsgx_heap_find(s545, 0x3e000000, SZ_4K),
			DRM_POWERVR_SGX_HEAP_GENERAL);
}

static struct kunit_case pvrsgx_device_test_cases[] = {
	KUNIT_CASE(device_test_unisolated_gate),
	KUNIT_CASE(device_test_unknown_part),
	KUNIT_CASE(device_test_va_limit),
	{}
};

static struct kunit_suite pvrsgx_device_test_suite = {
	.name = "powervr_sgx_device",
	.test_cases = pvrsgx_device_test_cases,
};

kunit_test_suite(pvrsgx_device_test_suite);
