// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

#include <drm/drm_print.h>

#include <linux/pm_runtime.h>

#include "pvrsgx_device.h"
#include "pvrsgx_fw.h"
#include "pvrsgx_job.h"
#include "pvrsgx_pm.h"

/*
 * Runtime PM. The SGX function has no PCI PM capability, so the PCI core keeps
 * it in D0: suspend is the microkernel's power-off (pvrsgx_fw_stop), resume
 * the same full start a reset does. A job on the GPU holds a reference from
 * run_job until it leaves the active list.
 */
#define AUTOSUSPEND_MS	500

int pvrsgx_power_get(struct pvrsgx_device *sdev)
{
	return pm_runtime_resume_and_get(sdev->base.dev);
}

void pvrsgx_power_put(struct pvrsgx_device *sdev, u32 n)
{
	if (!n)
		return;
	pm_runtime_mark_last_busy(sdev->base.dev);
	while (n--)
		pm_runtime_put_autosuspend(sdev->base.dev);
}

/*
 * After probe. The PCI core holds a reference over probe and forbids runtime
 * PM by default; an auxiliary device starts with runtime PM disabled, and its
 * parent (gma500, which powers the graphics island) stays up while it runs.
 */
void pvrsgx_power_init(struct pvrsgx_device *sdev)
{
	struct device *dev = sdev->base.dev;

	pm_runtime_set_autosuspend_delay(dev, AUTOSUSPEND_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_mark_last_busy(dev);
	if (sdev->pdev) {
		pm_runtime_put_autosuspend(dev);
		pm_runtime_allow(dev);
	} else {
		pm_runtime_set_active(dev);
		pm_runtime_enable(dev);
		/*
		 * A single-context core is still in bring-up: its power-off and
		 * the state loss of a suspend are untested, so it stays awake.
		 */
		if (pvrsgx_core_single_context(sdev->family)) {
			pm_runtime_get_noresume(dev);
			sdev->pm_pinned = true;
		}
	}
}

/* Before remove: undo pvrsgx_power_init, with the device awake. */
void pvrsgx_power_fini(struct pvrsgx_device *sdev)
{
	struct device *dev = sdev->base.dev;

	if (sdev->pdev) {
		pm_runtime_forbid(dev);
		pm_runtime_dont_use_autosuspend(dev);
		pm_runtime_get_noresume(dev);
	} else {
		pm_runtime_get_sync(dev);
		pm_runtime_dont_use_autosuspend(dev);
		pm_runtime_disable(dev);
		pm_runtime_set_suspended(dev);
		pm_runtime_put_noidle(dev);
		if (sdev->pm_pinned) {
			pm_runtime_put_noidle(dev);
			sdev->pm_pinned = false;
		}
	}
}

static int pvrsgx_runtime_suspend(struct device *dev)
{
	struct pvrsgx_device *sdev = dev_get_drvdata(dev);

	if (!sdev->lost)
		pvrsgx_fw_stop(sdev);
	return 0;
}

static int pvrsgx_runtime_resume(struct device *dev)
{
	struct pvrsgx_device *sdev = dev_get_drvdata(dev);

	/* A failed start loses the device: run_job then fails every job with -EIO. */
	if (!sdev->lost && pvrsgx_fw_start(sdev)) {
		drm_err(&sdev->base, "microkernel did not start on resume: device lost\n");
		sdev->lost = true;
	}
	return 0;
}

/*
 * System sleep. The core loses power, so the microkernel has to stop first and
 * start again after: drain the GPU, then the runtime suspend and resume above.
 * Behind gma500 this runs before its suspend and after its resume, with the
 * graphics island up.
 */
static int pvrsgx_suspend(struct device *dev)
{
	struct pvrsgx_device *sdev = dev_get_drvdata(dev);
	int err = pvrsgx_job_quiesce(sdev);

	if (err)
		return err;
	err = pm_runtime_force_suspend(dev);
	if (err)
		pvrsgx_job_unquiesce(sdev);
	return err;
}

static int pvrsgx_resume(struct device *dev)
{
	struct pvrsgx_device *sdev = dev_get_drvdata(dev);
	int err = pm_runtime_force_resume(dev);

	pvrsgx_job_unquiesce(sdev);
	return err;
}

const struct dev_pm_ops pvrsgx_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(pvrsgx_suspend, pvrsgx_resume)
	RUNTIME_PM_OPS(pvrsgx_runtime_suspend, pvrsgx_runtime_resume, NULL)
};
