/**********************************************************************
 *
 * Copyright (C) Imagination Technologies Ltd. All rights reserved.
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope it will be useful but, except 
 * as otherwise stated in writing, without any warranty; without even the 
 * implied warranty of merchantability or fitness for a particular purpose. 
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 * 
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 * Contact Information:
 * Imagination Technologies Ltd. <gpl-support@imgtec.com>
 * Home Park Estate, Kings Langley, Herts, WD4 8LZ, UK 
 *
 ******************************************************************************/

#if defined(SUPPORT_DRI_DRM)

#include <linux/version.h>

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,38))
#ifndef AUTOCONF_INCLUDED
#include <linux/config.h>
#endif
#endif

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <asm/ioctl.h>
#include <linux/pci.h>

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_drv.h>
#include <drm/drm_ioctl.h>
#include <drm/drm.h>

#include "img_defs.h"
#include "services.h"
#include "kerneldisplay.h"
#include "kernelbuffer.h"
#include "syscommon.h"
#include "pvrmmap.h"
#include "mm.h"
#include "mmap.h"
#include "mutex.h"
#include "pvr_debug.h"
#include "srvkm.h"
#include "perproc.h"
#include "handle.h"
#include "pvr_bridge_km.h"
#include "pvr_bridge.h"
#include "proc.h"
#include "pvrmodule.h"
#include "pvrversion.h"
#include "lock.h"
#include "linkage.h"
#include "pvr_drm.h"

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>

#if defined(PVR_DRI_DRM_NOT_PCI)
#include "pvr_drm_mod.h"
#endif

#define PVR_DRM_NAME	PVRSRV_MODNAME
#define PVR_DRM_DESC	"Imagination Technologies PVR DRM"

DECLARE_WAIT_QUEUE_HEAD(sWaitForInit);

IMG_BOOL bInitComplete;
IMG_BOOL bInitFailed;

#if !defined(PVR_DRI_DRM_NOT_PCI)
#if defined(PVR_DRI_DRM_PLATFORM_DEV)
struct platform_device *gpsPVRLDMDev;
#else
struct pci_dev *gpsPVRLDMDev;
#endif
#endif

struct drm_device *gpsPVRDRMDev;

#if (LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,24))
#error "Linux kernel version 2.6.25 or later required for PVR DRM support"
#endif

#define PVR_DRM_FILE struct drm_file *

DRI_DRM_STATIC int
PVRSRVDrmLoad(struct drm_device *dev, unsigned long flags)
{
	int iRes = 0;

	PVR_TRACE(("PVRSRVDrmLoad"));

	gpsPVRDRMDev = dev;
#if !defined(PVR_DRI_DRM_NOT_PCI)
#if defined(PVR_DRI_DRM_PLATFORM_DEV)
	gpsPVRLDMDev = dev->platformdev;
#else
	gpsPVRLDMDev = to_pci_dev(dev->dev);
#endif
#endif

#if defined(PDUMP)
	iRes = dbgdrv_init();
	if (iRes != 0)
	{
		goto exit;
	}
#endif
	
	iRes = PVRCore_Init();
	if (iRes != 0)
	{
		goto exit_dbgdrv_cleanup;
	}

#if defined(DISPLAY_CONTROLLER)
	iRes = PVR_DRM_MAKENAME(DISPLAY_CONTROLLER, _Init)(dev);
	if (iRes != 0)
	{
		goto exit_pvrcore_cleanup;
	}
#endif
	goto exit;

#if defined(DISPLAY_CONTROLLER)
exit_pvrcore_cleanup:
	PVRCore_Cleanup();
#endif
exit_dbgdrv_cleanup:
#if defined(PDUMP)
	dbgdrv_cleanup();
#endif
exit:
	if (iRes != 0)
	{
		bInitFailed = IMG_TRUE;
	}
	bInitComplete = IMG_TRUE;

	wake_up_interruptible(&sWaitForInit);

	return iRes;
}

DRI_DRM_STATIC int
PVRSRVDrmUnload(struct drm_device *dev)
{
	PVR_TRACE(("PVRSRVDrmUnload"));

#if defined(DISPLAY_CONTROLLER)
	PVR_DRM_MAKENAME(DISPLAY_CONTROLLER, _Cleanup)(dev);
#endif

	PVRCore_Cleanup();

#if defined(PDUMP)
	dbgdrv_cleanup();
#endif

	return 0;
}

DRI_DRM_STATIC int
PVRSRVDrmOpen(struct drm_device *dev, struct drm_file *file)
{
	while (!bInitComplete)
	{
		DEFINE_WAIT(sWait);

		prepare_to_wait(&sWaitForInit, &sWait, TASK_INTERRUPTIBLE);

		if (!bInitComplete)
		{
			PVR_TRACE(("%s: Waiting for module initialisation to complete", __FUNCTION__));

			schedule();
		}

		finish_wait(&sWaitForInit, &sWait);

		if (signal_pending(current))
		{
			return -ERESTARTSYS;
		}
	}

	if (bInitFailed)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Module initialisation failed", __FUNCTION__));
		return -EINVAL;
	}

	return PVRSRVOpen(dev, file);
}

#if defined(SUPPORT_DRI_DRM_EXT) && !defined(PVR_LINUX_USING_WORKQUEUES)
DRI_DRM_STATIC void
PVRSRVDrmPostClose(struct drm_device *dev, struct drm_file *file)
{
	PVRSRVRelease(file->driver_priv);

	file->driver_priv = NULL;
}
#else
DRI_DRM_STATIC int
PVRSRVDrmRelease(struct inode *inode, struct file *filp)
{
	struct drm_file *file_priv = filp->private_data;
	void *psDriverPriv = file_priv->driver_priv;
	int ret;

	ret = drm_release(inode, filp);

	if (ret != 0)
	{
		
		PVR_DPF((PVR_DBG_ERROR, "%s : drm_release failed: %d",
			__FUNCTION__, ret));
	}

	PVRSRVRelease(psDriverPriv);

	return 0;
}
#endif

DRI_DRM_STATIC int
PVRDRMIsMaster(struct drm_device *dev, void *arg, struct drm_file *pFile)
{
	return 0;
}

#if defined(SUPPORT_DRI_DRM_EXT)
int
PVRDRM_Dummy_ioctl(struct drm_device *dev, void *arg, struct drm_file *pFile)
{
	return 0;
}
#endif

DRI_DRM_STATIC int
PVRDRMUnprivCmd(struct drm_device *dev, void *arg, struct drm_file *pFile)
{
	int ret = 0;

	LinuxLockMutex(&gPVRSRVLock);

	if (arg == NULL)
	{
		ret = -EFAULT;
	}
	else
	{
		IMG_UINT32 *pui32Args = (IMG_UINT32 *)arg;
		IMG_UINT32 ui32Cmd = pui32Args[0];
		IMG_UINT32 *pui32OutArg = (IMG_UINT32 *)arg;

		switch (ui32Cmd)
		{
			case PVR_DRM_UNPRIV_INIT_SUCCESFUL:
				*pui32OutArg = PVRSRVGetInitServerState(PVRSRV_INIT_SERVER_SUCCESSFUL) ? 1 : 0;
				break;

			default:
				ret = -EFAULT;
		}

	}

	LinuxUnLockMutex(&gPVRSRVLock);

	return ret;
}

#if defined(DISPLAY_CONTROLLER) && defined(PVR_DISPLAY_CONTROLLER_DRM_IOCTL)
static int
PVRDRM_Display_ioctl(struct drm_device *dev, void *arg, struct drm_file *pFile)
{
	int res;

	LinuxLockMutex(&gPVRSRVLock);

	res = PVR_DRM_MAKENAME(DISPLAY_CONTROLLER, _Ioctl)(dev, arg, pFile);

	LinuxUnLockMutex(&gPVRSRVLock);

	return res;
}
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,33))
#define	PVR_DRM_FOPS_IOCTL	.unlocked_ioctl
#define	PVR_DRM_UNLOCKED	DRM_UNLOCKED
#else
#define	PVR_DRM_FOPS_IOCTL	.ioctl
#define	PVR_DRM_UNLOCKED	0
#endif

#if !defined(SUPPORT_DRI_DRM_EXT)

#define	PVR_DRM_IOCTL_DEF(ioctl, _func, _flags) DRM_IOCTL_DEF_DRV(ioctl, _func, _flags)

/*
 * DRM_UNLOCKED is gone -- every ioctl has been unlocked since 4.x, so the
 * flag no longer exists to be passed.
 */
static const struct drm_ioctl_desc sPVRDrmIoctls[] = {
	PVR_DRM_IOCTL_DEF(PVR_SRVKM, PVRSRV_BridgeDispatchKM, 0),
	PVR_DRM_IOCTL_DEF(PVR_IS_MASTER, PVRDRMIsMaster, DRM_MASTER),
	PVR_DRM_IOCTL_DEF(PVR_UNPRIV, PVRDRMUnprivCmd, 0),
#if defined(PDUMP)
	PVR_DRM_IOCTL_DEF(PVR_DBGDRV, dbgdrv_ioctl, 0),
#endif
#if defined(DISPLAY_CONTROLLER) && defined(PVR_DISPLAY_CONTROLLER_DRM_IOCTL)
	PVR_DRM_IOCTL_DEF(PVR_DISP, PVRDRM_Display_ioctl, DRM_MASTER)
#endif
};

static struct pci_device_id asPciIdList[] = {
	{SYS_SGX_DEV_VENDOR_ID, SYS_SGX_DEV_DEVICE_ID, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
#if defined(SYS_SGX_DEV1_DEVICE_ID)
	{SYS_SGX_DEV_VENDOR_ID, SYS_SGX_DEV1_DEVICE_ID, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
#endif
	{0}
};
MODULE_DEVICE_TABLE(pci, asPciIdList);

/*
 * struct drm_driver no longer embeds the file_operations, so they live out
 * here and the driver points at them. .fasync went away entirely; nothing
 * used it.
 */
static const struct file_operations sPVRDrmFops = {
	.owner		= THIS_MODULE,
	.open		= drm_open,
	.release	= PVRSRVDrmRelease,
	.unlocked_ioctl	= drm_ioctl,
#if defined(CONFIG_COMPAT)
	.compat_ioctl	= drm_compat_ioctl,
#endif
	.mmap		= PVRMMap,
	.poll		= drm_poll,
	.llseek		= noop_llseek,
	/*
	 * DRM hands the mmap offset through as an unsigned token rather than a
	 * signed file position -- PVRMMap keys its lookup on vm_pgoff, which is
	 * exactly that. Since 6.12 drm_open_helper() enforces the declaration
	 * and refuses the open with -EINVAL (plus a WARN) if it is missing, so
	 * this is not optional. DRM_GEM_FOPS sets it for GEM drivers; this fops
	 * is hand-rolled because we have no GEM, so it has to be set here.
	 */
	.fop_flags	= FOP_UNSIGNED_OFFSET,
};

/*
 * driver_features is 0 deliberately. This is not a modesetting driver and it
 * owns no GEM objects -- DRM is used purely as an ioctl multiplexer with a
 * named device node, which is what userspace's drmOpen() matches on. Display
 * on this board is ce5300-fb, entirely separately.
 *
 * .load/.unload are gone from struct drm_driver; the work they did now
 * happens either side of drm_dev_register() in probe. .suspend/.resume are
 * gone too and belong to the pci_driver. .pci_driver is gone -- the PCI
 * driver is registered on its own, below.
 */
static const struct drm_driver sPVRDrmDriver =
{
	.driver_features = 0,
	.open		= PVRSRVDrmOpen,
	.ioctls		= sPVRDrmIoctls,
	.num_ioctls	= ARRAY_SIZE(sPVRDrmIoctls),
	.fops		= &sPVRDrmFops,
	.name		= PVR_DRM_NAME,
	.desc		= PVR_DRM_DESC,
	.major		= PVRVERSION_MAJ,
	.minor		= PVRVERSION_MIN,
	.patchlevel	= PVRVERSION_BUILD,
};

static int PVRSRVDrmProbe(struct pci_dev *pDevice, const struct pci_device_id *pId)
{
	struct drm_device *dev;
	int iRes;

	PVR_TRACE(("PVRSRVDrmProbe"));

	PVR_UNREFERENCED_PARAMETER(pId);

	iRes = pci_enable_device(pDevice);
	if (iRes != 0)
	{
		return iRes;
	}

	dev = drm_dev_alloc(&sPVRDrmDriver, &pDevice->dev);
	if (IS_ERR(dev))
	{
		iRes = PTR_ERR(dev);
		goto err_disable;
	}

	pci_set_drvdata(pDevice, dev);

	/*
	 * PVRSRVDrmLoad brings up PVR services, which the system layer needs
	 * to have happened before anything touches the hardware. It used to be
	 * drm_driver.load, called from inside registration; now it has to run
	 * before we publish the node.
	 */
	iRes = PVRSRVDrmLoad(dev, 0);
	if (iRes != 0)
	{
		goto err_put;
	}

	iRes = drm_dev_register(dev, 0);
	if (iRes != 0)
	{
		goto err_unload;
	}

	return 0;

err_unload:
	PVRSRVDrmUnload(dev);
err_put:
	drm_dev_put(dev);
	pci_set_drvdata(pDevice, NULL);
err_disable:
	pci_disable_device(pDevice);

	return iRes;
}

static void PVRSRVDrmRemove(struct pci_dev *pDevice)
{
	struct drm_device *dev = pci_get_drvdata(pDevice);

	PVR_TRACE(("PVRSRVDrmRemove"));

	if (dev == NULL)
	{
		return;
	}

	drm_dev_unregister(dev);
	PVRSRVDrmUnload(dev);
	drm_dev_put(dev);

	pci_set_drvdata(pDevice, NULL);
	pci_disable_device(pDevice);
}

/*
 * module.c's PVRSRVDriverShutdown only exists under PVR_LDM_MODULE, which we
 * do not define, so do the same one thing here.
 */
static void PVRSRVDrmShutdown(struct pci_dev *pDevice)
{
	PVR_TRACE(("PVRSRVDrmShutdown"));

	PVR_UNREFERENCED_PARAMETER(pDevice);

	(void) PVRSRVSetPowerStateKM(PVRSRV_SYS_POWER_STATE_D3);
}

static struct pci_driver sPVRPciDriver =
{
	.name		= PVR_DRM_NAME,
	.id_table	= asPciIdList,
	.probe		= PVRSRVDrmProbe,
	.remove		= PVRSRVDrmRemove,
	.shutdown	= PVRSRVDrmShutdown,
};

static int __init PVRSRVDrmInit(void)
{
	PVRDPFInit();

	return pci_register_driver(&sPVRPciDriver);
}

static void __exit PVRSRVDrmExit(void)
{
	pci_unregister_driver(&sPVRPciDriver);
}

module_init(PVRSRVDrmInit);
module_exit(PVRSRVDrmExit);
#endif	/* !defined(SUPPORT_DRI_DRM_EXT) */
#endif
