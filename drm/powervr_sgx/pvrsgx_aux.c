// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * The SGX545 of a Cedarview GPU (GMA3600/3650). It shares the PCI function and
 * register BAR with the display engine, which gma500 drives; gma500 publishes
 * the SGX as the auxiliary device "gma500_gfx.sgx" with its register window mapped
 * and hands over its interrupt (include/drm/gma500_sgx.h).
 */

#include <linux/auxiliary_bus.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>

#include <drm/gma500_sgx.h>

#include "pvrsgx_device.h"
#include "pvrsgx_pm.h"
#include "pvrsgx_drv.h"

static int pvrsgx_aux_probe(struct auxiliary_device *adev, const struct auxiliary_device_id *id)
{
	struct gma500_sgx_device *gma = to_gma500_sgx_device(adev);
	struct device *dev = &adev->dev;
	struct pvrsgx_device *sdev;
	int err;

	if (!gma->regs || gma->regs_len < PVRSGX_REGS_MIN || !dev_is_pci(dev->parent)) {
		dev_err(dev, "unexpected SGX register window (%pa bytes); not touching it\n",
			&gma->regs_len);
		return -ENODEV;
	}

	sdev = pvrsgx_alloc(dev);
	if (IS_ERR(sdev))
		return PTR_ERR(sdev);
	/* The SGX does DMA as the GPU's PCI function: no device of its own. */
	drm_dev_set_dma_dev(&sdev->base, dev->parent);
	sdev->gma_sgx = gma;
	sdev->family = pvrsgx_core_family_get(gma->core);
	sdev->regs = gma->regs;
	sdev->irq = to_pci_dev(dev->parent)->irq;
	auxiliary_set_drvdata(adev, sdev);

	/* The graphics island is up only while gma500 is. */
	err = pm_runtime_resume_and_get(dev->parent);
	if (err)
		return err;
	err = pvrsgx_probe_common(sdev);
	pm_runtime_put(dev->parent);
	return err;
}

static void pvrsgx_aux_remove(struct auxiliary_device *adev)
{
	pvrsgx_remove_common(auxiliary_get_drvdata(adev));
}

static const struct auxiliary_device_id pvrsgx_aux_ids[] = {
	{ .name = "gma500_gfx.sgx" },
	{ }
};
MODULE_DEVICE_TABLE(auxiliary, pvrsgx_aux_ids);

static struct auxiliary_driver pvrsgx_aux_driver = {
	.name = "sgx",
	.probe = pvrsgx_aux_probe,
	.remove = pvrsgx_aux_remove,
	.id_table = pvrsgx_aux_ids,
	.driver.pm = pm_ptr(&pvrsgx_pm_ops),
};

int pvrsgx_aux_register(void)
{
	return auxiliary_driver_register(&pvrsgx_aux_driver);
}

void pvrsgx_aux_unregister(void)
{
	auxiliary_driver_unregister(&pvrsgx_aux_driver);
}
