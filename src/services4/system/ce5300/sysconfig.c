/**********************************************************************
 *
 * Copyright (C) Imagination Technologies Ltd. All rights reserved.
 * Copyright (C) 2026 openHC contributors.
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
 * Contact Information:
 * Imagination Technologies Ltd. <gpl-support@imgtec.com>
 *
 ******************************************************************************/

/*
 * System configuration layer for the PowerVR SGX545 in the Intel Atom CE5300
 * family (CE5310 as fitted to the Control4 EA-3).
 *
 * Derived from Intel's Cedarview layer (services4/system/unified) with three
 * structural changes:
 *
 *   1. No DRM. Cedarview's layer reaches into psb_drv.h for gpDrmDevice,
 *      IS_CDV() and drm_psb_private. We build with SUPPORT_DRI_DRM off, so
 *      the SGX attaches as a plain PCI driver and exposes a char device. All
 *      of that coupling is gone.
 *
 *   2. Registers at BAR0+0, not BAR0+0x80000. On Poulsbo/Moorestown/Cedarview
 *      the SGX shares the display engine's BAR. On CE5300 it is its own PCI
 *      function (01:02.0) and its register block starts at offset zero. See
 *      the note in sysconfig.h for how this was established.
 *
 *   3. No MSVDX, no host port, no local memory. The CE5300 video decoder is a
 *      separate driver entirely (ismdviddec), the part has no SGX host port,
 *      and graphics memory is plain system memory.
 */

#include <linux/module.h>
#include <linux/pci.h>

#include "sysconfig.h"
#include "services_headers.h"
#include "kerneldisplay.h"
#include "oemfuncs.h"
#include "sgxinfo.h"
#include "sgxinfokm.h"
#include "syslocal.h"
#include "sgxdefs.h"

/*
 * Timing. These feed the DDK's watchdog and power-management arithmetic, not
 * the hardware clock tree -- nothing here changes how fast the GPU runs. They
 * decide how long the driver waits before declaring the core hung, and how
 * often the microkernel's PDS timer fires.
 *
 * ui32CoreClockSpeed is the one worth measuring. Intel shipped Cedarview's
 * SGX545 at 400 MHz and we have no published figure for CE5300, so that is
 * the default. If it is wrong the driver still works; hardware-recovery
 * timeouts are simply scaled by the ratio. Left as a module parameter rather
 * than a constant precisely because it wants tuning against real silicon.
 */
static unsigned int sgx_core_clock = 400000000;
module_param(sgx_core_clock, uint, 0444);
MODULE_PARM_DESC(sgx_core_clock,
	"SGX core clock in Hz, used for watchdog timing only (default 400000000)");

static unsigned int sgx_apm;
module_param(sgx_apm, uint, 0444);
MODULE_PARM_DESC(sgx_apm,
	"Enable SGX active power management (default 0 -- off until bring-up is stable)");

#define SYS_SGX_HWRECOVERY_TIMEOUT_FREQ		(100)	/* Hz */
#define SYS_SGX_PDS_TIMER_FREQ			(1000)	/* Hz */
#define SYS_SGX_ACTIVE_POWER_LATENCY_MS		(5)

SYS_DATA *gpsSysData = (SYS_DATA *)IMG_NULL;
static SYS_DATA gsSysData;

static SYS_SPECIFIC_DATA gsSysSpecificData;

IMG_UINT32 gui32SGXDeviceID;
static SGX_DEVICE_MAP gsSGXDeviceMap;
static PVRSRV_DEVICE_NODE *gpsSGXDevNode;

/* Set by PVRSRVDriverProbe() in env/linux/module.c when the PCI id matches. */
extern struct pci_dev *gpsPVRLDMDev;

static PVRSRV_ERROR SysMapInRegisters(IMG_VOID);
static PVRSRV_ERROR SysUnmapRegisters(IMG_VOID);

/*****************************************************************************/

static PVRSRV_ERROR PCIInitDev(SYS_DATA *psSysData)
{
	SYS_SPECIFIC_DATA *psSysSpecData =
		(SYS_SPECIFIC_DATA *)psSysData->pvSysSpecificData;

	if (psSysSpecData->psPCIDev == IMG_NULL)
	{
		PVR_DPF((PVR_DBG_ERROR, "PCIInitDev: no PCI device"));
		return PVRSRV_ERROR_PCI_DEVICE_NOT_FOUND;
	}

	psSysSpecData->hSGXPCI =
		OSPCISetDev((IMG_VOID *)psSysSpecData->psPCIDev, HOST_PCI_INIT_FLAG_BUS_MASTER);
	if (!psSysSpecData->hSGXPCI)
	{
		PVR_DPF((PVR_DBG_ERROR, "PCIInitDev: failed to acquire PCI device"));
		return PVRSRV_ERROR_PCI_DEVICE_NOT_FOUND;
	}
	SYS_SPECIFIC_DATA_SET(psSysSpecData, SYS_SPECIFIC_DATA_PCI_ACQUIRE_DEV);

	PVR_TRACE(("SGX PCI region: 0x%08X to 0x%08X",
		OSPCIAddrRangeStart(psSysSpecData->hSGXPCI, SYS_SGX_ADDR_RANGE_INDEX),
		OSPCIAddrRangeEnd(psSysSpecData->hSGXPCI, SYS_SGX_ADDR_RANGE_INDEX)));

	if (OSPCIAddrRangeLen(psSysSpecData->hSGXPCI, SYS_SGX_ADDR_RANGE_INDEX)
			< SYS_SGX_MAX_OFFSET)
	{
		PVR_DPF((PVR_DBG_ERROR, "PCIInitDev: BAR0 too small for the SGX register block"));
		return PVRSRV_ERROR_PCI_REGION_TOO_SMALL;
	}

	if (OSPCIRequestAddrRange(psSysSpecData->hSGXPCI, SYS_SGX_ADDR_RANGE_INDEX)
			!= PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "PCIInitDev: BAR0 not available"));
		return PVRSRV_ERROR_PCI_REGION_UNAVAILABLE;
	}
	SYS_SPECIFIC_DATA_SET(psSysSpecData, SYS_SPECIFIC_DATA_PCI_REQUEST_SGX_ADDR_RANGE);

	return PVRSRV_OK;
}

static IMG_VOID PCIDeInitDev(SYS_DATA *psSysData)
{
	SYS_SPECIFIC_DATA *psSysSpecData =
		(SYS_SPECIFIC_DATA *)psSysData->pvSysSpecificData;

	if (SYS_SPECIFIC_DATA_TEST(psSysSpecData, SYS_SPECIFIC_DATA_PCI_REQUEST_SGX_ADDR_RANGE))
	{
		OSPCIReleaseAddrRange(psSysSpecData->hSGXPCI, SYS_SGX_ADDR_RANGE_INDEX);
		SYS_SPECIFIC_DATA_CLEAR(psSysSpecData, SYS_SPECIFIC_DATA_PCI_REQUEST_SGX_ADDR_RANGE);
	}

	if (SYS_SPECIFIC_DATA_TEST(psSysSpecData, SYS_SPECIFIC_DATA_PCI_ACQUIRE_DEV))
	{
		OSPCIReleaseDev(psSysSpecData->hSGXPCI);
		SYS_SPECIFIC_DATA_CLEAR(psSysSpecData, SYS_SPECIFIC_DATA_PCI_ACQUIRE_DEV);
	}
}

static PVRSRV_ERROR SysLocateDevices(SYS_DATA *psSysData)
{
	SYS_SPECIFIC_DATA *psSysSpecData =
		(SYS_SPECIFIC_DATA *)psSysData->pvSysSpecificData;
	IMG_UINT32 ui32BaseAddr;
	IMG_UINT32 ui32IRQ = 0;

	ui32BaseAddr = OSPCIAddrRangeStart(psSysSpecData->hSGXPCI,
					   SYS_SGX_ADDR_RANGE_INDEX);

	if (OSPCIIRQ(psSysSpecData->hSGXPCI, &ui32IRQ) != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "SysLocateDevices: couldn't get IRQ"));
		return PVRSRV_ERROR_INVALID_DEVICE;
	}

	PVR_TRACE(("SGX registers at 0x%08X (+0x%X), IRQ %u",
		   ui32BaseAddr, SGX_REGS_OFFSET, ui32IRQ));

	gsSGXDeviceMap.ui32Flags = 0;
	gsSGXDeviceMap.ui32IRQ = ui32IRQ;

	gsSGXDeviceMap.sRegsSysPBase.uiAddr = ui32BaseAddr + SGX_REGS_OFFSET;
	gsSGXDeviceMap.sRegsCpuPBase =
		SysSysPAddrToCpuPAddr(gsSGXDeviceMap.sRegsSysPBase);
	gsSGXDeviceMap.ui32RegsSize = SGX_REG_SIZE;

	/* UMA part: graphics memory is ordinary system memory. */
	gsSGXDeviceMap.sLocalMemSysPBase.uiAddr = 0;
	gsSGXDeviceMap.sLocalMemDevPBase.uiAddr = 0;
	gsSGXDeviceMap.sLocalMemCpuPBase.uiAddr = 0;
	gsSGXDeviceMap.ui32LocalMemSize = 0;

#if defined(PDUMP)
	{
		static IMG_CHAR pszPDumpDevName[] = "SGXMEM";
		gsSGXDeviceMap.pszPDumpDevName = pszPDumpDevName;
	}
#endif

	return PVRSRV_OK;
}

/*****************************************************************************/

#define VERSION_STR_MAX_LEN_TEMPLATE "SGX revision = 000.000.000"

static PVRSRV_ERROR SysCreateVersionString(SYS_DATA *psSysData)
{
	IMG_UINT32 ui32MaxStrLen;
	PVRSRV_ERROR eError;
	IMG_INT32 i32Count;
	IMG_CHAR *pszVersionString;
	IMG_UINT32 ui32SGXRevision = 0;
	IMG_VOID *pvSGXRegs;

	pvSGXRegs = OSMapPhysToLin(gsSGXDeviceMap.sRegsCpuPBase,
				   gsSGXDeviceMap.ui32RegsSize,
				   PVRSRV_HAP_KERNEL_ONLY | PVRSRV_HAP_UNCACHED,
				   IMG_NULL);
	if (pvSGXRegs != IMG_NULL)
	{
		ui32SGXRevision = OSReadHWReg(pvSGXRegs, EUR_CR_CORE_REVISION);
		OSUnMapPhysToLin(pvSGXRegs,
				 gsSGXDeviceMap.ui32RegsSize,
				 PVRSRV_HAP_KERNEL_ONLY | PVRSRV_HAP_UNCACHED,
				 IMG_NULL);
	}
	else
	{
		PVR_DPF((PVR_DBG_ERROR, "SysCreateVersionString: couldn't map SGX registers"));
	}

	ui32MaxStrLen = OSStringLength(VERSION_STR_MAX_LEN_TEMPLATE);
	eError = OSAllocMem(PVRSRV_OS_PAGEABLE_HEAP,
			    ui32MaxStrLen + 1,
			    (IMG_PVOID *)&pszVersionString,
			    IMG_NULL,
			    "Version String");
	if (eError != PVRSRV_OK)
	{
		return eError;
	}

	i32Count = OSSNPrintf(pszVersionString, ui32MaxStrLen + 1,
			      "SGX revision = %u.%u.%u",
			      (IMG_UINT)((ui32SGXRevision & EUR_CR_CORE_REVISION_MAJOR_MASK)
					 >> EUR_CR_CORE_REVISION_MAJOR_SHIFT),
			      (IMG_UINT)((ui32SGXRevision & EUR_CR_CORE_REVISION_MINOR_MASK)
					 >> EUR_CR_CORE_REVISION_MINOR_SHIFT),
			      (IMG_UINT)((ui32SGXRevision & EUR_CR_CORE_REVISION_MAINTENANCE_MASK)
					 >> EUR_CR_CORE_REVISION_MAINTENANCE_SHIFT));
	if (i32Count == -1)
	{
		OSFreeMem(PVRSRV_OS_PAGEABLE_HEAP, ui32MaxStrLen + 1,
			  pszVersionString, IMG_NULL);
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	psSysData->pszVersionString = pszVersionString;

	return PVRSRV_OK;
}

static IMG_VOID SysFreeVersionString(SYS_DATA *psSysData)
{
	if (psSysData->pszVersionString)
	{
		IMG_UINT32 ui32MaxStrLen = OSStringLength(VERSION_STR_MAX_LEN_TEMPLATE);

		OSFreeMem(PVRSRV_OS_PAGEABLE_HEAP, ui32MaxStrLen + 1,
			  psSysData->pszVersionString, IMG_NULL);
		psSysData->pszVersionString = IMG_NULL;
	}
}

/*****************************************************************************/

PVRSRV_ERROR SysInitialise(IMG_VOID)
{
	IMG_UINT32 i;
	PVRSRV_ERROR eError;
	PVRSRV_DEVICE_NODE *psDeviceNode;
	SGX_TIMING_INFORMATION *psTimingInfo;

	gpsSysData = &gsSysData;
	OSMemSet(gpsSysData, 0, sizeof(SYS_DATA));

	gpsSysData->pvSysSpecificData = &gsSysSpecificData;
	OSMemSet(&gsSysSpecificData, 0, sizeof(SYS_SPECIFIC_DATA));

	if (gpsPVRLDMDev == IMG_NULL)
	{
		PVR_DPF((PVR_DBG_ERROR, "SysInitialise: PCI device not probed"));
		gpsSysData = IMG_NULL;
		return PVRSRV_ERROR_PCI_DEVICE_NOT_FOUND;
	}
	gsSysSpecificData.psPCIDev = gpsPVRLDMDev;

	eError = OSInitEnvData(&gpsSysData->pvEnvSpecificData);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "SysInitialise: failed to set up env structure"));
		goto fail;
	}

	psTimingInfo = &gsSGXDeviceMap.sTimingInfo;
	psTimingInfo->ui32CoreClockSpeed = sgx_core_clock;
	psTimingInfo->ui32HWRecoveryFreq = SYS_SGX_HWRECOVERY_TIMEOUT_FREQ;
	psTimingInfo->ui32ActivePowManLatencyms = SYS_SGX_ACTIVE_POWER_LATENCY_MS;
	psTimingInfo->ui32uKernelFreq = SYS_SGX_PDS_TIMER_FREQ;
#if defined(SUPPORT_ACTIVE_POWER_MANAGEMENT)
	psTimingInfo->bEnableActivePM = (sgx_apm != 0) ? IMG_TRUE : IMG_FALSE;
#else
	psTimingInfo->bEnableActivePM = IMG_FALSE;
#endif

	eError = PCIInitDev(gpsSysData);
	if (eError != PVRSRV_OK)
	{
		goto fail;
	}

	gpsSysData->ui32NumDevices = SYS_DEVICE_COUNT;
	for (i = 0; i < SYS_DEVICE_COUNT; i++)
	{
		gpsSysData->sDeviceID[i].uiID = i;
		gpsSysData->sDeviceID[i].bInUse = IMG_FALSE;
	}
	gpsSysData->psDeviceNodeList = IMG_NULL;
	gpsSysData->psQueueList = IMG_NULL;

	eError = SysInitialiseCommon(gpsSysData);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "SysInitialise: SysInitialiseCommon failed"));
		goto fail;
	}

	eError = SysLocateDevices(gpsSysData);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "SysInitialise: failed to locate devices"));
		goto fail;
	}

	eError = PVRSRVRegisterDevice(gpsSysData, SGXRegisterDevice,
				      DEVICE_SGX_INTERRUPT, &gui32SGXDeviceID);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "SysInitialise: failed to register device"));
		goto fail;
	}

	/*
	 * Every heap is backed by non-contiguous system memory. There is no
	 * local/stolen graphics aperture on this part.
	 */
	psDeviceNode = gpsSysData->psDeviceNodeList;
	while (psDeviceNode)
	{
		if (psDeviceNode->sDevId.eDeviceType == PVRSRV_DEVICE_TYPE_SGX)
		{
			DEVICE_MEMORY_INFO *psDevMemoryInfo;
			DEVICE_MEMORY_HEAP_INFO *psDeviceMemoryHeap;

			psDeviceNode->psLocalDevMemArena = IMG_NULL;

			psDevMemoryInfo = &psDeviceNode->sDevMemoryInfo;
			psDeviceMemoryHeap = psDevMemoryInfo->psDeviceMemoryHeap;

			for (i = 0; i < psDevMemoryInfo->ui32HeapCount; i++)
			{
				psDeviceMemoryHeap[i].ui32Attribs |=
					PVRSRV_BACKINGSTORE_SYSMEM_NONCONTIG;
			}

			gpsSGXDevNode = psDeviceNode;

			eError = PVRSRVInitialiseDevice(gui32SGXDeviceID);
			if (eError != PVRSRV_OK)
			{
				PVR_DPF((PVR_DBG_ERROR, "SysInitialise: failed to initialise SGX"));
				goto fail;
			}
			SYS_SPECIFIC_DATA_SET(&gsSysSpecificData,
					      SYS_SPECIFIC_DATA_SGX_INITIALISED);
		}
		psDeviceNode = psDeviceNode->psNext;
	}

	return PVRSRV_OK;

fail:
	SysDeinitialise(gpsSysData);
	gpsSysData = IMG_NULL;
	return eError;
}

PVRSRV_ERROR SysFinalise(IMG_VOID)
{
	PVRSRV_ERROR eError;

	eError = OSInstallMISR(gpsSysData);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "SysFinalise: OSInstallMISR failed"));
		return eError;
	}
	SYS_SPECIFIC_DATA_SET(&gsSysSpecificData, SYS_SPECIFIC_DATA_MISR_INSTALLED);

	eError = SysCreateVersionString(gpsSysData);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "SysFinalise: failed to create version string"));
	}
	else
	{
		PVR_LOG(("SysFinalise: %s", gpsSysData->pszVersionString));
	}

	return eError;
}

PVRSRV_ERROR SysDeinitialise(SYS_DATA *psSysData)
{
	PVRSRV_ERROR eError;
	SYS_SPECIFIC_DATA *psSysSpecData;

	if (psSysData == IMG_NULL)
	{
		return PVRSRV_OK;
	}

	psSysSpecData = (SYS_SPECIFIC_DATA *)psSysData->pvSysSpecificData;

	if (SYS_SPECIFIC_DATA_TEST(psSysSpecData, SYS_SPECIFIC_DATA_MISR_INSTALLED))
	{
		eError = OSUninstallMISR(psSysData);
		if (eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "SysDeinitialise: OSUninstallMISR failed"));
			return eError;
		}
		SYS_SPECIFIC_DATA_CLEAR(psSysSpecData, SYS_SPECIFIC_DATA_MISR_INSTALLED);
	}

	if (SYS_SPECIFIC_DATA_TEST(psSysSpecData, SYS_SPECIFIC_DATA_SGX_INITIALISED))
	{
		eError = PVRSRVDeinitialiseDevice(gui32SGXDeviceID);
		if (eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "SysDeinitialise: failed to de-init SGX"));
			return eError;
		}
		SYS_SPECIFIC_DATA_CLEAR(psSysSpecData, SYS_SPECIFIC_DATA_SGX_INITIALISED);
	}

	SysFreeVersionString(psSysData);

	PCIDeInitDev(psSysData);

	eError = OSDeInitEnvData(psSysData->pvEnvSpecificData);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "SysDeinitialise: failed to de-init env structure"));
		return eError;
	}

	SysDeinitialiseCommon(gpsSysData);

	if (SYS_SPECIFIC_DATA_TEST(psSysSpecData, SYS_SPECIFIC_DATA_PDUMP_INIT))
	{
		PDUMPDEINIT();
		SYS_SPECIFIC_DATA_CLEAR(psSysSpecData, SYS_SPECIFIC_DATA_PDUMP_INIT);
	}

	gpsSysData = IMG_NULL;

	return PVRSRV_OK;
}

/*****************************************************************************/

IMG_UINT32 SysGetInterruptSource(SYS_DATA *psSysData,
				 PVRSRV_DEVICE_NODE *psDeviceNode)
{
	PVR_UNREFERENCED_PARAMETER(psSysData);
	PVR_UNREFERENCED_PARAMETER(psDeviceNode);

	/*
	 * The SGX owns its PCI interrupt line outright on this part -- there is
	 * no shared SoC interrupt-identity register to demultiplex, so the SGX
	 * device layer reads EUR_CR_EVENT_STATUS itself. Returning 0 tells the
	 * core "no system-level source information", which is what Cedarview
	 * does too.
	 */
	return 0;
}

IMG_VOID SysClearInterrupts(SYS_DATA *psSysData, IMG_UINT32 ui32ClearBits)
{
	PVR_UNREFERENCED_PARAMETER(psSysData);
	PVR_UNREFERENCED_PARAMETER(ui32ClearBits);
}

PVRSRV_ERROR SysGetDeviceMemoryMap(PVRSRV_DEVICE_TYPE eDeviceType,
				   IMG_VOID **ppvDeviceMap)
{
	switch (eDeviceType)
	{
	case PVRSRV_DEVICE_TYPE_SGX:
		*ppvDeviceMap = (IMG_VOID *)&gsSGXDeviceMap;
		break;
	default:
		PVR_DPF((PVR_DBG_ERROR, "SysGetDeviceMemoryMap: unsupported device type"));
		return PVRSRV_ERROR_INVALID_DEVICE;
	}

	return PVRSRV_OK;
}

/*
 * CE5300 is a flat, uniform-memory x86 part: CPU physical, system physical
 * and device physical addresses are all the same number.
 */
IMG_DEV_PHYADDR SysCpuPAddrToDevPAddr(PVRSRV_DEVICE_TYPE eDeviceType,
				      IMG_CPU_PHYADDR CpuPAddr)
{
	IMG_DEV_PHYADDR DevPAddr;

	PVR_UNREFERENCED_PARAMETER(eDeviceType);
	DevPAddr.uiAddr = CpuPAddr.uiAddr;

	return DevPAddr;
}

IMG_CPU_PHYADDR SysSysPAddrToCpuPAddr(IMG_SYS_PHYADDR sys_paddr)
{
	IMG_CPU_PHYADDR cpu_paddr;

	cpu_paddr.uiAddr = sys_paddr.uiAddr;

	return cpu_paddr;
}

IMG_SYS_PHYADDR SysCpuPAddrToSysPAddr(IMG_CPU_PHYADDR cpu_paddr)
{
	IMG_SYS_PHYADDR sys_paddr;

	sys_paddr.uiAddr = cpu_paddr.uiAddr;

	return sys_paddr;
}

IMG_DEV_PHYADDR SysSysPAddrToDevPAddr(PVRSRV_DEVICE_TYPE eDeviceType,
				      IMG_SYS_PHYADDR SysPAddr)
{
	IMG_DEV_PHYADDR DevPAddr;

	PVR_UNREFERENCED_PARAMETER(eDeviceType);
	DevPAddr.uiAddr = SysPAddr.uiAddr;

	return DevPAddr;
}

IMG_SYS_PHYADDR SysDevPAddrToSysPAddr(PVRSRV_DEVICE_TYPE eDeviceType,
				      IMG_DEV_PHYADDR DevPAddr)
{
	IMG_SYS_PHYADDR SysPAddr;

	PVR_UNREFERENCED_PARAMETER(eDeviceType);
	SysPAddr.uiAddr = DevPAddr.uiAddr;

	return SysPAddr;
}

IMG_VOID SysRegisterExternalDevice(PVRSRV_DEVICE_NODE *psDeviceNode)
{
	psDeviceNode->ui32SOCInterruptBit = DEVICE_DISP_INTERRUPT;
}

IMG_VOID SysRemoveExternalDevice(PVRSRV_DEVICE_NODE *psDeviceNode)
{
	PVR_UNREFERENCED_PARAMETER(psDeviceNode);
}

PVRSRV_ERROR SysOEMFunction(IMG_UINT32 ui32ID,
			    IMG_VOID *pvIn,
			    IMG_UINT32 ulInSize,
			    IMG_VOID *pvOut,
			    IMG_UINT32 ulOutSize)
{
	PVR_UNREFERENCED_PARAMETER(ui32ID);
	PVR_UNREFERENCED_PARAMETER(pvIn);
	PVR_UNREFERENCED_PARAMETER(ulInSize);
	PVR_UNREFERENCED_PARAMETER(pvOut);

	if (ulOutSize != 0)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	return PVRSRV_OK;
}

PVRSRV_ERROR SysResetDevice(IMG_UINT32 ui32DeviceIndex)
{
	PVR_UNREFERENCED_PARAMETER(ui32DeviceIndex);

	/* The SGX device layer drives EUR_CR_SOFT_RESET itself (sgxreset.c). */
	return PVRSRV_OK;
}

/*****************************************************************************/

static PVRSRV_ERROR SysMapInRegisters(IMG_VOID)
{
	PVRSRV_DEVICE_NODE *psDeviceNodeList = gpsSysData->psDeviceNodeList;

	while (psDeviceNodeList)
	{
		if (psDeviceNodeList->sDevId.eDeviceType == PVRSRV_DEVICE_TYPE_SGX)
		{
			PVRSRV_SGXDEV_INFO *psDevInfo =
				(PVRSRV_SGXDEV_INFO *)psDeviceNodeList->pvDevice;

			if (SYS_SPECIFIC_DATA_TEST(&gsSysSpecificData,
						   SYS_SPECIFIC_DATA_PM_UNMAP_SGX_REGS))
			{
				psDevInfo->pvRegsBaseKM =
					OSMapPhysToLin(gsSGXDeviceMap.sRegsCpuPBase,
						       gsSGXDeviceMap.ui32RegsSize,
						       PVRSRV_HAP_KERNEL_ONLY | PVRSRV_HAP_UNCACHED,
						       IMG_NULL);
				if (!psDevInfo->pvRegsBaseKM)
				{
					PVR_DPF((PVR_DBG_ERROR,
						 "SysMapInRegisters: failed to map SGX registers"));
					return PVRSRV_ERROR_BAD_MAPPING;
				}
				SYS_SPECIFIC_DATA_CLEAR(&gsSysSpecificData,
							SYS_SPECIFIC_DATA_PM_UNMAP_SGX_REGS);
			}

			psDevInfo->ui32RegSize = gsSGXDeviceMap.ui32RegsSize;
			psDevInfo->sRegsPhysBase = gsSGXDeviceMap.sRegsSysPBase;
		}
		psDeviceNodeList = psDeviceNodeList->psNext;
	}

	return PVRSRV_OK;
}

static PVRSRV_ERROR SysUnmapRegisters(IMG_VOID)
{
	PVRSRV_DEVICE_NODE *psDeviceNodeList = gpsSysData->psDeviceNodeList;

	while (psDeviceNodeList)
	{
		if (psDeviceNodeList->sDevId.eDeviceType == PVRSRV_DEVICE_TYPE_SGX)
		{
			PVRSRV_SGXDEV_INFO *psDevInfo =
				(PVRSRV_SGXDEV_INFO *)psDeviceNodeList->pvDevice;

			if (psDevInfo->pvRegsBaseKM)
			{
				OSUnMapPhysToLin(psDevInfo->pvRegsBaseKM,
						 gsSGXDeviceMap.ui32RegsSize,
						 PVRSRV_HAP_KERNEL_ONLY | PVRSRV_HAP_UNCACHED,
						 IMG_NULL);
				SYS_SPECIFIC_DATA_SET(&gsSysSpecificData,
						      SYS_SPECIFIC_DATA_PM_UNMAP_SGX_REGS);
			}

			psDevInfo->pvRegsBaseKM = IMG_NULL;
			psDevInfo->ui32RegSize = 0;
			psDevInfo->sRegsPhysBase.uiAddr = 0;
		}
		psDeviceNodeList = psDeviceNodeList->psNext;
	}

	return PVRSRV_OK;
}

/*
 * Power management.
 *
 * Deliberately minimal for bring-up: we unmap and remap the register window
 * across a D3 transition and otherwise leave the PCI device alone. Cedarview's
 * layer additionally saves and restores MSI configuration and a pile of
 * Poulsbo-specific registers (BSM, VBT) through the psb driver; none of that
 * plumbing exists here, and on CE5300 the firmware leaves the SGX powered at
 * boot. Revisit once the driver renders reliably.
 */
PVRSRV_ERROR SysSystemPrePowerState(PVRSRV_SYS_POWER_STATE eNewPowerState)
{
	PVRSRV_ERROR eError = PVRSRV_OK;

	if (eNewPowerState == PVRSRV_SYS_POWER_STATE_D3)
	{
		PVR_TRACE(("SysSystemPrePowerState: entering D3"));

		if (SYS_SPECIFIC_DATA_TEST(&gsSysSpecificData, SYS_SPECIFIC_DATA_IRQ_ENABLED))
		{
			OSPCIReleaseDev(gsSysSpecificData.hSGXPCI);
			SYS_SPECIFIC_DATA_CLEAR(&gsSysSpecificData, SYS_SPECIFIC_DATA_IRQ_ENABLED);
		}

		eError = SysUnmapRegisters();
		if (eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "SysSystemPrePowerState: SysUnmapRegisters failed"));
		}
	}

	return eError;
}

PVRSRV_ERROR SysSystemPostPowerState(PVRSRV_SYS_POWER_STATE eNewPowerState)
{
	PVRSRV_ERROR eError = PVRSRV_OK;

	if (eNewPowerState == PVRSRV_SYS_POWER_STATE_D0)
	{
		PVR_TRACE(("SysSystemPostPowerState: leaving D3"));

		eError = SysMapInRegisters();
		if (eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "SysSystemPostPowerState: SysMapInRegisters failed"));
		}
	}

	return eError;
}

PVRSRV_ERROR SysDevicePrePowerState(IMG_UINT32 ui32DeviceIndex,
				    PVRSRV_DEV_POWER_STATE eNewPowerState,
				    PVRSRV_DEV_POWER_STATE eCurrentPowerState)
{
	PVR_UNREFERENCED_PARAMETER(eCurrentPowerState);

	if (ui32DeviceIndex != gui32SGXDeviceID)
	{
		return PVRSRV_OK;
	}

	if (eNewPowerState == PVRSRV_DEV_POWER_STATE_OFF)
	{
		PVR_DPF((PVR_DBG_MESSAGE, "SysDevicePrePowerState: SGX off"));
	}

	return PVRSRV_OK;
}

PVRSRV_ERROR SysDevicePostPowerState(IMG_UINT32 ui32DeviceIndex,
				     PVRSRV_DEV_POWER_STATE eNewPowerState,
				     PVRSRV_DEV_POWER_STATE eCurrentPowerState)
{
	PVR_UNREFERENCED_PARAMETER(eNewPowerState);

	if (ui32DeviceIndex != gui32SGXDeviceID)
	{
		return PVRSRV_OK;
	}

	if (eCurrentPowerState == PVRSRV_DEV_POWER_STATE_OFF)
	{
		PVR_DPF((PVR_DBG_MESSAGE, "SysDevicePostPowerState: SGX on"));
	}

	return PVRSRV_OK;
}
