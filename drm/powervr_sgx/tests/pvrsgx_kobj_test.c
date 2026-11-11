// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * KUnit tests of kernel-object teardown at unbind, included from
 * pvrsgx_render.c so they reach the device PB's static functions. No GPU:
 * the objects, page tables and VMs are memory only.
 */

#include <kunit/test.h>

#include <drm/drm_kunit_helpers.h>

/* The parts of pvrsgx_device_init() that kernel objects, VMs and PBs need. */
static struct pvrsgx_device *kobj_test_device(struct kunit *test)
{
	struct pvrsgx_device *sdev;
	struct device *dev;

	dev = drm_kunit_helper_alloc_device(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	sdev = drm_kunit_helper_alloc_drm_device(test, dev, struct pvrsgx_device, base, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sdev);
	sdev->kernel_mmu = pvrsgx_mmu_create(sdev);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sdev->kernel_mmu);
	mutex_init(&sdev->ccb_lock);
	mutex_init(&sdev->pb_lock);
	pvrsgx_kobj_init(sdev);
	return sdev;
}

/* What pvrsgx_device_fini() does with them; every range must come back empty. */
static void kobj_test_fini(struct kunit *test, struct pvrsgx_device *sdev)
{
	pvrsgx_pb_put(sdev->pb);
	sdev->pb = NULL;
	pvrsgx_kobj_fini(sdev);
	KUNIT_EXPECT_TRUE(test, list_empty(&sdev->graveyard));
	for (int r = 0; r < PVRSGX_KRANGE_COUNT; r++)
		KUNIT_EXPECT_TRUE_MSG(test, drm_mm_clean(&sdev->krange[r]), "range %d", r);
	pvrsgx_mmu_destroy(sdev->kernel_mmu);
	mutex_destroy(&sdev->pb_lock);
	mutex_destroy(&sdev->ccb_lock);
}

/*
 * A client's last render context goes, and no render runs after it: its
 * per-VM scratch object waits in the graveyard and keeps the VM, so the VM
 * and its PB reference are freed only at unbind, by the reap. The PB's
 * objects join the graveyard then, and unbind must free them too.
 */
static void kobj_test_vm_held_by_graveyard(struct kunit *test)
{
	struct pvrsgx_device *sdev = kobj_test_device(test);
	struct pvrsgx_kobj *scratch = NULL;
	struct pvrsgx_vm *vm;
	struct pvrsgx_pb *pb;

	vm = pvrsgx_vm_create(sdev);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vm);
	pb = dev_pb_get(sdev, 0x40);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pb);
	KUNIT_ASSERT_EQ(test, pvrsgx_pb_map(vm, pb), 0);
	KUNIT_ASSERT_EQ(test, pvrsgx_kobj_new(sdev, &scratch, PVRSGX_KRANGE_3DPARAMS, 0x40,
					      pb->base, vm), 0);

	/* pvrsgx_render_ctx_fini(), then the file's VM handle. */
	pvrsgx_kobj_drop(sdev, &scratch);
	pvrsgx_pb_put(pb);
	pvrsgx_vm_put(vm);
	KUNIT_EXPECT_FALSE(test, list_empty(&sdev->graveyard));

	kobj_test_fini(test, sdev);
}

/* The VM goes first and nothing waits: the PB is the device's alone at unbind. */
static void kobj_test_vm_gone_first(struct kunit *test)
{
	struct pvrsgx_device *sdev = kobj_test_device(test);
	struct pvrsgx_vm *vm;
	struct pvrsgx_pb *pb;

	vm = pvrsgx_vm_create(sdev);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vm);
	pb = dev_pb_get(sdev, 0x40);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, pb);
	KUNIT_ASSERT_EQ(test, pvrsgx_pb_map(vm, pb), 0);
	pvrsgx_pb_put(pb);
	pvrsgx_vm_put(vm);
	KUNIT_EXPECT_TRUE(test, list_empty(&sdev->graveyard));

	kobj_test_fini(test, sdev);
}

static struct kunit_case pvrsgx_kobj_test_cases[] = {
	KUNIT_CASE(kobj_test_vm_held_by_graveyard),
	KUNIT_CASE(kobj_test_vm_gone_first),
	{}
};

static struct kunit_suite pvrsgx_kobj_test_suite = {
	.name = "powervr_sgx_kobj",
	.test_cases = pvrsgx_kobj_test_cases,
};

kunit_test_suite(pvrsgx_kobj_test_suite);
