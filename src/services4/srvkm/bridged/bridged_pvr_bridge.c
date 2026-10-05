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



#include <linux/stddef.h>
#if defined(CONFIG_COMPAT)
#include <linux/compat.h>	/* in_compat_syscall() for the 32-on-64 bridge */
#include <linux/build_bug.h>	/* BUILD_BUG_ON for the DEVINITPART2 struct-size pin */
#endif

#include "img_defs.h"
#include "services.h"
#include "pvr_bridge_km.h"
#include "pvr_debug.h"
#include "ra.h"
#include "pvr_bridge.h"
#if defined(SUPPORT_SGX)
#include "sgx_bridge.h"
#endif
#if defined(SUPPORT_VGX)
#include "vgx_bridge.h"
#endif
#if defined(SUPPORT_MSVDX)
#include "msvdx_bridge.h"
#endif
#include "perproc.h"
#include "device.h"
#include "buffer_manager.h"

#include "pdump_km.h"
#include "syscommon.h"

#include "bridged_pvr_bridge.h"
#if defined(SUPPORT_SGX)
#include "bridged_sgx_bridge.h"
#endif
#if defined(SUPPORT_VGX)
#include "bridged_vgx_bridge.h"
#endif
#if defined(SUPPORT_MSVDX)
#include "bridged_msvdx_bridge.h"
#endif

#include "env_data.h"

#if defined (__linux__) || defined(__QNXNTO__)
#include "mmap.h"
#endif


#include "srvkm.h"

PVRSRV_BRIDGE_DISPATCH_TABLE_ENTRY g_BridgeDispatchTable[BRIDGE_DISPATCH_TABLE_ENTRY_COUNT];

#if defined(DEBUG_BRIDGE_KM)
PVRSRV_BRIDGE_GLOBAL_STATS g_BridgeGlobalStats;
#endif

#if defined(PVR_SECURE_HANDLES) || defined (SUPPORT_SID_INTERFACE)
static IMG_BOOL abSharedDeviceMemHeap[PVRSRV_MAX_CLIENT_HEAPS];
static IMG_BOOL *pbSharedDeviceMemHeap = abSharedDeviceMemHeap;
#else
static IMG_BOOL *pbSharedDeviceMemHeap = (IMG_BOOL*)IMG_NULL;
#endif


#if defined(DEBUG_BRIDGE_KM)
PVRSRV_ERROR
CopyFromUserWrapper(PVRSRV_PER_PROCESS_DATA *pProcData,
                    IMG_UINT32 ui32BridgeID,
                    IMG_VOID *pvDest,
                    IMG_VOID *pvSrc,
                    IMG_UINT32 ui32Size)
{
	PVRSRV_ERROR ret;
	ret = OSCopyFromUser(pProcData, pvDest, pvSrc, ui32Size);

	if (ret == PVRSRV_OK) {
		g_BridgeDispatchTable[ui32BridgeID].ui32CopyFromUserTotalBytes += ui32Size;
		g_BridgeGlobalStats.ui32TotalCopyFromUserBytes += ui32Size;
	}
	return ret;
}
PVRSRV_ERROR
CopyToUserWrapper(PVRSRV_PER_PROCESS_DATA *pProcData,
                  IMG_UINT32 ui32BridgeID,
                  IMG_VOID *pvDest,
                  IMG_VOID *pvSrc,
                  IMG_UINT32 ui32Size)
{
	PVRSRV_ERROR ret;
	ret = OSCopyToUser(pProcData, pvDest, pvSrc, ui32Size);
	if (ret == PVRSRV_OK) {
		g_BridgeDispatchTable[ui32BridgeID].ui32CopyToUserTotalBytes += ui32Size;
		g_BridgeGlobalStats.ui32TotalCopyToUserBytes += ui32Size;
	}
	return ret;
}
#endif


static IMG_INT
PVRSRVEnumerateDevicesBW(IMG_UINT32 ui32BridgeID,
						 IMG_VOID *psBridgeIn,
						 PVRSRV_BRIDGE_OUT_ENUMDEVICE *psEnumDeviceOUT,
						 PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_ENUM_DEVICES);

    	PVR_UNREFERENCED_PARAMETER(psPerProc);
    	PVR_UNREFERENCED_PARAMETER(psBridgeIn);

	psEnumDeviceOUT->eError =
		PVRSRVEnumerateDevicesKM(&psEnumDeviceOUT->ui32NumDevices,
								 psEnumDeviceOUT->asDeviceIdentifier);
	return 0;
}

static IMG_INT
PVRSRVAcquireDeviceDataBW(IMG_UINT32 ui32BridgeID,
						  PVRSRV_BRIDGE_IN_ACQUIRE_DEVICEINFO *psAcquireDevInfoIN,
						  PVRSRV_BRIDGE_OUT_ACQUIRE_DEVICEINFO *psAcquireDevInfoOUT,
						  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_HANDLE hDevCookieInt;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_ACQUIRE_DEVICEINFO);

	psAcquireDevInfoOUT->eError =
		PVRSRVAcquireDeviceDataKM(psAcquireDevInfoIN->uiDevIndex,
								  psAcquireDevInfoIN->eDeviceType,
								  &hDevCookieInt);
	if(psAcquireDevInfoOUT->eError != PVRSRV_OK)
	{
		return 0;
	}
    
	psAcquireDevInfoOUT->eError =
		PVRSRVAllocHandle(psPerProc->psHandleBase,
						  &psAcquireDevInfoOUT->hDevCookie,
						  hDevCookieInt,
						  PVRSRV_HANDLE_TYPE_DEV_NODE,
						  PVRSRV_HANDLE_ALLOC_FLAG_SHARED);

	return 0;
}


static IMG_INT
PVRSRVCreateDeviceMemContextBW(IMG_UINT32 ui32BridgeID,
                               PVRSRV_BRIDGE_IN_CREATE_DEVMEMCONTEXT *psCreateDevMemContextIN,
                               PVRSRV_BRIDGE_OUT_CREATE_DEVMEMCONTEXT *psCreateDevMemContextOUT,
                               PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_HANDLE hDevCookieInt;
	IMG_HANDLE hDevMemContextInt;
	IMG_UINT32 i;
	IMG_BOOL bCreated;
#if defined (SUPPORT_SID_INTERFACE)
	PVRSRV_HEAP_INFO_KM asHeapInfo[PVRSRV_MAX_CLIENT_HEAPS];
#endif

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_CREATE_DEVMEMCONTEXT);


	NEW_HANDLE_BATCH_OR_ERROR(psCreateDevMemContextOUT->eError, psPerProc, PVRSRV_MAX_CLIENT_HEAPS + 1)

	psCreateDevMemContextOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase, &hDevCookieInt,
				psCreateDevMemContextIN->hDevCookie,
				PVRSRV_HANDLE_TYPE_DEV_NODE);

	if(psCreateDevMemContextOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psCreateDevMemContextOUT->eError =
		PVRSRVCreateDeviceMemContextKM(hDevCookieInt,
									   psPerProc,
									   &hDevMemContextInt,
									   &psCreateDevMemContextOUT->ui32ClientHeapCount,
#if defined (SUPPORT_SID_INTERFACE)
									   &asHeapInfo[0],
#else
									   &psCreateDevMemContextOUT->sHeapInfo[0],
#endif
									   &bCreated,
									   pbSharedDeviceMemHeap);

	if(psCreateDevMemContextOUT->eError != PVRSRV_OK)
	{
		return 0;
	}


	if(bCreated)
	{
		PVRSRVAllocHandleNR(psPerProc->psHandleBase,
						  &psCreateDevMemContextOUT->hDevMemContext,
						  hDevMemContextInt,
						  PVRSRV_HANDLE_TYPE_DEV_MEM_CONTEXT,
						  PVRSRV_HANDLE_ALLOC_FLAG_NONE);
	}
	else
	{
		psCreateDevMemContextOUT->eError =
			PVRSRVFindHandle(psPerProc->psHandleBase,
							 &psCreateDevMemContextOUT->hDevMemContext,
							 hDevMemContextInt,
							 PVRSRV_HANDLE_TYPE_DEV_MEM_CONTEXT);
		if(psCreateDevMemContextOUT->eError != PVRSRV_OK)
		{
			return 0;
		}
	}

	for(i = 0; i < psCreateDevMemContextOUT->ui32ClientHeapCount; i++)
	{
#if defined (SUPPORT_SID_INTERFACE)
		IMG_SID hDevMemHeapExt;
#else
		IMG_HANDLE hDevMemHeapExt;
#endif

#if defined(PVR_SECURE_HANDLES) || defined (SUPPORT_SID_INTERFACE)
		if(abSharedDeviceMemHeap[i])
#endif
		{

#if defined (SUPPORT_SID_INTERFACE)
			PVRSRVAllocHandleNR(psPerProc->psHandleBase,
								&hDevMemHeapExt,
								asHeapInfo[i].hDevMemHeap,
								PVRSRV_HANDLE_TYPE_DEV_MEM_HEAP,
								PVRSRV_HANDLE_ALLOC_FLAG_SHARED);
#else
			PVRSRVAllocHandleNR(psPerProc->psHandleBase, &hDevMemHeapExt,
								psCreateDevMemContextOUT->sHeapInfo[i].hDevMemHeap,
								PVRSRV_HANDLE_TYPE_DEV_MEM_HEAP,
								PVRSRV_HANDLE_ALLOC_FLAG_SHARED);
#endif
		}
#if defined(PVR_SECURE_HANDLES) || defined (SUPPORT_SID_INTERFACE)
		else
		{

			if(bCreated)
			{
#if defined (SUPPORT_SID_INTERFACE)
				PVRSRVAllocSubHandleNR(psPerProc->psHandleBase,
									   &hDevMemHeapExt,
									   asHeapInfo[i].hDevMemHeap,
									   PVRSRV_HANDLE_TYPE_DEV_MEM_HEAP,
									   PVRSRV_HANDLE_ALLOC_FLAG_NONE,
									   psCreateDevMemContextOUT->hDevMemContext);
#else
				PVRSRVAllocSubHandleNR(psPerProc->psHandleBase, &hDevMemHeapExt,
									 psCreateDevMemContextOUT->sHeapInfo[i].hDevMemHeap,
									 PVRSRV_HANDLE_TYPE_DEV_MEM_HEAP,
									 PVRSRV_HANDLE_ALLOC_FLAG_NONE,
									 psCreateDevMemContextOUT->hDevMemContext);
#endif
			}
			else
			{
				psCreateDevMemContextOUT->eError =
					PVRSRVFindHandle(psPerProc->psHandleBase,
									 &hDevMemHeapExt,
#if defined (SUPPORT_SID_INTERFACE)
									 asHeapInfo[i].hDevMemHeap,
#else
									 psCreateDevMemContextOUT->sHeapInfo[i].hDevMemHeap,
#endif
									 PVRSRV_HANDLE_TYPE_DEV_MEM_HEAP);
				if(psCreateDevMemContextOUT->eError != PVRSRV_OK)
				{
					return 0;
				}
			}
		}
#endif
		psCreateDevMemContextOUT->sHeapInfo[i].hDevMemHeap = hDevMemHeapExt;
#if defined (SUPPORT_SID_INTERFACE)
		psCreateDevMemContextOUT->sHeapInfo[i].ui32HeapID       = asHeapInfo[i].ui32HeapID;
		psCreateDevMemContextOUT->sHeapInfo[i].sDevVAddrBase    = asHeapInfo[i].sDevVAddrBase;
		psCreateDevMemContextOUT->sHeapInfo[i].ui32HeapByteSize = asHeapInfo[i].ui32HeapByteSize;
		psCreateDevMemContextOUT->sHeapInfo[i].ui32Attribs      = asHeapInfo[i].ui32Attribs;
		psCreateDevMemContextOUT->sHeapInfo[i].ui32XTileStride  = asHeapInfo[i].ui32XTileStride;
#endif
	}

	COMMIT_HANDLE_BATCH_OR_ERROR(psCreateDevMemContextOUT->eError, psPerProc)

	return 0;
}

static IMG_INT
PVRSRVDestroyDeviceMemContextBW(IMG_UINT32 ui32BridgeID,
								PVRSRV_BRIDGE_IN_DESTROY_DEVMEMCONTEXT *psDestroyDevMemContextIN,
								PVRSRV_BRIDGE_RETURN *psRetOUT,
								PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_HANDLE hDevCookieInt;
	IMG_HANDLE hDevMemContextInt;
	IMG_BOOL bDestroyed;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_DESTROY_DEVMEMCONTEXT);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase, &hDevCookieInt,
						   psDestroyDevMemContextIN->hDevCookie,
						   PVRSRV_HANDLE_TYPE_DEV_NODE);

	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase, &hDevMemContextInt,
						   psDestroyDevMemContextIN->hDevMemContext,
						   PVRSRV_HANDLE_TYPE_DEV_MEM_CONTEXT);

	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PVRSRVDestroyDeviceMemContextKM(hDevCookieInt, hDevMemContextInt, &bDestroyed);

	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	if(bDestroyed)
	{
		psRetOUT->eError =
			PVRSRVReleaseHandle(psPerProc->psHandleBase,
								psDestroyDevMemContextIN->hDevMemContext,
								PVRSRV_HANDLE_TYPE_DEV_MEM_CONTEXT);
	}

	return 0;
}


static IMG_INT
PVRSRVGetDeviceMemHeapInfoBW(IMG_UINT32 ui32BridgeID,
							   PVRSRV_BRIDGE_IN_GET_DEVMEM_HEAPINFO *psGetDevMemHeapInfoIN,
							   PVRSRV_BRIDGE_OUT_GET_DEVMEM_HEAPINFO *psGetDevMemHeapInfoOUT,
							   PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_HANDLE hDevCookieInt;
	IMG_HANDLE hDevMemContextInt;
	IMG_UINT32 i;
#if defined (SUPPORT_SID_INTERFACE)
	PVRSRV_HEAP_INFO_KM asHeapInfo[PVRSRV_MAX_CLIENT_HEAPS];
#endif

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_GET_DEVMEM_HEAPINFO);

	NEW_HANDLE_BATCH_OR_ERROR(psGetDevMemHeapInfoOUT->eError, psPerProc, PVRSRV_MAX_CLIENT_HEAPS)

	psGetDevMemHeapInfoOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase, &hDevCookieInt,
						   psGetDevMemHeapInfoIN->hDevCookie,
						   PVRSRV_HANDLE_TYPE_DEV_NODE);

	if(psGetDevMemHeapInfoOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psGetDevMemHeapInfoOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase, &hDevMemContextInt,
						   psGetDevMemHeapInfoIN->hDevMemContext,
						   PVRSRV_HANDLE_TYPE_DEV_MEM_CONTEXT);

	if(psGetDevMemHeapInfoOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psGetDevMemHeapInfoOUT->eError =
		PVRSRVGetDeviceMemHeapInfoKM(hDevCookieInt,
									   hDevMemContextInt,
									   &psGetDevMemHeapInfoOUT->ui32ClientHeapCount,
#if defined (SUPPORT_SID_INTERFACE)
									   &asHeapInfo[0],
#else
									   &psGetDevMemHeapInfoOUT->sHeapInfo[0],
#endif
									   pbSharedDeviceMemHeap);

	if(psGetDevMemHeapInfoOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	for(i = 0; i < psGetDevMemHeapInfoOUT->ui32ClientHeapCount; i++)
	{
#if defined (SUPPORT_SID_INTERFACE)
		IMG_SID hDevMemHeapExt;
#else
		IMG_HANDLE hDevMemHeapExt;
#endif

#if defined(PVR_SECURE_HANDLES) || defined (SUPPORT_SID_INTERFACE)
		if(abSharedDeviceMemHeap[i])
#endif
		{

#if defined (SUPPORT_SID_INTERFACE)
			PVRSRVAllocHandleNR(psPerProc->psHandleBase,
								&hDevMemHeapExt,
								asHeapInfo[i].hDevMemHeap,
								PVRSRV_HANDLE_TYPE_DEV_MEM_HEAP,
								PVRSRV_HANDLE_ALLOC_FLAG_SHARED);
#else
			PVRSRVAllocHandleNR(psPerProc->psHandleBase, &hDevMemHeapExt,
							  psGetDevMemHeapInfoOUT->sHeapInfo[i].hDevMemHeap,
							  PVRSRV_HANDLE_TYPE_DEV_MEM_HEAP,
							  PVRSRV_HANDLE_ALLOC_FLAG_SHARED);
#endif
		}
#if defined(PVR_SECURE_HANDLES) || defined (SUPPORT_SID_INTERFACE)
		else
		{

			psGetDevMemHeapInfoOUT->eError =
				PVRSRVFindHandle(psPerProc->psHandleBase,
								 &hDevMemHeapExt,
#if defined (SUPPORT_SID_INTERFACE)
								 asHeapInfo[i].hDevMemHeap,
#else
								 psGetDevMemHeapInfoOUT->sHeapInfo[i].hDevMemHeap,
#endif
								 PVRSRV_HANDLE_TYPE_DEV_MEM_HEAP);
			if(psGetDevMemHeapInfoOUT->eError != PVRSRV_OK)
			{
				return 0;
			}
		}
#endif
		psGetDevMemHeapInfoOUT->sHeapInfo[i].hDevMemHeap = hDevMemHeapExt;
#if defined (SUPPORT_SID_INTERFACE)
		psGetDevMemHeapInfoOUT->sHeapInfo[i].ui32HeapID       = asHeapInfo[i].ui32HeapID;
		psGetDevMemHeapInfoOUT->sHeapInfo[i].sDevVAddrBase    = asHeapInfo[i].sDevVAddrBase;
		psGetDevMemHeapInfoOUT->sHeapInfo[i].ui32HeapByteSize = asHeapInfo[i].ui32HeapByteSize;
		psGetDevMemHeapInfoOUT->sHeapInfo[i].ui32Attribs      = asHeapInfo[i].ui32Attribs;
		psGetDevMemHeapInfoOUT->sHeapInfo[i].ui32XTileStride  = asHeapInfo[i].ui32XTileStride;
#endif
	}

	COMMIT_HANDLE_BATCH_OR_ERROR(psGetDevMemHeapInfoOUT->eError, psPerProc)

	return 0;
}


#if defined(OS_PVRSRV_ALLOC_DEVICE_MEM_BW)
IMG_INT
PVRSRVAllocDeviceMemBW(IMG_UINT32 ui32BridgeID,
					   PVRSRV_BRIDGE_IN_ALLOCDEVICEMEM *psAllocDeviceMemIN,
					   PVRSRV_BRIDGE_OUT_ALLOCDEVICEMEM *psAllocDeviceMemOUT,
					   PVRSRV_PER_PROCESS_DATA *psPerProc);
#else
static IMG_INT
PVRSRVAllocDeviceMemBW(IMG_UINT32 ui32BridgeID,
					   PVRSRV_BRIDGE_IN_ALLOCDEVICEMEM *psAllocDeviceMemIN,
					   PVRSRV_BRIDGE_OUT_ALLOCDEVICEMEM *psAllocDeviceMemOUT,
					   PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_KERNEL_MEM_INFO *psMemInfo;
	IMG_HANDLE hDevCookieInt;
	IMG_HANDLE hDevMemHeapInt;
	IMG_UINT32 ui32ShareIndex;
	IMG_BOOL bUseShareMemWorkaround;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_ALLOC_DEVICEMEM);

	NEW_HANDLE_BATCH_OR_ERROR(psAllocDeviceMemOUT->eError, psPerProc, 2)

	psAllocDeviceMemOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase, &hDevCookieInt,
						   psAllocDeviceMemIN->hDevCookie,
						   PVRSRV_HANDLE_TYPE_DEV_NODE);

	if(psAllocDeviceMemOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psAllocDeviceMemOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase, &hDevMemHeapInt,
						   psAllocDeviceMemIN->hDevMemHeap,
						   PVRSRV_HANDLE_TYPE_DEV_MEM_HEAP);

	if(psAllocDeviceMemOUT->eError != PVRSRV_OK)
	{
		return 0;
	}



	bUseShareMemWorkaround = ((psAllocDeviceMemIN->ui32Attribs & PVRSRV_MEM_XPROC) != 0) ? IMG_TRUE : IMG_FALSE;
	ui32ShareIndex = 7654321; 

	if (bUseShareMemWorkaround)
	{



		psAllocDeviceMemOUT->eError =
			BM_XProcWorkaroundFindNewBufferAndSetShareIndex(&ui32ShareIndex);
		if(psAllocDeviceMemOUT->eError != PVRSRV_OK)
		{
			return 0;
		}
	}

	psAllocDeviceMemOUT->eError =
		PVRSRVAllocDeviceMemKM(hDevCookieInt,
							   psPerProc,
							   hDevMemHeapInt,
							   psAllocDeviceMemIN->ui32Attribs,
							   psAllocDeviceMemIN->ui32Size,
							   psAllocDeviceMemIN->ui32Alignment,
							   &psMemInfo,
							   "" );

	if (bUseShareMemWorkaround)
	{
		PVR_ASSERT(ui32ShareIndex != 7654321);
		BM_XProcWorkaroundUnsetShareIndex(ui32ShareIndex);
	}

	if(psAllocDeviceMemOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psMemInfo->sShareMemWorkaround.bInUse = bUseShareMemWorkaround;
	if (bUseShareMemWorkaround)
	{
		PVR_ASSERT(ui32ShareIndex != 7654321);
		psMemInfo->sShareMemWorkaround.ui32ShareIndex = ui32ShareIndex;
		psMemInfo->sShareMemWorkaround.hDevCookieInt = hDevCookieInt;
		psMemInfo->sShareMemWorkaround.ui32OrigReqAttribs = psAllocDeviceMemIN->ui32Attribs;
		psMemInfo->sShareMemWorkaround.ui32OrigReqSize = (IMG_UINT32)psAllocDeviceMemIN->ui32Size;
		psMemInfo->sShareMemWorkaround.ui32OrigReqAlignment = (IMG_UINT32)psAllocDeviceMemIN->ui32Alignment;
	}

	OSMemSet(&psAllocDeviceMemOUT->sClientMemInfo,
			0,
			sizeof(psAllocDeviceMemOUT->sClientMemInfo));

	psAllocDeviceMemOUT->sClientMemInfo.pvLinAddrKM =
			psMemInfo->pvLinAddrKM;

#if defined (__linux__)
	psAllocDeviceMemOUT->sClientMemInfo.pvLinAddr = 0;
#else
	psAllocDeviceMemOUT->sClientMemInfo.pvLinAddr = psMemInfo->pvLinAddrKM;
#endif
	psAllocDeviceMemOUT->sClientMemInfo.sDevVAddr = psMemInfo->sDevVAddr;
	psAllocDeviceMemOUT->sClientMemInfo.ui32Flags = psMemInfo->ui32Flags;
	psAllocDeviceMemOUT->sClientMemInfo.uAllocSize = psMemInfo->uAllocSize;
#if defined (SUPPORT_SID_INTERFACE)
#else
	psAllocDeviceMemOUT->sClientMemInfo.hMappingInfo = psMemInfo->sMemBlk.hOSMemHandle;
#endif

	PVRSRVAllocHandleNR(psPerProc->psHandleBase,
					  &psAllocDeviceMemOUT->sClientMemInfo.hKernelMemInfo,
					  psMemInfo,
					  PVRSRV_HANDLE_TYPE_MEM_INFO,
					  PVRSRV_HANDLE_ALLOC_FLAG_NONE);

#if defined (SUPPORT_SID_INTERFACE)
	PVR_ASSERT(psAllocDeviceMemOUT->sClientMemInfo.hKernelMemInfo != 0);

	if (psMemInfo->sMemBlk.hOSMemHandle != IMG_NULL)
	{
		PVRSRVAllocSubHandleNR(psPerProc->psHandleBase,
							   &psAllocDeviceMemOUT->sClientMemInfo.hMappingInfo,
							   psMemInfo->sMemBlk.hOSMemHandle,
							   PVRSRV_HANDLE_TYPE_MEM_INFO,
							   PVRSRV_HANDLE_ALLOC_FLAG_NONE,
							   psAllocDeviceMemOUT->sClientMemInfo.hKernelMemInfo);
	}
	else
	{
		psAllocDeviceMemOUT->sClientMemInfo.hMappingInfo = 0;
	}
#endif

	if(psAllocDeviceMemIN->ui32Attribs & PVRSRV_MEM_NO_SYNCOBJ)
	{

		OSMemSet(&psAllocDeviceMemOUT->sClientSyncInfo,
				 0,
				 sizeof (PVRSRV_CLIENT_SYNC_INFO));
		psAllocDeviceMemOUT->sClientMemInfo.psClientSyncInfo = IMG_NULL;
	}
	else
	{


#if !defined(PVRSRV_DISABLE_UM_SYNCOBJ_MAPPINGS)
		psAllocDeviceMemOUT->sClientSyncInfo.psSyncData =
			psMemInfo->psKernelSyncInfo->psSyncData;
		psAllocDeviceMemOUT->sClientSyncInfo.sWriteOpsCompleteDevVAddr =
			psMemInfo->psKernelSyncInfo->sWriteOpsCompleteDevVAddr;
		psAllocDeviceMemOUT->sClientSyncInfo.sReadOpsCompleteDevVAddr =
			psMemInfo->psKernelSyncInfo->sReadOpsCompleteDevVAddr;

#if defined (SUPPORT_SID_INTERFACE)
		if (psMemInfo->psKernelSyncInfo->psSyncDataMemInfoKM->sMemBlk.hOSMemHandle != IMG_NULL)
		{
			PVRSRVAllocSubHandleNR(psPerProc->psHandleBase,
									   &psAllocDeviceMemOUT->sClientSyncInfo.hMappingInfo,
									   psMemInfo->psKernelSyncInfo->psSyncDataMemInfoKM->sMemBlk.hOSMemHandle,
									   PVRSRV_HANDLE_TYPE_SYNC_INFO,
									   PVRSRV_HANDLE_ALLOC_FLAG_NONE,
									   psAllocDeviceMemOUT->sClientMemInfo.hKernelMemInfo);
		}
		else
		{
			psAllocDeviceMemOUT->sClientSyncInfo.hMappingInfo = 0;
		}
#else
		psAllocDeviceMemOUT->sClientSyncInfo.hMappingInfo =
			psMemInfo->psKernelSyncInfo->psSyncDataMemInfoKM->sMemBlk.hOSMemHandle;
#endif
#endif

		PVRSRVAllocSubHandleNR(psPerProc->psHandleBase,
							 &psAllocDeviceMemOUT->sClientSyncInfo.hKernelSyncInfo,
							 psMemInfo->psKernelSyncInfo,
							 PVRSRV_HANDLE_TYPE_SYNC_INFO,
							 PVRSRV_HANDLE_ALLOC_FLAG_NONE,
							 psAllocDeviceMemOUT->sClientMemInfo.hKernelMemInfo);

		psAllocDeviceMemOUT->sClientMemInfo.psClientSyncInfo =
			&psAllocDeviceMemOUT->sClientSyncInfo;

	}

	COMMIT_HANDLE_BATCH_OR_ERROR(psAllocDeviceMemOUT->eError, psPerProc)

	return 0;
}

#endif 

static IMG_INT
PVRSRVFreeDeviceMemBW(IMG_UINT32 ui32BridgeID,
					  PVRSRV_BRIDGE_IN_FREEDEVICEMEM *psFreeDeviceMemIN,
					  PVRSRV_BRIDGE_RETURN *psRetOUT,
					  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_HANDLE hDevCookieInt;
	IMG_VOID *pvKernelMemInfo;


	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_FREE_DEVICEMEM);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase, &hDevCookieInt,
						   psFreeDeviceMemIN->hDevCookie,
						   PVRSRV_HANDLE_TYPE_DEV_NODE);

	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
				&pvKernelMemInfo,
#if defined (SUPPORT_SID_INTERFACE)
						   psFreeDeviceMemIN->hKernelMemInfo,
#else
						   psFreeDeviceMemIN->psKernelMemInfo,
#endif
						   PVRSRV_HANDLE_TYPE_MEM_INFO);

	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError = PVRSRVFreeDeviceMemKM(hDevCookieInt, pvKernelMemInfo);

	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PVRSRVReleaseHandle(psPerProc->psHandleBase,
#if defined (SUPPORT_SID_INTERFACE)
							psFreeDeviceMemIN->hKernelMemInfo,
#else
							psFreeDeviceMemIN->psKernelMemInfo,
#endif
							PVRSRV_HANDLE_TYPE_MEM_INFO);

	return 0;
}


static IMG_INT
PVRSRVExportDeviceMemBW(IMG_UINT32 ui32BridgeID,
					  PVRSRV_BRIDGE_IN_EXPORTDEVICEMEM *psExportDeviceMemIN,
					  PVRSRV_BRIDGE_OUT_EXPORTDEVICEMEM *psExportDeviceMemOUT,
					  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_HANDLE hDevCookieInt;
#if defined (SUPPORT_SID_INTERFACE)
	PVRSRV_KERNEL_MEM_INFO *psKernelMemInfo = IMG_NULL;
#else
	PVRSRV_KERNEL_MEM_INFO *psKernelMemInfo;
#endif

	PVR_ASSERT(ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_EXPORT_DEVICEMEM) ||
			ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_EXPORT_DEVICEMEM_2));
	PVR_UNREFERENCED_PARAMETER(ui32BridgeID);


	psExportDeviceMemOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
				&hDevCookieInt,
						   psExportDeviceMemIN->hDevCookie,
						   PVRSRV_HANDLE_TYPE_DEV_NODE);

	if(psExportDeviceMemOUT->eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVExportDeviceMemBW: can't find devcookie"));
		return 0;
	}


	psExportDeviceMemOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   (IMG_PVOID *)&psKernelMemInfo,
#if defined (SUPPORT_SID_INTERFACE)
						   psExportDeviceMemIN->hKernelMemInfo,
#else
						   psExportDeviceMemIN->psKernelMemInfo,
#endif
						   PVRSRV_HANDLE_TYPE_MEM_INFO);

	if(psExportDeviceMemOUT->eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVExportDeviceMemBW: can't find kernel meminfo"));
		return 0;
	}


	psExportDeviceMemOUT->eError =
		PVRSRVFindHandle(KERNEL_HANDLE_BASE,
							 &psExportDeviceMemOUT->hMemInfo,
							 psKernelMemInfo,
							 PVRSRV_HANDLE_TYPE_MEM_INFO);
	if(psExportDeviceMemOUT->eError == PVRSRV_OK)
	{

		PVR_DPF((PVR_DBG_MESSAGE, "PVRSRVExportDeviceMemBW: allocation is already exported"));
		return 0;
	}


	psExportDeviceMemOUT->eError = PVRSRVAllocHandle(KERNEL_HANDLE_BASE,
													&psExportDeviceMemOUT->hMemInfo,
													psKernelMemInfo,
													PVRSRV_HANDLE_TYPE_MEM_INFO,
													PVRSRV_HANDLE_ALLOC_FLAG_NONE);
	if (psExportDeviceMemOUT->eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVExportDeviceMemBW: failed to allocate handle from global handle list"));
		return 0;
	}


	psKernelMemInfo->ui32Flags |= PVRSRV_MEM_EXPORTED;

	return 0;
}


static IMG_INT
PVRSRVMapDeviceMemoryBW(IMG_UINT32 ui32BridgeID,
							 PVRSRV_BRIDGE_IN_MAP_DEV_MEMORY *psMapDevMemIN,
							 PVRSRV_BRIDGE_OUT_MAP_DEV_MEMORY *psMapDevMemOUT,
							 PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_KERNEL_MEM_INFO  *psSrcKernelMemInfo = IMG_NULL;
	PVRSRV_KERNEL_MEM_INFO  *psDstKernelMemInfo = IMG_NULL;
	IMG_HANDLE              hDstDevMemHeap = IMG_NULL;

	PVR_ASSERT(ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_MAP_DEV_MEMORY) ||
			ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_MAP_DEV_MEMORY_2));
	PVR_UNREFERENCED_PARAMETER(ui32BridgeID);

	NEW_HANDLE_BATCH_OR_ERROR(psMapDevMemOUT->eError, psPerProc, 2)


	psMapDevMemOUT->eError = PVRSRVLookupHandle(KERNEL_HANDLE_BASE,
												(IMG_VOID**)&psSrcKernelMemInfo,
												psMapDevMemIN->hKernelMemInfo,
												PVRSRV_HANDLE_TYPE_MEM_INFO);
	if(psMapDevMemOUT->eError != PVRSRV_OK)
	{
		return 0;
	}


	psMapDevMemOUT->eError = PVRSRVLookupHandle(psPerProc->psHandleBase,
												&hDstDevMemHeap,
												psMapDevMemIN->hDstDevMemHeap,
												PVRSRV_HANDLE_TYPE_DEV_MEM_HEAP);
	if(psMapDevMemOUT->eError != PVRSRV_OK)
	{
		return 0;
	}


	if (psSrcKernelMemInfo->sShareMemWorkaround.bInUse)
	{
		PVR_DPF((PVR_DBG_MESSAGE, "using the mem wrap workaround."));









		psMapDevMemOUT->eError = BM_XProcWorkaroundSetShareIndex(psSrcKernelMemInfo->sShareMemWorkaround.ui32ShareIndex);
		if(psMapDevMemOUT->eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "PVRSRVMapDeviceMemoryBW(): failed to recycle shared buffer"));
			return 0;
		}

		psMapDevMemOUT->eError =
			PVRSRVAllocDeviceMemKM(psSrcKernelMemInfo->sShareMemWorkaround.hDevCookieInt,
								   psPerProc,
								   hDstDevMemHeap,
								   psSrcKernelMemInfo->sShareMemWorkaround.ui32OrigReqAttribs | PVRSRV_MEM_NO_SYNCOBJ,
								   psSrcKernelMemInfo->sShareMemWorkaround.ui32OrigReqSize,
								   psSrcKernelMemInfo->sShareMemWorkaround.ui32OrigReqAlignment,
								   &psDstKernelMemInfo,
								   "" );


		BM_XProcWorkaroundUnsetShareIndex(psSrcKernelMemInfo->sShareMemWorkaround.ui32ShareIndex);
		if(psMapDevMemOUT->eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "lakjgfgewjlrgebhe"));
			return 0;
		}

		if(psSrcKernelMemInfo->psKernelSyncInfo)
		{
			psSrcKernelMemInfo->psKernelSyncInfo->ui32RefCount++;
		}

		psDstKernelMemInfo->psKernelSyncInfo = psSrcKernelMemInfo->psKernelSyncInfo;
	}
	else
	{

		psMapDevMemOUT->eError = PVRSRVMapDeviceMemoryKM(psPerProc,
														 psSrcKernelMemInfo,
														 hDstDevMemHeap,
														 &psDstKernelMemInfo);
		if(psMapDevMemOUT->eError != PVRSRV_OK)
		{
			return 0;
		}
	}


	psDstKernelMemInfo->sShareMemWorkaround = psSrcKernelMemInfo->sShareMemWorkaround;

	OSMemSet(&psMapDevMemOUT->sDstClientMemInfo,
			0,
			sizeof(psMapDevMemOUT->sDstClientMemInfo));
	OSMemSet(&psMapDevMemOUT->sDstClientSyncInfo,
			0,
			sizeof(psMapDevMemOUT->sDstClientSyncInfo));

	psMapDevMemOUT->sDstClientMemInfo.pvLinAddrKM =
			psDstKernelMemInfo->pvLinAddrKM;

	psMapDevMemOUT->sDstClientMemInfo.pvLinAddr = 0;
	psMapDevMemOUT->sDstClientMemInfo.sDevVAddr = psDstKernelMemInfo->sDevVAddr;
	psMapDevMemOUT->sDstClientMemInfo.ui32Flags = psDstKernelMemInfo->ui32Flags;
	psMapDevMemOUT->sDstClientMemInfo.uAllocSize = psDstKernelMemInfo->uAllocSize;
#if defined (SUPPORT_SID_INTERFACE)
#else
	psMapDevMemOUT->sDstClientMemInfo.hMappingInfo = psDstKernelMemInfo->sMemBlk.hOSMemHandle;
#endif


	PVRSRVAllocHandleNR(psPerProc->psHandleBase,
					  &psMapDevMemOUT->sDstClientMemInfo.hKernelMemInfo,
					  psDstKernelMemInfo,
					  PVRSRV_HANDLE_TYPE_MEM_INFO,
					  PVRSRV_HANDLE_ALLOC_FLAG_NONE);
	psMapDevMemOUT->sDstClientSyncInfo.hKernelSyncInfo = IMG_NULL;

#if defined (SUPPORT_SID_INTERFACE)

	if (psDstKernelMemInfo->sMemBlk.hOSMemHandle != IMG_NULL)
	{
		PVRSRVAllocSubHandleNR(psPerProc->psHandleBase,
						&psMapDevMemOUT->sDstClientMemInfo.hMappingInfo,
						psDstKernelMemInfo->sMemBlk.hOSMemHandle,
						PVRSRV_HANDLE_TYPE_MEM_INFO,
						PVRSRV_HANDLE_ALLOC_FLAG_NONE,
						psMapDevMemOUT->sDstClientMemInfo.hKernelMemInfo);
	}
	else
	{
		psMapDevMemOUT->sDstClientMemInfo.hMappingInfo = 0;
	}
#endif


	if(psDstKernelMemInfo->psKernelSyncInfo)
	{
#if !defined(PVRSRV_DISABLE_UM_SYNCOBJ_MAPPINGS)
		psMapDevMemOUT->sDstClientSyncInfo.psSyncData =
			psDstKernelMemInfo->psKernelSyncInfo->psSyncData;
		psMapDevMemOUT->sDstClientSyncInfo.sWriteOpsCompleteDevVAddr =
			psDstKernelMemInfo->psKernelSyncInfo->sWriteOpsCompleteDevVAddr;
		psMapDevMemOUT->sDstClientSyncInfo.sReadOpsCompleteDevVAddr =
			psDstKernelMemInfo->psKernelSyncInfo->sReadOpsCompleteDevVAddr;

#if defined (SUPPORT_SID_INTERFACE)

		if (psDstKernelMemInfo->psKernelSyncInfo->psSyncDataMemInfoKM->sMemBlk.hOSMemHandle != IMG_NULL)
		{
			PVRSRVAllocSubHandleNR(psPerProc->psHandleBase,
							&psMapDevMemOUT->sDstClientSyncInfo.hMappingInfo,
							psDstKernelMemInfo->psKernelSyncInfo->psSyncDataMemInfoKM->sMemBlk.hOSMemHandle,
							PVRSRV_HANDLE_TYPE_MEM_INFO,
							PVRSRV_HANDLE_ALLOC_FLAG_NONE,
							psMapDevMemOUT->sDstClientMemInfo.hKernelMemInfo);
		}
		else
		{
			psMapDevMemOUT->sDstClientSyncInfo.hMappingInfo = 0;
		}
#else
		psMapDevMemOUT->sDstClientSyncInfo.hMappingInfo =
			psDstKernelMemInfo->psKernelSyncInfo->psSyncDataMemInfoKM->sMemBlk.hOSMemHandle;
#endif
#endif

		psMapDevMemOUT->sDstClientMemInfo.psClientSyncInfo = &psMapDevMemOUT->sDstClientSyncInfo;

		PVRSRVAllocSubHandleNR(psPerProc->psHandleBase,
					  &psMapDevMemOUT->sDstClientSyncInfo.hKernelSyncInfo,
					  psDstKernelMemInfo->psKernelSyncInfo,
					  PVRSRV_HANDLE_TYPE_SYNC_INFO,
					  PVRSRV_HANDLE_ALLOC_FLAG_MULTI,
					  psMapDevMemOUT->sDstClientMemInfo.hKernelMemInfo);
	}

	COMMIT_HANDLE_BATCH_OR_ERROR(psMapDevMemOUT->eError, psPerProc)

	return 0;
}


static IMG_INT
PVRSRVUnmapDeviceMemoryBW(IMG_UINT32 ui32BridgeID,
							 PVRSRV_BRIDGE_IN_UNMAP_DEV_MEMORY *psUnmapDevMemIN,
							 PVRSRV_BRIDGE_RETURN *psRetOUT,
							 PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_KERNEL_MEM_INFO  *psKernelMemInfo = IMG_NULL;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_UNMAP_DEV_MEMORY);

	psRetOUT->eError = PVRSRVLookupHandle(psPerProc->psHandleBase,
											(IMG_VOID**)&psKernelMemInfo,
#if defined (SUPPORT_SID_INTERFACE)
											psUnmapDevMemIN->hKernelMemInfo,
#else
											psUnmapDevMemIN->psKernelMemInfo,
#endif
											PVRSRV_HANDLE_TYPE_MEM_INFO);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	if (psKernelMemInfo->sShareMemWorkaround.bInUse)
	{
		psRetOUT->eError = PVRSRVFreeDeviceMemKM(psKernelMemInfo->sShareMemWorkaround.hDevCookieInt, psKernelMemInfo);
		if(psRetOUT->eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "PVRSRVUnmapDeviceMemoryBW: internal error, should expect FreeDeviceMem to fail"));
			return 0;
		}
	}
	else
	{
		psRetOUT->eError = PVRSRVUnmapDeviceMemoryKM(psKernelMemInfo);
		if(psRetOUT->eError != PVRSRV_OK)
		{
			return 0;
		}
	}

	psRetOUT->eError = PVRSRVReleaseHandle(psPerProc->psHandleBase,
#if defined (SUPPORT_SID_INTERFACE)
							psUnmapDevMemIN->hKernelMemInfo,
#else
							psUnmapDevMemIN->psKernelMemInfo,
#endif
							PVRSRV_HANDLE_TYPE_MEM_INFO);

	return 0;
}



static IMG_INT
PVRSRVMapDeviceClassMemoryBW(IMG_UINT32 ui32BridgeID,
							 PVRSRV_BRIDGE_IN_MAP_DEVICECLASS_MEMORY *psMapDevClassMemIN,
							 PVRSRV_BRIDGE_OUT_MAP_DEVICECLASS_MEMORY *psMapDevClassMemOUT,
							 PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_KERNEL_MEM_INFO *psMemInfo;
	IMG_HANDLE hOSMapInfo;
	IMG_HANDLE hDeviceClassBufferInt;
	IMG_HANDLE hDevMemContextInt;
	PVRSRV_HANDLE_TYPE eHandleType;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_MAP_DEVICECLASS_MEMORY);

	NEW_HANDLE_BATCH_OR_ERROR(psMapDevClassMemOUT->eError, psPerProc, 2)


	psMapDevClassMemOUT->eError =
		PVRSRVLookupHandleAnyType(psPerProc->psHandleBase,
								  &hDeviceClassBufferInt,
								  &eHandleType,
								  psMapDevClassMemIN->hDeviceClassBuffer);

	if(psMapDevClassMemOUT->eError != PVRSRV_OK)
	{
		return 0;
	}


	psMapDevClassMemOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
				   &hDevMemContextInt,
				   psMapDevClassMemIN->hDevMemContext,
				   PVRSRV_HANDLE_TYPE_DEV_MEM_CONTEXT);

	if(psMapDevClassMemOUT->eError != PVRSRV_OK)
	{
		return 0;
	}


	switch(eHandleType)
	{
#if defined(PVR_SECURE_HANDLES) || defined (SUPPORT_SID_INTERFACE)
		case PVRSRV_HANDLE_TYPE_DISP_BUFFER:
		case PVRSRV_HANDLE_TYPE_BUF_BUFFER:
#else
		case PVRSRV_HANDLE_TYPE_NONE:
#endif
			break;
		default:
			psMapDevClassMemOUT->eError = PVRSRV_ERROR_INVALID_HANDLE_TYPE;
			return 0;
	}

	psMapDevClassMemOUT->eError =
		PVRSRVMapDeviceClassMemoryKM(psPerProc,
									 hDevMemContextInt,
									 hDeviceClassBufferInt,
									 &psMemInfo,
									 &hOSMapInfo);
	if(psMapDevClassMemOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	OSMemSet(&psMapDevClassMemOUT->sClientMemInfo,
			0,
			sizeof(psMapDevClassMemOUT->sClientMemInfo));
	OSMemSet(&psMapDevClassMemOUT->sClientSyncInfo,
			0,
			sizeof(psMapDevClassMemOUT->sClientSyncInfo));

	psMapDevClassMemOUT->sClientMemInfo.pvLinAddrKM =
			psMemInfo->pvLinAddrKM;

	psMapDevClassMemOUT->sClientMemInfo.pvLinAddr = 0;
	psMapDevClassMemOUT->sClientMemInfo.sDevVAddr = psMemInfo->sDevVAddr;
	psMapDevClassMemOUT->sClientMemInfo.ui32Flags = psMemInfo->ui32Flags;
	psMapDevClassMemOUT->sClientMemInfo.uAllocSize = psMemInfo->uAllocSize;
#if defined (SUPPORT_SID_INTERFACE)
	if (psMemInfo->sMemBlk.hOSMemHandle != 0)
	{
		PVRSRVAllocSubHandleNR(psPerProc->psHandleBase,
					&psMapDevClassMemOUT->sClientMemInfo.hMappingInfo,
					psMemInfo->sMemBlk.hOSMemHandle,
					PVRSRV_HANDLE_TYPE_MEM_INFO,
					PVRSRV_HANDLE_ALLOC_FLAG_NONE,
					psMapDevClassMemIN->hDeviceClassBuffer);
	}
	else
	{
		psMapDevClassMemOUT->sClientMemInfo.hMappingInfo = 0;
	}
#else
	psMapDevClassMemOUT->sClientMemInfo.hMappingInfo = psMemInfo->sMemBlk.hOSMemHandle;
#endif

	PVRSRVAllocSubHandleNR(psPerProc->psHandleBase,
					  &psMapDevClassMemOUT->sClientMemInfo.hKernelMemInfo,
					  psMemInfo,
					  PVRSRV_HANDLE_TYPE_MEM_INFO,
					  PVRSRV_HANDLE_ALLOC_FLAG_NONE,
					  psMapDevClassMemIN->hDeviceClassBuffer);

	psMapDevClassMemOUT->sClientSyncInfo.hKernelSyncInfo = IMG_NULL;


	if(psMemInfo->psKernelSyncInfo)
	{
#if !defined(PVRSRV_DISABLE_UM_SYNCOBJ_MAPPINGS)
		psMapDevClassMemOUT->sClientSyncInfo.psSyncData =
			psMemInfo->psKernelSyncInfo->psSyncData;
		psMapDevClassMemOUT->sClientSyncInfo.sWriteOpsCompleteDevVAddr =
			psMemInfo->psKernelSyncInfo->sWriteOpsCompleteDevVAddr;
		psMapDevClassMemOUT->sClientSyncInfo.sReadOpsCompleteDevVAddr =
			psMemInfo->psKernelSyncInfo->sReadOpsCompleteDevVAddr;

#if defined (SUPPORT_SID_INTERFACE)
		if (psMemInfo->psKernelSyncInfo->psSyncDataMemInfoKM->sMemBlk.hOSMemHandle != 0)
		{
			PVRSRVAllocSubHandleNR(psPerProc->psHandleBase,
								&psMapDevClassMemOUT->sClientSyncInfo.hMappingInfo,
								psMemInfo->psKernelSyncInfo->psSyncDataMemInfoKM->sMemBlk.hOSMemHandle,
								PVRSRV_HANDLE_TYPE_SYNC_INFO,
								PVRSRV_HANDLE_ALLOC_FLAG_MULTI,
								psMapDevClassMemOUT->sClientMemInfo.hKernelMemInfo);
		}
		else
		{
			psMapDevClassMemOUT->sClientSyncInfo.hMappingInfo = 0;
		}
#else
		psMapDevClassMemOUT->sClientSyncInfo.hMappingInfo =
			psMemInfo->psKernelSyncInfo->psSyncDataMemInfoKM->sMemBlk.hOSMemHandle;
#endif
#endif

		psMapDevClassMemOUT->sClientMemInfo.psClientSyncInfo = &psMapDevClassMemOUT->sClientSyncInfo;

		PVRSRVAllocSubHandleNR(psPerProc->psHandleBase,
						  &psMapDevClassMemOUT->sClientSyncInfo.hKernelSyncInfo,
						  psMemInfo->psKernelSyncInfo,
						  PVRSRV_HANDLE_TYPE_SYNC_INFO,
						  PVRSRV_HANDLE_ALLOC_FLAG_MULTI,
						  psMapDevClassMemOUT->sClientMemInfo.hKernelMemInfo);
	}

	COMMIT_HANDLE_BATCH_OR_ERROR(psMapDevClassMemOUT->eError, psPerProc)

	return 0;
}

static IMG_INT
PVRSRVUnmapDeviceClassMemoryBW(IMG_UINT32 ui32BridgeID,
							   PVRSRV_BRIDGE_IN_UNMAP_DEVICECLASS_MEMORY *psUnmapDevClassMemIN,
							   PVRSRV_BRIDGE_RETURN *psRetOUT,
							   PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_VOID *pvKernelMemInfo;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_UNMAP_DEVICECLASS_MEMORY);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase, &pvKernelMemInfo,
#if defined (SUPPORT_SID_INTERFACE)
						   psUnmapDevClassMemIN->hKernelMemInfo,
#else
						   psUnmapDevClassMemIN->psKernelMemInfo,
#endif
						   PVRSRV_HANDLE_TYPE_MEM_INFO);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError = PVRSRVUnmapDeviceClassMemoryKM(pvKernelMemInfo);

	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PVRSRVReleaseHandle(psPerProc->psHandleBase,
#if defined (SUPPORT_SID_INTERFACE)
							psUnmapDevClassMemIN->hKernelMemInfo,
#else
							psUnmapDevClassMemIN->psKernelMemInfo,
#endif
							PVRSRV_HANDLE_TYPE_MEM_INFO);

	return 0;
}


#if defined(OS_PVRSRV_WRAP_EXT_MEM_BW)
IMG_INT
PVRSRVWrapExtMemoryBW(IMG_UINT32 ui32BridgeID,
					  PVRSRV_BRIDGE_IN_WRAP_EXT_MEMORY *psWrapExtMemIN,
					  PVRSRV_BRIDGE_OUT_WRAP_EXT_MEMORY *psWrapExtMemOUT,
					  PVRSRV_PER_PROCESS_DATA *psPerProc);
#else 
static IMG_INT
PVRSRVWrapExtMemoryBW(IMG_UINT32 ui32BridgeID,
					  PVRSRV_BRIDGE_IN_WRAP_EXT_MEMORY *psWrapExtMemIN,
					  PVRSRV_BRIDGE_OUT_WRAP_EXT_MEMORY *psWrapExtMemOUT,
					  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_HANDLE hDevCookieInt;
	IMG_HANDLE hDevMemContextInt;
	PVRSRV_KERNEL_MEM_INFO *psMemInfo;
	IMG_SYS_PHYADDR *psSysPAddr = IMG_NULL;
	IMG_UINT32 ui32PageTableSize = 0;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_WRAP_EXT_MEMORY);

	NEW_HANDLE_BATCH_OR_ERROR(psWrapExtMemOUT->eError, psPerProc, 2)


	psWrapExtMemOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase, &hDevCookieInt,
						   psWrapExtMemIN->hDevCookie,
						   PVRSRV_HANDLE_TYPE_DEV_NODE);
	if(psWrapExtMemOUT->eError != PVRSRV_OK)
	{
		return 0;
	}


	psWrapExtMemOUT->eError =
	PVRSRVLookupHandle(psPerProc->psHandleBase, &hDevMemContextInt,
				   psWrapExtMemIN->hDevMemContext,
				   PVRSRV_HANDLE_TYPE_DEV_MEM_CONTEXT);

	if(psWrapExtMemOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	if(psWrapExtMemIN->ui32NumPageTableEntries)
	{
		ui32PageTableSize = psWrapExtMemIN->ui32NumPageTableEntries
						* sizeof(IMG_SYS_PHYADDR);

		ASSIGN_AND_EXIT_ON_ERROR(psWrapExtMemOUT->eError,
				OSAllocMem(PVRSRV_OS_PAGEABLE_HEAP,
				  ui32PageTableSize,
				  (IMG_VOID **)&psSysPAddr, 0,
				  "Page Table"));

		if(CopyFromUserWrapper(psPerProc,
							   ui32BridgeID,
							   psSysPAddr,
							   psWrapExtMemIN->psSysPAddr,
							   ui32PageTableSize) != PVRSRV_OK)
		{
			OSFreeMem(PVRSRV_OS_PAGEABLE_HEAP,  ui32PageTableSize, (IMG_VOID *)psSysPAddr, 0);

			return -EFAULT;
		}
	}

	psWrapExtMemOUT->eError =
		PVRSRVWrapExtMemoryKM(hDevCookieInt,
							  psPerProc,
							  hDevMemContextInt,
							  psWrapExtMemIN->ui32ByteSize,
							  psWrapExtMemIN->ui32PageOffset,
							  psWrapExtMemIN->bPhysContig,
							  psSysPAddr,
							  psWrapExtMemIN->pvLinAddr,
							  psWrapExtMemIN->ui32Flags,
							  &psMemInfo);

	if(psWrapExtMemIN->ui32NumPageTableEntries)
	{
		OSFreeMem(PVRSRV_OS_PAGEABLE_HEAP,
			  ui32PageTableSize,
			  (IMG_VOID *)psSysPAddr, 0);

	}

	if(psWrapExtMemOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psWrapExtMemOUT->sClientMemInfo.pvLinAddrKM =
			psMemInfo->pvLinAddrKM;


	psWrapExtMemOUT->sClientMemInfo.pvLinAddr = 0;
	psWrapExtMemOUT->sClientMemInfo.sDevVAddr = psMemInfo->sDevVAddr;
	psWrapExtMemOUT->sClientMemInfo.ui32Flags = psMemInfo->ui32Flags;
	psWrapExtMemOUT->sClientMemInfo.uAllocSize = psMemInfo->uAllocSize;
#if defined (SUPPORT_SID_INTERFACE)
#else
	psWrapExtMemOUT->sClientMemInfo.hMappingInfo = psMemInfo->sMemBlk.hOSMemHandle;
#endif

	PVRSRVAllocHandleNR(psPerProc->psHandleBase,
					  &psWrapExtMemOUT->sClientMemInfo.hKernelMemInfo,
					  psMemInfo,
					  PVRSRV_HANDLE_TYPE_MEM_INFO,
					  PVRSRV_HANDLE_ALLOC_FLAG_NONE);

#if defined (SUPPORT_SID_INTERFACE)

	if (psMemInfo->sMemBlk.hOSMemHandle != IMG_NULL)
	{
		PVRSRVAllocSubHandleNR(psPerProc->psHandleBase,
						&psWrapExtMemOUT->sClientMemInfo.hMappingInfo,
						psMemInfo->sMemBlk.hOSMemHandle,
						PVRSRV_HANDLE_TYPE_MEM_INFO,
						PVRSRV_HANDLE_ALLOC_FLAG_NONE,
						psWrapExtMemOUT->sClientMemInfo.hKernelMemInfo);
	}
	else
	{
		psWrapExtMemOUT->sClientMemInfo.hMappingInfo = 0;
	}
#endif


#if !defined(PVRSRV_DISABLE_UM_SYNCOBJ_MAPPINGS)
	psWrapExtMemOUT->sClientSyncInfo.psSyncData =
		psMemInfo->psKernelSyncInfo->psSyncData;
	psWrapExtMemOUT->sClientSyncInfo.sWriteOpsCompleteDevVAddr =
		psMemInfo->psKernelSyncInfo->sWriteOpsCompleteDevVAddr;
	psWrapExtMemOUT->sClientSyncInfo.sReadOpsCompleteDevVAddr =
		psMemInfo->psKernelSyncInfo->sReadOpsCompleteDevVAddr;

#if defined (SUPPORT_SID_INTERFACE)

	if (psMemInfo->psKernelSyncInfo->psSyncDataMemInfoKM->sMemBlk.hOSMemHandle != IMG_NULL)
	{
		PVRSRVAllocSubHandleNR(psPerProc->psHandleBase,
						&psWrapExtMemOUT->sClientSyncInfo.hMappingInfo,
						psMemInfo->psKernelSyncInfo->psSyncDataMemInfoKM->sMemBlk.hOSMemHandle,
						PVRSRV_HANDLE_TYPE_MEM_INFO,
						PVRSRV_HANDLE_ALLOC_FLAG_NONE,
						psWrapExtMemOUT->sClientMemInfo.hKernelMemInfo);
	}
	else
	{
		psWrapExtMemOUT->sClientSyncInfo.hMappingInfo = 0;
	}
#else
	psWrapExtMemOUT->sClientSyncInfo.hMappingInfo =
		psMemInfo->psKernelSyncInfo->psSyncDataMemInfoKM->sMemBlk.hOSMemHandle;
#endif
#endif

	psWrapExtMemOUT->sClientMemInfo.psClientSyncInfo = &psWrapExtMemOUT->sClientSyncInfo;

	PVRSRVAllocSubHandleNR(psPerProc->psHandleBase,
					  &psWrapExtMemOUT->sClientSyncInfo.hKernelSyncInfo,
					  (IMG_HANDLE)psMemInfo->psKernelSyncInfo,
					  PVRSRV_HANDLE_TYPE_SYNC_INFO,
					  PVRSRV_HANDLE_ALLOC_FLAG_NONE,
					  psWrapExtMemOUT->sClientMemInfo.hKernelMemInfo);

	COMMIT_HANDLE_BATCH_OR_ERROR(psWrapExtMemOUT->eError, psPerProc)

	return 0;
}
#endif 

static IMG_INT
PVRSRVUnwrapExtMemoryBW(IMG_UINT32 ui32BridgeID,
						PVRSRV_BRIDGE_IN_UNWRAP_EXT_MEMORY *psUnwrapExtMemIN,
						PVRSRV_BRIDGE_RETURN *psRetOUT,
						PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_VOID *pvMemInfo;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_UNWRAP_EXT_MEMORY);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvMemInfo,
						   psUnwrapExtMemIN->hKernelMemInfo,
						   PVRSRV_HANDLE_TYPE_MEM_INFO);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PVRSRVUnwrapExtMemoryKM((PVRSRV_KERNEL_MEM_INFO *)pvMemInfo);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PVRSRVReleaseHandle(psPerProc->psHandleBase,
						   psUnwrapExtMemIN->hKernelMemInfo,
						   PVRSRV_HANDLE_TYPE_MEM_INFO);

	return 0;
}

static IMG_INT
PVRSRVGetFreeDeviceMemBW(IMG_UINT32 ui32BridgeID,
						 PVRSRV_BRIDGE_IN_GETFREEDEVICEMEM *psGetFreeDeviceMemIN,
						 PVRSRV_BRIDGE_OUT_GETFREEDEVICEMEM *psGetFreeDeviceMemOUT,
						 PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_GETFREE_DEVICEMEM);

	PVR_UNREFERENCED_PARAMETER(psPerProc);

	psGetFreeDeviceMemOUT->eError =
		PVRSRVGetFreeDeviceMemKM(psGetFreeDeviceMemIN->ui32Flags,
								 &psGetFreeDeviceMemOUT->ui32Total,
								 &psGetFreeDeviceMemOUT->ui32Free,
								 &psGetFreeDeviceMemOUT->ui32LargestBlock);

	return 0;
}

static IMG_INT
PVRMMapOSMemHandleToMMapDataBW(IMG_UINT32 ui32BridgeID,
								  PVRSRV_BRIDGE_IN_MHANDLE_TO_MMAP_DATA *psMMapDataIN,
								  PVRSRV_BRIDGE_OUT_MHANDLE_TO_MMAP_DATA *psMMapDataOUT,
								  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_MHANDLE_TO_MMAP_DATA);

#if defined (__linux__) || defined(__QNXNTO__)
	psMMapDataOUT->eError =
		PVRMMapOSMemHandleToMMapData(psPerProc,
										psMMapDataIN->hMHandle,
										&psMMapDataOUT->ui32MMapOffset,
										&psMMapDataOUT->ui32ByteOffset,
										&psMMapDataOUT->ui32RealByteSize,
										&psMMapDataOUT->ui32UserVAddr);
#else
	PVR_UNREFERENCED_PARAMETER(psPerProc);
	PVR_UNREFERENCED_PARAMETER(psMMapDataIN);

	psMMapDataOUT->eError = PVRSRV_ERROR_NOT_SUPPORTED;
#endif
	return 0;
}


static IMG_INT
PVRMMapReleaseMMapDataBW(IMG_UINT32 ui32BridgeID,
								  PVRSRV_BRIDGE_IN_RELEASE_MMAP_DATA *psMMapDataIN,
								  PVRSRV_BRIDGE_OUT_RELEASE_MMAP_DATA *psMMapDataOUT,
								  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_RELEASE_MMAP_DATA);

#if defined (__linux__) || defined(__QNXNTO__)
	psMMapDataOUT->eError =
		PVRMMapReleaseMMapData(psPerProc,
										psMMapDataIN->hMHandle,
										&psMMapDataOUT->bMUnmap,
										&psMMapDataOUT->ui32RealByteSize,
										&psMMapDataOUT->ui32UserVAddr);
#else

	PVR_UNREFERENCED_PARAMETER(psPerProc);
	PVR_UNREFERENCED_PARAMETER(psMMapDataIN);

	psMMapDataOUT->eError = PVRSRV_ERROR_NOT_SUPPORTED;
#endif
	return 0;
}


#if defined (SUPPORT_SID_INTERFACE)
static IMG_INT
PVRSRVChangeDeviceMemoryAttributesBW(IMG_UINT32 ui32BridgeID,
                                     PVRSRV_BRIDGE_IN_CHG_DEV_MEM_ATTRIBS *psChgMemAttribIN,
                                     PVRSRV_BRIDGE_RETURN *psRetOUT,
                                     PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_HANDLE hKernelMemInfo;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_CHG_DEV_MEM_ATTRIBS);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
							&hKernelMemInfo,
							psChgMemAttribIN->hKernelMemInfo,
							PVRSRV_HANDLE_TYPE_MEM_INFO);

	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PVRSRVChangeDeviceMemoryAttributesKM(hKernelMemInfo, psChgMemAttribIN->ui32Attribs);

	return 0;
}
#else
static IMG_INT
PVRSRVChangeDeviceMemoryAttributesBW(IMG_UINT32 ui32BridgeID,
                                     PVRSRV_BRIDGE_IN_CHG_DEV_MEM_ATTRIBS *psChgMemAttribIN,
                                     PVRSRV_BRIDGE_RETURN *psRetOUT,
                                     PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVR_UNREFERENCED_PARAMETER(ui32BridgeID);
	PVR_UNREFERENCED_PARAMETER(psChgMemAttribIN);
	PVR_UNREFERENCED_PARAMETER(psRetOUT);
	PVR_UNREFERENCED_PARAMETER(psPerProc);

	return 0;
}
#endif

#ifdef PDUMP
static IMG_INT
PDumpIsCaptureFrameBW(IMG_UINT32 ui32BridgeID,
					  IMG_VOID *psBridgeIn,
					  PVRSRV_BRIDGE_OUT_PDUMP_ISCAPTURING *psPDumpIsCapturingOUT,
					  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_PDUMP_ISCAPTURING);
	PVR_UNREFERENCED_PARAMETER(psBridgeIn);
	PVR_UNREFERENCED_PARAMETER(psPerProc);

	psPDumpIsCapturingOUT->bIsCapturing = PDumpIsCaptureFrameKM();
	psPDumpIsCapturingOUT->eError = PVRSRV_OK;

	return 0;
}

static IMG_INT
PDumpCommentBW(IMG_UINT32 ui32BridgeID,
			   PVRSRV_BRIDGE_IN_PDUMP_COMMENT *psPDumpCommentIN,
			   PVRSRV_BRIDGE_RETURN *psRetOUT,
			   PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_PDUMP_COMMENT);
	PVR_UNREFERENCED_PARAMETER(psPerProc);

	psRetOUT->eError = PDumpCommentKM(&psPDumpCommentIN->szComment[0],
									  psPDumpCommentIN->ui32Flags);
	return 0;
}

static IMG_INT
PDumpSetFrameBW(IMG_UINT32 ui32BridgeID,
				PVRSRV_BRIDGE_IN_PDUMP_SETFRAME *psPDumpSetFrameIN,
				PVRSRV_BRIDGE_RETURN *psRetOUT,
				PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_PDUMP_SETFRAME);
	PVR_UNREFERENCED_PARAMETER(psPerProc);

	psRetOUT->eError = PDumpSetFrameKM(psPDumpSetFrameIN->ui32Frame);

	return 0;
}

static IMG_INT
PDumpRegWithFlagsBW(IMG_UINT32 ui32BridgeID,
					PVRSRV_BRIDGE_IN_PDUMP_DUMPREG *psPDumpRegDumpIN,
					PVRSRV_BRIDGE_RETURN *psRetOUT,
					PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_DEVICE_NODE *psDeviceNode;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_PDUMP_REG);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   (IMG_VOID **)&psDeviceNode,
						   psPDumpRegDumpIN->hDevCookie,
						   PVRSRV_HANDLE_TYPE_DEV_NODE);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError = PDumpRegWithFlagsKM (psPDumpRegDumpIN->szRegRegion,
											psPDumpRegDumpIN->sHWReg.ui32RegAddr,
											psPDumpRegDumpIN->sHWReg.ui32RegVal,
											psPDumpRegDumpIN->ui32Flags);

	return 0;
}

static IMG_INT
PDumpRegPolBW(IMG_UINT32 ui32BridgeID,
			  PVRSRV_BRIDGE_IN_PDUMP_REGPOL *psPDumpRegPolIN,
			  PVRSRV_BRIDGE_RETURN *psRetOUT,
			  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_DEVICE_NODE *psDeviceNode;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_PDUMP_REGPOL);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   (IMG_VOID **)&psDeviceNode,
						   psPDumpRegPolIN->hDevCookie,
						   PVRSRV_HANDLE_TYPE_DEV_NODE);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}


	psRetOUT->eError =
		PDumpRegPolWithFlagsKM(psPDumpRegPolIN->szRegRegion,
							   psPDumpRegPolIN->sHWReg.ui32RegAddr,
							   psPDumpRegPolIN->sHWReg.ui32RegVal,
							   psPDumpRegPolIN->ui32Mask,
							   psPDumpRegPolIN->ui32Flags,
							   PDUMP_POLL_OPERATOR_EQUAL);

	return 0;
}

static IMG_INT
PDumpMemPolBW(IMG_UINT32 ui32BridgeID,
			  PVRSRV_BRIDGE_IN_PDUMP_MEMPOL *psPDumpMemPolIN,
			  PVRSRV_BRIDGE_RETURN *psRetOUT,
			  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_VOID *pvMemInfo;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_PDUMP_MEMPOL);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvMemInfo,
#if defined (SUPPORT_SID_INTERFACE)
                           psPDumpMemPolIN->hKernelMemInfo,
#else
						   psPDumpMemPolIN->psKernelMemInfo,
#endif
						   PVRSRV_HANDLE_TYPE_MEM_INFO);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PDumpMemPolKM(((PVRSRV_KERNEL_MEM_INFO *)pvMemInfo),
					  psPDumpMemPolIN->ui32Offset,
					  psPDumpMemPolIN->ui32Value,
					  psPDumpMemPolIN->ui32Mask,
					  psPDumpMemPolIN->eOperator,
					  psPDumpMemPolIN->ui32Flags,
					  MAKEUNIQUETAG(pvMemInfo));

	return 0;
}

static IMG_INT
PDumpMemBW(IMG_UINT32 ui32BridgeID,
		   PVRSRV_BRIDGE_IN_PDUMP_DUMPMEM *psPDumpMemDumpIN,
		   PVRSRV_BRIDGE_RETURN *psRetOUT,
		   PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_VOID *pvMemInfo;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_PDUMP_DUMPMEM);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvMemInfo,
#if defined (SUPPORT_SID_INTERFACE)
						   psPDumpMemDumpIN->hKernelMemInfo,
#else
						   psPDumpMemDumpIN->psKernelMemInfo,
#endif
						   PVRSRV_HANDLE_TYPE_MEM_INFO);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PDumpMemUM(psPerProc,
				   psPDumpMemDumpIN->pvAltLinAddr,
				   psPDumpMemDumpIN->pvLinAddr,
				   pvMemInfo,
				   psPDumpMemDumpIN->ui32Offset,
				   psPDumpMemDumpIN->ui32Bytes,
				   psPDumpMemDumpIN->ui32Flags,
				   MAKEUNIQUETAG(pvMemInfo));

	return 0;
}

static IMG_INT
PDumpBitmapBW(IMG_UINT32 ui32BridgeID,
			  PVRSRV_BRIDGE_IN_PDUMP_BITMAP *psPDumpBitmapIN,
			  PVRSRV_BRIDGE_RETURN *psRetOUT,
			  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_DEVICE_NODE *psDeviceNode;
	IMG_HANDLE hDevMemContextInt;

	PVR_UNREFERENCED_PARAMETER(ui32BridgeID);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase, (IMG_VOID **)&psDeviceNode,
						   psPDumpBitmapIN->hDevCookie,
						   PVRSRV_HANDLE_TYPE_DEV_NODE);

	psRetOUT->eError =
		PVRSRVLookupHandle(	psPerProc->psHandleBase,
							&hDevMemContextInt,
							psPDumpBitmapIN->hDevMemContext,
							PVRSRV_HANDLE_TYPE_DEV_MEM_CONTEXT);

	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PDumpBitmapKM(psDeviceNode,
					  &psPDumpBitmapIN->szFileName[0],
					  psPDumpBitmapIN->ui32FileOffset,
					  psPDumpBitmapIN->ui32Width,
					  psPDumpBitmapIN->ui32Height,
					  psPDumpBitmapIN->ui32StrideInBytes,
					  psPDumpBitmapIN->sDevBaseAddr,
					  hDevMemContextInt,
					  psPDumpBitmapIN->ui32Size,
					  psPDumpBitmapIN->ePixelFormat,
					  psPDumpBitmapIN->eMemFormat,
					  psPDumpBitmapIN->ui32Flags);

	return 0;
}

static IMG_INT
PDumpReadRegBW(IMG_UINT32 ui32BridgeID,
			   PVRSRV_BRIDGE_IN_PDUMP_READREG *psPDumpReadRegIN,
			   PVRSRV_BRIDGE_RETURN *psRetOUT,
			   PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_DEVICE_NODE *psDeviceNode;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_PDUMP_DUMPREADREG);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase, (IMG_VOID **)&psDeviceNode,
						   psPDumpReadRegIN->hDevCookie,
						   PVRSRV_HANDLE_TYPE_DEV_NODE);

	psRetOUT->eError =
		PDumpReadRegKM(&psPDumpReadRegIN->szRegRegion[0],
					   &psPDumpReadRegIN->szFileName[0],
					   psPDumpReadRegIN->ui32FileOffset,
					   psPDumpReadRegIN->ui32Address,
					   psPDumpReadRegIN->ui32Size,
					   psPDumpReadRegIN->ui32Flags);

	return 0;
}

static IMG_INT
PDumpMemPagesBW(IMG_UINT32 ui32BridgeID,
				  PVRSRV_BRIDGE_IN_PDUMP_MEMPAGES *psPDumpMemPagesIN,
				  PVRSRV_BRIDGE_RETURN *psRetOUT,
				  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_DEVICE_NODE *psDeviceNode;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_PDUMP_MEMPAGES);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   (IMG_VOID **)&psDeviceNode,
						   psPDumpMemPagesIN->hDevCookie,
						   PVRSRV_HANDLE_TYPE_DEV_NODE);

	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}


	return 0;
}

static IMG_INT
PDumpDriverInfoBW(IMG_UINT32 ui32BridgeID,
				  PVRSRV_BRIDGE_IN_PDUMP_DRIVERINFO *psPDumpDriverInfoIN,
				  PVRSRV_BRIDGE_RETURN *psRetOUT,
				  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_UINT32 ui32PDumpFlags;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_PDUMP_DRIVERINFO);
	PVR_UNREFERENCED_PARAMETER(psPerProc);

	ui32PDumpFlags = 0;
	if(psPDumpDriverInfoIN->bContinuous)
	{
		ui32PDumpFlags |= PDUMP_FLAGS_CONTINUOUS;
	}
	psRetOUT->eError =
		PDumpDriverInfoKM(&psPDumpDriverInfoIN->szString[0],
						  ui32PDumpFlags);

	return 0;
}

static IMG_INT
PDumpSyncDumpBW(IMG_UINT32 ui32BridgeID,
				PVRSRV_BRIDGE_IN_PDUMP_DUMPSYNC *psPDumpSyncDumpIN,
				PVRSRV_BRIDGE_RETURN *psRetOUT,
				PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_UINT32 ui32Bytes = psPDumpSyncDumpIN->ui32Bytes;
	IMG_VOID *pvSyncInfo;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_PDUMP_DUMPSYNC);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase, &pvSyncInfo,
#if defined (SUPPORT_SID_INTERFACE)
                           psPDumpSyncDumpIN->hKernelSyncInfo,
#else
						   psPDumpSyncDumpIN->psKernelSyncInfo,
#endif
						   PVRSRV_HANDLE_TYPE_SYNC_INFO);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PDumpMemUM(psPerProc,
				   psPDumpSyncDumpIN->pvAltLinAddr,
				   IMG_NULL,
				   ((PVRSRV_KERNEL_SYNC_INFO *)pvSyncInfo)->psSyncDataMemInfoKM,
				   psPDumpSyncDumpIN->ui32Offset,
				   ui32Bytes,
				   0,
				   MAKEUNIQUETAG(((PVRSRV_KERNEL_SYNC_INFO *)pvSyncInfo)->psSyncDataMemInfoKM));

	return 0;
}

static IMG_INT
PDumpSyncPolBW(IMG_UINT32 ui32BridgeID,
			   PVRSRV_BRIDGE_IN_PDUMP_SYNCPOL *psPDumpSyncPolIN,
			   PVRSRV_BRIDGE_RETURN *psRetOUT,
			   PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_UINT32 ui32Offset;
	IMG_VOID *pvSyncInfo;
	IMG_UINT32 ui32Value;
	IMG_UINT32 ui32Mask;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_PDUMP_SYNCPOL);

	psRetOUT->eError =
        PVRSRVLookupHandle(psPerProc->psHandleBase,
                           &pvSyncInfo,
#if defined (SUPPORT_SID_INTERFACE)
						   psPDumpSyncPolIN->hKernelSyncInfo,
#else
						   psPDumpSyncPolIN->psKernelSyncInfo,
#endif
						   PVRSRV_HANDLE_TYPE_SYNC_INFO);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	if(psPDumpSyncPolIN->bIsRead)
	{
		ui32Offset = offsetof(PVRSRV_SYNC_DATA, ui32ReadOpsComplete);
	}
	else
	{
		ui32Offset = offsetof(PVRSRV_SYNC_DATA, ui32WriteOpsComplete);
	}

	
	if (psPDumpSyncPolIN->bUseLastOpDumpVal)
	{
		if(psPDumpSyncPolIN->bIsRead)
		{
			ui32Value = ((PVRSRV_KERNEL_SYNC_INFO *)pvSyncInfo)->psSyncData->ui32LastReadOpDumpVal;
		}
		else
		{
			ui32Value = ((PVRSRV_KERNEL_SYNC_INFO *)pvSyncInfo)->psSyncData->ui32LastOpDumpVal;
		}
		ui32Mask = 0xffffffff;
	}
	else
	{
		ui32Value = psPDumpSyncPolIN->ui32Value;
		ui32Mask =  psPDumpSyncPolIN->ui32Mask;
	}

	psRetOUT->eError =
		PDumpMemPolKM(((PVRSRV_KERNEL_SYNC_INFO *)pvSyncInfo)->psSyncDataMemInfoKM,
					  ui32Offset,
					  ui32Value,
					  ui32Mask,
					  PDUMP_POLL_OPERATOR_EQUAL,
					  0,
					  MAKEUNIQUETAG(((PVRSRV_KERNEL_SYNC_INFO *)pvSyncInfo)->psSyncDataMemInfoKM));

	return 0;
}


static IMG_INT
PDumpCycleCountRegReadBW(IMG_UINT32 ui32BridgeID,
						 PVRSRV_BRIDGE_IN_PDUMP_CYCLE_COUNT_REG_READ *psPDumpCycleCountRegReadIN,
						 PVRSRV_BRIDGE_RETURN *psRetOUT,
						 PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_DEVICE_NODE *psDeviceNode;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_PDUMP_CYCLE_COUNT_REG_READ);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   (IMG_VOID **)&psDeviceNode,
						   psPDumpCycleCountRegReadIN->hDevCookie,
						   PVRSRV_HANDLE_TYPE_DEV_NODE);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	PDumpCycleCountRegRead(&psDeviceNode->sDevId,
						   psPDumpCycleCountRegReadIN->ui32RegOffset,
						   psPDumpCycleCountRegReadIN->bLastFrame);

	psRetOUT->eError = PVRSRV_OK;

	return 0;
}

static IMG_INT
PDumpPDDevPAddrBW(IMG_UINT32 ui32BridgeID,
				  PVRSRV_BRIDGE_IN_PDUMP_DUMPPDDEVPADDR *psPDumpPDDevPAddrIN,
				  PVRSRV_BRIDGE_RETURN *psRetOUT,
				  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_VOID *pvMemInfo;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_PDUMP_DUMPPDDEVPADDR);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase, &pvMemInfo,
						   psPDumpPDDevPAddrIN->hKernelMemInfo,
						   PVRSRV_HANDLE_TYPE_MEM_INFO);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PDumpPDDevPAddrKM((PVRSRV_KERNEL_MEM_INFO *)pvMemInfo,
						  psPDumpPDDevPAddrIN->ui32Offset,
						  psPDumpPDDevPAddrIN->sPDDevPAddr,
						  MAKEUNIQUETAG(pvMemInfo),
						  PDUMP_PD_UNIQUETAG);
	return 0;
}

static IMG_INT
PDumpStartInitPhaseBW(IMG_UINT32 ui32BridgeID,
					  IMG_VOID *psBridgeIn,
					  PVRSRV_BRIDGE_RETURN *psRetOUT,
					  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_PDUMP_STARTINITPHASE);
	PVR_UNREFERENCED_PARAMETER(psBridgeIn);
	PVR_UNREFERENCED_PARAMETER(psPerProc);

	psRetOUT->eError = PDumpStartInitPhaseKM();

	return 0;
}

static IMG_INT
PDumpStopInitPhaseBW(IMG_UINT32 ui32BridgeID,
					  IMG_VOID *psBridgeIn,
					  PVRSRV_BRIDGE_RETURN *psRetOUT,
					  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_PDUMP_STOPINITPHASE);
	PVR_UNREFERENCED_PARAMETER(psBridgeIn);
	PVR_UNREFERENCED_PARAMETER(psPerProc);

	psRetOUT->eError = PDumpStopInitPhaseKM();

	return 0;
}

#endif 


static IMG_INT
PVRSRVGetMiscInfoBW(IMG_UINT32 ui32BridgeID,
					PVRSRV_BRIDGE_IN_GET_MISC_INFO *psGetMiscInfoIN,
					PVRSRV_BRIDGE_OUT_GET_MISC_INFO *psGetMiscInfoOUT,
					PVRSRV_PER_PROCESS_DATA *psPerProc)
{
#if defined (SUPPORT_SID_INTERFACE)
	PVRSRV_MISC_INFO_KM sMiscInfo = {0};
#endif
	PVRSRV_ERROR eError;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_GET_MISC_INFO);
#if defined (SUPPORT_SID_INTERFACE)
	sMiscInfo.ui32StateRequest = psGetMiscInfoIN->sMiscInfo.ui32StateRequest;
	sMiscInfo.ui32StatePresent = psGetMiscInfoIN->sMiscInfo.ui32StatePresent;
	sMiscInfo.ui32MemoryStrLen = psGetMiscInfoIN->sMiscInfo.ui32MemoryStrLen;
	sMiscInfo.pszMemoryStr     = psGetMiscInfoIN->sMiscInfo.pszMemoryStr;

	OSMemCopy(&sMiscInfo.sCacheOpCtl,
			  &psGetMiscInfoIN->sMiscInfo.sCacheOpCtl,
			  sizeof(sMiscInfo.sCacheOpCtl));
#else

	OSMemCopy(&psGetMiscInfoOUT->sMiscInfo,
			  &psGetMiscInfoIN->sMiscInfo,
			  sizeof(PVRSRV_MISC_INFO));
#endif

	if (((psGetMiscInfoIN->sMiscInfo.ui32StateRequest & PVRSRV_MISC_INFO_MEMSTATS_PRESENT) != 0) &&
	    ((psGetMiscInfoIN->sMiscInfo.ui32StateRequest & PVRSRV_MISC_INFO_DDKVERSION_PRESENT) != 0) &&
	    ((psGetMiscInfoIN->sMiscInfo.ui32StateRequest & PVRSRV_MISC_INFO_FREEMEM_PRESENT) != 0))
	{

		psGetMiscInfoOUT->eError = PVRSRV_ERROR_INVALID_PARAMS;
		return 0;
	}

	if (((psGetMiscInfoIN->sMiscInfo.ui32StateRequest & PVRSRV_MISC_INFO_MEMSTATS_PRESENT) != 0) ||
	    ((psGetMiscInfoIN->sMiscInfo.ui32StateRequest & PVRSRV_MISC_INFO_DDKVERSION_PRESENT) != 0) ||
	    ((psGetMiscInfoIN->sMiscInfo.ui32StateRequest & PVRSRV_MISC_INFO_FREEMEM_PRESENT) != 0))
	{

#if defined (SUPPORT_SID_INTERFACE)
		ASSIGN_AND_EXIT_ON_ERROR(psGetMiscInfoOUT->eError,
				OSAllocMem(PVRSRV_OS_PAGEABLE_HEAP,
						psGetMiscInfoOUT->sMiscInfo.ui32MemoryStrLen,
						(IMG_VOID **)&sMiscInfo.pszMemoryStr, 0,
						"Output string buffer"));
		psGetMiscInfoOUT->eError = PVRSRVGetMiscInfoKM(&sMiscInfo);


		eError = CopyToUserWrapper(psPerProc, ui32BridgeID,
									psGetMiscInfoIN->sMiscInfo.pszMemoryStr,
									sMiscInfo.pszMemoryStr,
									sMiscInfo.ui32MemoryStrLen);
#else
		ASSIGN_AND_EXIT_ON_ERROR(psGetMiscInfoOUT->eError,
				OSAllocMem(PVRSRV_OS_PAGEABLE_HEAP,
		                    psGetMiscInfoOUT->sMiscInfo.ui32MemoryStrLen,
		                    (IMG_VOID **)&psGetMiscInfoOUT->sMiscInfo.pszMemoryStr, 0,
							"Output string buffer"));

		psGetMiscInfoOUT->eError = PVRSRVGetMiscInfoKM(&psGetMiscInfoOUT->sMiscInfo);


		eError = CopyToUserWrapper(psPerProc, ui32BridgeID,
					   psGetMiscInfoIN->sMiscInfo.pszMemoryStr,
					   psGetMiscInfoOUT->sMiscInfo.pszMemoryStr,
					   psGetMiscInfoOUT->sMiscInfo.ui32MemoryStrLen);
#endif


#if defined (SUPPORT_SID_INTERFACE)
		OSFreeMem(PVRSRV_OS_PAGEABLE_HEAP,
				     sMiscInfo.ui32MemoryStrLen,
				     (IMG_VOID *)sMiscInfo.pszMemoryStr, 0);
#else
		OSFreeMem(PVRSRV_OS_PAGEABLE_HEAP,
			  psGetMiscInfoOUT->sMiscInfo.ui32MemoryStrLen,
			 (IMG_VOID *)psGetMiscInfoOUT->sMiscInfo.pszMemoryStr, 0);
#endif


		psGetMiscInfoOUT->sMiscInfo.pszMemoryStr = psGetMiscInfoIN->sMiscInfo.pszMemoryStr;

		if(eError != PVRSRV_OK)
		{

			PVR_DPF((PVR_DBG_ERROR, "PVRSRVGetMiscInfoBW Error copy to user"));
			return -EFAULT;
		}
	}
	else
	{
#if defined (SUPPORT_SID_INTERFACE)
		psGetMiscInfoOUT->eError = PVRSRVGetMiscInfoKM(&sMiscInfo);
#else
		psGetMiscInfoOUT->eError = PVRSRVGetMiscInfoKM(&psGetMiscInfoOUT->sMiscInfo);
#endif
	}


	if (psGetMiscInfoOUT->eError != PVRSRV_OK)
	{
		return 0;
	}


#if defined (SUPPORT_SID_INTERFACE)
	if (sMiscInfo.ui32StateRequest & PVRSRV_MISC_INFO_GLOBALEVENTOBJECT_PRESENT)
#else
		if (psGetMiscInfoIN->sMiscInfo.ui32StateRequest & PVRSRV_MISC_INFO_GLOBALEVENTOBJECT_PRESENT)
#endif
		{
			psGetMiscInfoOUT->eError = PVRSRVAllocHandle(psPerProc->psHandleBase,
													&psGetMiscInfoOUT->sMiscInfo.sGlobalEventObject.hOSEventKM,
#if defined (SUPPORT_SID_INTERFACE)
					sMiscInfo.sGlobalEventObject.hOSEventKM,
#else
													psGetMiscInfoOUT->sMiscInfo.sGlobalEventObject.hOSEventKM,
#endif
													PVRSRV_HANDLE_TYPE_SHARED_EVENT_OBJECT,
													PVRSRV_HANDLE_ALLOC_FLAG_SHARED);

			if (psGetMiscInfoOUT->eError != PVRSRV_OK)
			{
				return 0;
			}

#if defined (SUPPORT_SID_INTERFACE)
			OSMemCopy(&psGetMiscInfoOUT->sMiscInfo.sGlobalEventObject.szName,
							sMiscInfo.sGlobalEventObject.szName,
							EVENTOBJNAME_MAXLENGTH);

#endif
		}

#if defined (SUPPORT_SID_INTERFACE)
	if (sMiscInfo.hSOCTimerRegisterOSMemHandle)
#else
	if (psGetMiscInfoOUT->sMiscInfo.hSOCTimerRegisterOSMemHandle)
#endif
	{

		psGetMiscInfoOUT->eError = PVRSRVAllocHandle(psPerProc->psHandleBase,
						  &psGetMiscInfoOUT->sMiscInfo.hSOCTimerRegisterOSMemHandle,
#if defined (SUPPORT_SID_INTERFACE)
					sMiscInfo.hSOCTimerRegisterOSMemHandle,
#else
						  psGetMiscInfoOUT->sMiscInfo.hSOCTimerRegisterOSMemHandle,
#endif
						  PVRSRV_HANDLE_TYPE_SOC_TIMER,
						  PVRSRV_HANDLE_ALLOC_FLAG_SHARED);

		if (psGetMiscInfoOUT->eError != PVRSRV_OK)
		{
			return 0;
		}
	}
#if defined (SUPPORT_SID_INTERFACE)
	else
	{
		psGetMiscInfoOUT->sMiscInfo.hSOCTimerRegisterOSMemHandle = 0;
	}


	psGetMiscInfoOUT->sMiscInfo.ui32StateRequest = sMiscInfo.ui32StateRequest;
	psGetMiscInfoOUT->sMiscInfo.ui32StatePresent = sMiscInfo.ui32StatePresent;

	psGetMiscInfoOUT->sMiscInfo.pvSOCTimerRegisterKM = sMiscInfo.pvSOCTimerRegisterKM;
	psGetMiscInfoOUT->sMiscInfo.pvSOCTimerRegisterUM = sMiscInfo.pvSOCTimerRegisterUM;
	psGetMiscInfoOUT->sMiscInfo.pvSOCClockGateRegs   = sMiscInfo.pvSOCClockGateRegs;

	psGetMiscInfoOUT->sMiscInfo.ui32SOCClockGateRegsSize = sMiscInfo.ui32SOCClockGateRegsSize;

	OSMemCopy(&psGetMiscInfoOUT->sMiscInfo.aui32DDKVersion,
				&sMiscInfo.aui32DDKVersion,
				sizeof(psGetMiscInfoOUT->sMiscInfo.aui32DDKVersion));
	OSMemCopy(&psGetMiscInfoOUT->sMiscInfo.sCacheOpCtl,
				&sMiscInfo.sCacheOpCtl,
				sizeof(psGetMiscInfoOUT->sMiscInfo.sCacheOpCtl));
#endif

	return 0;
}

static IMG_INT
PVRSRVConnectBW(IMG_UINT32 ui32BridgeID,
				PVRSRV_BRIDGE_IN_CONNECT_SERVICES *psConnectServicesIN,
				PVRSRV_BRIDGE_OUT_CONNECT_SERVICES *psConnectServicesOUT,
				PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_CONNECT_SERVICES);

#if defined(PDUMP)

	if ((psConnectServicesIN->ui32Flags & SRV_FLAGS_PERSIST) != 0)
	{
		psPerProc->bPDumpPersistent = IMG_TRUE;
	}

#if defined(SUPPORT_PDUMP_MULTI_PROCESS)

	if ((psConnectServicesIN->ui32Flags & SRV_FLAGS_PDUMP_ACTIVE) != 0)
	{
		psPerProc->bPDumpActive = IMG_TRUE;
	}
#endif 
#else
	PVR_UNREFERENCED_PARAMETER(psConnectServicesIN);
#endif
	psConnectServicesOUT->hKernelServices = psPerProc->hPerProcData;
	psConnectServicesOUT->eError = PVRSRV_OK;

	return 0;
}

static IMG_INT
PVRSRVDisconnectBW(IMG_UINT32 ui32BridgeID,
				   IMG_VOID *psBridgeIn,
				   PVRSRV_BRIDGE_RETURN *psRetOUT,
				   PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVR_UNREFERENCED_PARAMETER(psPerProc);
	PVR_UNREFERENCED_PARAMETER(psBridgeIn);

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_DISCONNECT_SERVICES);


	psRetOUT->eError = PVRSRV_OK;

	return 0;
}

static IMG_INT
PVRSRVEnumerateDCBW(IMG_UINT32 ui32BridgeID,
					PVRSRV_BRIDGE_IN_ENUMCLASS *psEnumDispClassIN,
					PVRSRV_BRIDGE_OUT_ENUMCLASS *psEnumDispClassOUT,
					PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVR_UNREFERENCED_PARAMETER(psPerProc);

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_ENUM_CLASS);

	psEnumDispClassOUT->eError =
		PVRSRVEnumerateDCKM(psEnumDispClassIN->sDeviceClass,
							&psEnumDispClassOUT->ui32NumDevices,
							&psEnumDispClassOUT->ui32DevID[0]);

	return 0;
}

static IMG_INT
PVRSRVOpenDCDeviceBW(IMG_UINT32 ui32BridgeID,
					 PVRSRV_BRIDGE_IN_OPEN_DISPCLASS_DEVICE *psOpenDispClassDeviceIN,
					 PVRSRV_BRIDGE_OUT_OPEN_DISPCLASS_DEVICE *psOpenDispClassDeviceOUT,
					 PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_HANDLE hDevCookieInt;
	IMG_HANDLE hDispClassInfoInt;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_OPEN_DISPCLASS_DEVICE);

	NEW_HANDLE_BATCH_OR_ERROR(psOpenDispClassDeviceOUT->eError, psPerProc, 1)

	psOpenDispClassDeviceOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &hDevCookieInt,
						   psOpenDispClassDeviceIN->hDevCookie,
						   PVRSRV_HANDLE_TYPE_DEV_NODE);
	if(psOpenDispClassDeviceOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psOpenDispClassDeviceOUT->eError =
		PVRSRVOpenDCDeviceKM(psPerProc,
							 psOpenDispClassDeviceIN->ui32DeviceID,
							 hDevCookieInt,
							 &hDispClassInfoInt);

	if(psOpenDispClassDeviceOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	PVRSRVAllocHandleNR(psPerProc->psHandleBase,
					  &psOpenDispClassDeviceOUT->hDeviceKM,
					  hDispClassInfoInt,
					  PVRSRV_HANDLE_TYPE_DISP_INFO,
					  PVRSRV_HANDLE_ALLOC_FLAG_NONE);
	COMMIT_HANDLE_BATCH_OR_ERROR(psOpenDispClassDeviceOUT->eError, psPerProc)

	return 0;
}

static IMG_INT
PVRSRVCloseDCDeviceBW(IMG_UINT32 ui32BridgeID,
					  PVRSRV_BRIDGE_IN_CLOSE_DISPCLASS_DEVICE *psCloseDispClassDeviceIN,
					  PVRSRV_BRIDGE_RETURN *psRetOUT,
					  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_VOID *pvDispClassInfoInt;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_CLOSE_DISPCLASS_DEVICE);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvDispClassInfoInt,
						   psCloseDispClassDeviceIN->hDeviceKM,
						   PVRSRV_HANDLE_TYPE_DISP_INFO);

	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError = PVRSRVCloseDCDeviceKM(pvDispClassInfoInt, IMG_FALSE);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PVRSRVReleaseHandle(psPerProc->psHandleBase,
							psCloseDispClassDeviceIN->hDeviceKM,
							PVRSRV_HANDLE_TYPE_DISP_INFO);
	return 0;
}

static IMG_INT
PVRSRVEnumDCFormatsBW(IMG_UINT32 ui32BridgeID,
					  PVRSRV_BRIDGE_IN_ENUM_DISPCLASS_FORMATS *psEnumDispClassFormatsIN,
					  PVRSRV_BRIDGE_OUT_ENUM_DISPCLASS_FORMATS *psEnumDispClassFormatsOUT,
					  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_VOID *pvDispClassInfoInt;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_ENUM_DISPCLASS_FORMATS);

	psEnumDispClassFormatsOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvDispClassInfoInt,
						   psEnumDispClassFormatsIN->hDeviceKM,
						   PVRSRV_HANDLE_TYPE_DISP_INFO);
	if(psEnumDispClassFormatsOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psEnumDispClassFormatsOUT->eError =
		PVRSRVEnumDCFormatsKM(pvDispClassInfoInt,
							  &psEnumDispClassFormatsOUT->ui32Count,
							  psEnumDispClassFormatsOUT->asFormat);

	return 0;
}

static IMG_INT
PVRSRVEnumDCDimsBW(IMG_UINT32 ui32BridgeID,
				   PVRSRV_BRIDGE_IN_ENUM_DISPCLASS_DIMS *psEnumDispClassDimsIN,
				   PVRSRV_BRIDGE_OUT_ENUM_DISPCLASS_DIMS *psEnumDispClassDimsOUT,
				   PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_VOID *pvDispClassInfoInt;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_ENUM_DISPCLASS_DIMS);

	psEnumDispClassDimsOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvDispClassInfoInt,
						   psEnumDispClassDimsIN->hDeviceKM,
						   PVRSRV_HANDLE_TYPE_DISP_INFO);

	if(psEnumDispClassDimsOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psEnumDispClassDimsOUT->eError =
		PVRSRVEnumDCDimsKM(pvDispClassInfoInt,
						   &psEnumDispClassDimsIN->sFormat,
						   &psEnumDispClassDimsOUT->ui32Count,
						   psEnumDispClassDimsOUT->asDim);

	return 0;
}

static IMG_INT
PVRSRVGetDCSystemBufferBW(IMG_UINT32 ui32BridgeID,
						  PVRSRV_BRIDGE_IN_GET_DISPCLASS_SYSBUFFER *psGetDispClassSysBufferIN,  
						  PVRSRV_BRIDGE_OUT_GET_DISPCLASS_SYSBUFFER *psGetDispClassSysBufferOUT,
						  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_HANDLE hBufferInt;
	IMG_VOID *pvDispClassInfoInt;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_GET_DISPCLASS_SYSBUFFER);

	NEW_HANDLE_BATCH_OR_ERROR(psGetDispClassSysBufferOUT->eError, psPerProc, 1)

	psGetDispClassSysBufferOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvDispClassInfoInt,
						   psGetDispClassSysBufferIN->hDeviceKM,
						   PVRSRV_HANDLE_TYPE_DISP_INFO);
	if(psGetDispClassSysBufferOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psGetDispClassSysBufferOUT->eError =
		PVRSRVGetDCSystemBufferKM(pvDispClassInfoInt,
								  &hBufferInt);

	if(psGetDispClassSysBufferOUT->eError != PVRSRV_OK)
	{
		return 0;
	}


	PVRSRVAllocSubHandleNR(psPerProc->psHandleBase,
						 &psGetDispClassSysBufferOUT->hBuffer,
						 hBufferInt,
						 PVRSRV_HANDLE_TYPE_DISP_BUFFER,
						 (PVRSRV_HANDLE_ALLOC_FLAG)(PVRSRV_HANDLE_ALLOC_FLAG_PRIVATE | PVRSRV_HANDLE_ALLOC_FLAG_SHARED),
						 psGetDispClassSysBufferIN->hDeviceKM);

	COMMIT_HANDLE_BATCH_OR_ERROR(psGetDispClassSysBufferOUT->eError, psPerProc)

	return 0;
}

static IMG_INT
PVRSRVGetDCInfoBW(IMG_UINT32 ui32BridgeID,
				  PVRSRV_BRIDGE_IN_GET_DISPCLASS_INFO *psGetDispClassInfoIN,
				  PVRSRV_BRIDGE_OUT_GET_DISPCLASS_INFO *psGetDispClassInfoOUT,
				  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_VOID *pvDispClassInfo;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_GET_DISPCLASS_INFO);

	psGetDispClassInfoOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvDispClassInfo,
						   psGetDispClassInfoIN->hDeviceKM,
						   PVRSRV_HANDLE_TYPE_DISP_INFO);
	if(psGetDispClassInfoOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psGetDispClassInfoOUT->eError =
		PVRSRVGetDCInfoKM(pvDispClassInfo,
						  &psGetDispClassInfoOUT->sDisplayInfo);

	return 0;
}

static IMG_INT
PVRSRVCreateDCSwapChainBW(IMG_UINT32 ui32BridgeID,
						  PVRSRV_BRIDGE_IN_CREATE_DISPCLASS_SWAPCHAIN *psCreateDispClassSwapChainIN,
						  PVRSRV_BRIDGE_OUT_CREATE_DISPCLASS_SWAPCHAIN *psCreateDispClassSwapChainOUT,
						  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_VOID *pvDispClassInfo;
	IMG_HANDLE hSwapChainInt;
	IMG_UINT32  ui32SwapChainID;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_CREATE_DISPCLASS_SWAPCHAIN);

	NEW_HANDLE_BATCH_OR_ERROR(psCreateDispClassSwapChainOUT->eError, psPerProc, 1)

	psCreateDispClassSwapChainOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvDispClassInfo,
						   psCreateDispClassSwapChainIN->hDeviceKM,
						   PVRSRV_HANDLE_TYPE_DISP_INFO);

	if(psCreateDispClassSwapChainOUT->eError != PVRSRV_OK)
	{
		return 0;
	}


	ui32SwapChainID = psCreateDispClassSwapChainIN->ui32SwapChainID;

	psCreateDispClassSwapChainOUT->eError =
		PVRSRVCreateDCSwapChainKM(psPerProc, pvDispClassInfo,
								  psCreateDispClassSwapChainIN->ui32Flags,
								  &psCreateDispClassSwapChainIN->sDstSurfAttrib,
								  &psCreateDispClassSwapChainIN->sSrcSurfAttrib,
								  psCreateDispClassSwapChainIN->ui32BufferCount,
								  psCreateDispClassSwapChainIN->ui32OEMFlags,
								  &hSwapChainInt,
								  &ui32SwapChainID);

	if(psCreateDispClassSwapChainOUT->eError != PVRSRV_OK)
	{
		return 0;
	}


	psCreateDispClassSwapChainOUT->ui32SwapChainID = ui32SwapChainID;

	PVRSRVAllocSubHandleNR(psPerProc->psHandleBase,
					  &psCreateDispClassSwapChainOUT->hSwapChain,
					  hSwapChainInt,
					  PVRSRV_HANDLE_TYPE_DISP_SWAP_CHAIN,
					  PVRSRV_HANDLE_ALLOC_FLAG_NONE,
					  psCreateDispClassSwapChainIN->hDeviceKM);

	COMMIT_HANDLE_BATCH_OR_ERROR(psCreateDispClassSwapChainOUT->eError, psPerProc)

	return 0;
}

static IMG_INT
PVRSRVDestroyDCSwapChainBW(IMG_UINT32 ui32BridgeID,
						   PVRSRV_BRIDGE_IN_DESTROY_DISPCLASS_SWAPCHAIN *psDestroyDispClassSwapChainIN,
						   PVRSRV_BRIDGE_RETURN *psRetOUT,
						   PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_VOID *pvSwapChain;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_DESTROY_DISPCLASS_SWAPCHAIN);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase, &pvSwapChain,
						   psDestroyDispClassSwapChainIN->hSwapChain,
						   PVRSRV_HANDLE_TYPE_DISP_SWAP_CHAIN);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PVRSRVDestroyDCSwapChainKM(pvSwapChain);

	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PVRSRVReleaseHandle(psPerProc->psHandleBase,
							psDestroyDispClassSwapChainIN->hSwapChain,
							PVRSRV_HANDLE_TYPE_DISP_SWAP_CHAIN);

	return 0;
}

static IMG_INT
PVRSRVSetDCDstRectBW(IMG_UINT32 ui32BridgeID,
					 PVRSRV_BRIDGE_IN_SET_DISPCLASS_RECT *psSetDispClassDstRectIN,
					 PVRSRV_BRIDGE_RETURN *psRetOUT,
					 PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_VOID *pvDispClassInfo;
	IMG_VOID *pvSwapChain;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_SET_DISPCLASS_DSTRECT);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvDispClassInfo,
						   psSetDispClassDstRectIN->hDeviceKM,
						   PVRSRV_HANDLE_TYPE_DISP_INFO);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvSwapChain,
						   psSetDispClassDstRectIN->hSwapChain,
						   PVRSRV_HANDLE_TYPE_DISP_SWAP_CHAIN);

	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PVRSRVSetDCDstRectKM(pvDispClassInfo,
							 pvSwapChain,
							 &psSetDispClassDstRectIN->sRect);

	return 0;
}

static IMG_INT
PVRSRVSetDCSrcRectBW(IMG_UINT32 ui32BridgeID,
					 PVRSRV_BRIDGE_IN_SET_DISPCLASS_RECT *psSetDispClassSrcRectIN,
					 PVRSRV_BRIDGE_RETURN *psRetOUT,
					 PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_VOID *pvDispClassInfo;
	IMG_VOID *pvSwapChain;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_SET_DISPCLASS_SRCRECT);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvDispClassInfo,
						   psSetDispClassSrcRectIN->hDeviceKM,
						   PVRSRV_HANDLE_TYPE_DISP_INFO);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvSwapChain,
						   psSetDispClassSrcRectIN->hSwapChain,
						   PVRSRV_HANDLE_TYPE_DISP_SWAP_CHAIN);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PVRSRVSetDCSrcRectKM(pvDispClassInfo,
							 pvSwapChain,
							 &psSetDispClassSrcRectIN->sRect);

	return 0;
}

static IMG_INT
PVRSRVSetDCDstColourKeyBW(IMG_UINT32 ui32BridgeID,
						  PVRSRV_BRIDGE_IN_SET_DISPCLASS_COLOURKEY *psSetDispClassColKeyIN,
						  PVRSRV_BRIDGE_RETURN *psRetOUT,
						  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_VOID *pvDispClassInfo;
	IMG_VOID *pvSwapChain;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_SET_DISPCLASS_DSTCOLOURKEY);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvDispClassInfo,
						   psSetDispClassColKeyIN->hDeviceKM,
						   PVRSRV_HANDLE_TYPE_DISP_INFO);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvSwapChain,
						   psSetDispClassColKeyIN->hSwapChain,
						   PVRSRV_HANDLE_TYPE_DISP_SWAP_CHAIN);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PVRSRVSetDCDstColourKeyKM(pvDispClassInfo,
								  pvSwapChain,
								  psSetDispClassColKeyIN->ui32CKColour);

	return 0;
}

static IMG_INT
PVRSRVSetDCSrcColourKeyBW(IMG_UINT32 ui32BridgeID,
						  PVRSRV_BRIDGE_IN_SET_DISPCLASS_COLOURKEY *psSetDispClassColKeyIN,
						  PVRSRV_BRIDGE_RETURN *psRetOUT,
						  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_VOID *pvDispClassInfo;
	IMG_VOID *pvSwapChain;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_SET_DISPCLASS_SRCCOLOURKEY);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvDispClassInfo,
						   psSetDispClassColKeyIN->hDeviceKM,
						   PVRSRV_HANDLE_TYPE_DISP_INFO);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvSwapChain,
						   psSetDispClassColKeyIN->hSwapChain,
						   PVRSRV_HANDLE_TYPE_DISP_SWAP_CHAIN);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PVRSRVSetDCSrcColourKeyKM(pvDispClassInfo,
								  pvSwapChain,
								  psSetDispClassColKeyIN->ui32CKColour);

	return 0;
}

static IMG_INT
PVRSRVGetDCBuffersBW(IMG_UINT32 ui32BridgeID,
					 PVRSRV_BRIDGE_IN_GET_DISPCLASS_BUFFERS *psGetDispClassBuffersIN,
					 PVRSRV_BRIDGE_OUT_GET_DISPCLASS_BUFFERS *psGetDispClassBuffersOUT,
					 PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_VOID    *pvDispClassInfo;
	IMG_VOID    *pvSwapChain;
	IMG_UINT32   i;
#if defined (SUPPORT_SID_INTERFACE)
	IMG_HANDLE  *pahBuffer;
#endif

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_GET_DISPCLASS_BUFFERS);

	NEW_HANDLE_BATCH_OR_ERROR(psGetDispClassBuffersOUT->eError, psPerProc, PVRSRV_MAX_DC_SWAPCHAIN_BUFFERS)

	psGetDispClassBuffersOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvDispClassInfo,
						   psGetDispClassBuffersIN->hDeviceKM,
						   PVRSRV_HANDLE_TYPE_DISP_INFO);
	if(psGetDispClassBuffersOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psGetDispClassBuffersOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvSwapChain,
						   psGetDispClassBuffersIN->hSwapChain,
						   PVRSRV_HANDLE_TYPE_DISP_SWAP_CHAIN);
	if(psGetDispClassBuffersOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

#if defined (SUPPORT_SID_INTERFACE)
	psGetDispClassBuffersOUT->eError = OSAllocMem(PVRSRV_OS_PAGEABLE_HEAP,
													sizeof(IMG_HANDLE) * PVRSRV_MAX_DC_SWAPCHAIN_BUFFERS,
													(IMG_PVOID *)&pahBuffer, 0,
													"Temp Swapchain Buffers");

	if (psGetDispClassBuffersOUT->eError != PVRSRV_OK)
	{
		return 0;
	}
#endif

	psGetDispClassBuffersOUT->eError =
		PVRSRVGetDCBuffersKM(pvDispClassInfo,
							 pvSwapChain,
							 &psGetDispClassBuffersOUT->ui32BufferCount,
#if defined (SUPPORT_SID_INTERFACE)
				pahBuffer);
#else
				psGetDispClassBuffersOUT->ahBuffer);
#endif
	if (psGetDispClassBuffersOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	PVR_ASSERT(psGetDispClassBuffersOUT->ui32BufferCount <= PVRSRV_MAX_DC_SWAPCHAIN_BUFFERS);

	for(i = 0; i < psGetDispClassBuffersOUT->ui32BufferCount; i++)
	{
#if defined (SUPPORT_SID_INTERFACE)
		IMG_SID hBufferExt;
#else
		IMG_HANDLE hBufferExt;
#endif


#if defined (SUPPORT_SID_INTERFACE)
		PVRSRVAllocSubHandleNR(psPerProc->psHandleBase,
							&hBufferExt,
							pahBuffer[i],
							PVRSRV_HANDLE_TYPE_DISP_BUFFER,
							(PVRSRV_HANDLE_ALLOC_FLAG)(PVRSRV_HANDLE_ALLOC_FLAG_PRIVATE | PVRSRV_HANDLE_ALLOC_FLAG_SHARED),
							psGetDispClassBuffersIN->hSwapChain);
#else
		PVRSRVAllocSubHandleNR(psPerProc->psHandleBase,
							 &hBufferExt,
							 psGetDispClassBuffersOUT->ahBuffer[i],
							 PVRSRV_HANDLE_TYPE_DISP_BUFFER,
							 (PVRSRV_HANDLE_ALLOC_FLAG)(PVRSRV_HANDLE_ALLOC_FLAG_PRIVATE | PVRSRV_HANDLE_ALLOC_FLAG_SHARED),
							 psGetDispClassBuffersIN->hSwapChain);
#endif

		psGetDispClassBuffersOUT->ahBuffer[i] = hBufferExt;
	}

#if defined (SUPPORT_SID_INTERFACE)
	OSFreeMem(PVRSRV_OS_PAGEABLE_HEAP,
				sizeof(IMG_HANDLE) * PVRSRV_MAX_DC_SWAPCHAIN_BUFFERS,
				(IMG_PVOID)pahBuffer, 0);
#endif

	COMMIT_HANDLE_BATCH_OR_ERROR(psGetDispClassBuffersOUT->eError, psPerProc)

	return 0;
}

static IMG_INT
PVRSRVSwapToDCBufferBW(IMG_UINT32 ui32BridgeID,
					   PVRSRV_BRIDGE_IN_SWAP_DISPCLASS_TO_BUFFER *psSwapDispClassBufferIN,
					   PVRSRV_BRIDGE_RETURN *psRetOUT,
					   PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_VOID  *pvDispClassInfo;
	IMG_VOID  *pvSwapChainBuf;
#if defined (SUPPORT_SID_INTERFACE)
	IMG_HANDLE hPrivateTag;
#endif

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_SWAP_DISPCLASS_TO_BUFFER);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvDispClassInfo,
						   psSwapDispClassBufferIN->hDeviceKM,
						   PVRSRV_HANDLE_TYPE_DISP_INFO);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PVRSRVLookupSubHandle(psPerProc->psHandleBase,
						   &pvSwapChainBuf,
						   psSwapDispClassBufferIN->hBuffer,
						   PVRSRV_HANDLE_TYPE_DISP_BUFFER,
						   psSwapDispClassBufferIN->hDeviceKM);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

#if defined (SUPPORT_SID_INTERFACE)
	if (psSwapDispClassBufferIN->hPrivateTag != 0)
	{
		psRetOUT->eError =
			PVRSRVLookupSubHandle(psPerProc->psHandleBase,
								&hPrivateTag,
								psSwapDispClassBufferIN->hPrivateTag,
								PVRSRV_HANDLE_TYPE_DISP_BUFFER,
								psSwapDispClassBufferIN->hDeviceKM);
		if(psRetOUT->eError != PVRSRV_OK)
		{
			return 0;
		}
	}
	else
	{
		hPrivateTag = IMG_NULL;
	}
#endif


	psRetOUT->eError =
		PVRSRVSwapToDCBufferKM(pvDispClassInfo,
							   pvSwapChainBuf,
							   psSwapDispClassBufferIN->ui32SwapInterval,
#if defined (SUPPORT_SID_INTERFACE)
				hPrivateTag,
#else
							   psSwapDispClassBufferIN->hPrivateTag,
#endif
							   psSwapDispClassBufferIN->ui32ClipRectCount,
							   psSwapDispClassBufferIN->sClipRect);

	return 0;
}

static IMG_INT
PVRSRVSwapToDCSystemBW(IMG_UINT32 ui32BridgeID,
					   PVRSRV_BRIDGE_IN_SWAP_DISPCLASS_TO_SYSTEM *psSwapDispClassSystemIN,
					   PVRSRV_BRIDGE_RETURN *psRetOUT,
					   PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_VOID *pvDispClassInfo;
	IMG_VOID *pvSwapChain;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_SWAP_DISPCLASS_TO_SYSTEM);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvDispClassInfo,
						   psSwapDispClassSystemIN->hDeviceKM,
						   PVRSRV_HANDLE_TYPE_DISP_INFO);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PVRSRVLookupSubHandle(psPerProc->psHandleBase,
						   &pvSwapChain,
						   psSwapDispClassSystemIN->hSwapChain,
						   PVRSRV_HANDLE_TYPE_DISP_SWAP_CHAIN,
						   psSwapDispClassSystemIN->hDeviceKM);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}
	psRetOUT->eError =
		PVRSRVSwapToDCSystemKM(pvDispClassInfo,
							   pvSwapChain);

	return 0;
}

static IMG_INT
PVRSRVOpenBCDeviceBW(IMG_UINT32 ui32BridgeID,
					 PVRSRV_BRIDGE_IN_OPEN_BUFFERCLASS_DEVICE *psOpenBufferClassDeviceIN,
					 PVRSRV_BRIDGE_OUT_OPEN_BUFFERCLASS_DEVICE *psOpenBufferClassDeviceOUT,
					 PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_HANDLE hDevCookieInt;
	IMG_HANDLE hBufClassInfo;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_OPEN_BUFFERCLASS_DEVICE);

	NEW_HANDLE_BATCH_OR_ERROR(psOpenBufferClassDeviceOUT->eError, psPerProc, 1)

	psOpenBufferClassDeviceOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &hDevCookieInt,
						   psOpenBufferClassDeviceIN->hDevCookie,
						   PVRSRV_HANDLE_TYPE_DEV_NODE);
	if(psOpenBufferClassDeviceOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psOpenBufferClassDeviceOUT->eError =
		PVRSRVOpenBCDeviceKM(psPerProc,
							 psOpenBufferClassDeviceIN->ui32DeviceID,
							 hDevCookieInt,
							 &hBufClassInfo);
	if(psOpenBufferClassDeviceOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	PVRSRVAllocHandleNR(psPerProc->psHandleBase,
					  &psOpenBufferClassDeviceOUT->hDeviceKM,
					  hBufClassInfo,
					  PVRSRV_HANDLE_TYPE_BUF_INFO,
					  PVRSRV_HANDLE_ALLOC_FLAG_NONE);

	COMMIT_HANDLE_BATCH_OR_ERROR(psOpenBufferClassDeviceOUT->eError, psPerProc)

	return 0;
}

static IMG_INT
PVRSRVCloseBCDeviceBW(IMG_UINT32 ui32BridgeID,
					  PVRSRV_BRIDGE_IN_CLOSE_BUFFERCLASS_DEVICE *psCloseBufferClassDeviceIN,
					  PVRSRV_BRIDGE_RETURN *psRetOUT,
					  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_VOID *pvBufClassInfo;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_CLOSE_BUFFERCLASS_DEVICE);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvBufClassInfo,
						   psCloseBufferClassDeviceIN->hDeviceKM,
						   PVRSRV_HANDLE_TYPE_BUF_INFO);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError =
		PVRSRVCloseBCDeviceKM(pvBufClassInfo, IMG_FALSE);

	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError = PVRSRVReleaseHandle(psPerProc->psHandleBase,
										   psCloseBufferClassDeviceIN->hDeviceKM,
										   PVRSRV_HANDLE_TYPE_BUF_INFO);

	return 0;
}

static IMG_INT
PVRSRVGetBCInfoBW(IMG_UINT32 ui32BridgeID,
				  PVRSRV_BRIDGE_IN_GET_BUFFERCLASS_INFO *psGetBufferClassInfoIN,
				  PVRSRV_BRIDGE_OUT_GET_BUFFERCLASS_INFO *psGetBufferClassInfoOUT,
				  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_VOID *pvBufClassInfo;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_GET_BUFFERCLASS_INFO);

	psGetBufferClassInfoOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvBufClassInfo,
						   psGetBufferClassInfoIN->hDeviceKM,
						   PVRSRV_HANDLE_TYPE_BUF_INFO);
	if(psGetBufferClassInfoOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psGetBufferClassInfoOUT->eError =
		PVRSRVGetBCInfoKM(pvBufClassInfo,
						  &psGetBufferClassInfoOUT->sBufferInfo);
	return 0;
}

static IMG_INT
PVRSRVGetBCBufferBW(IMG_UINT32 ui32BridgeID,
					PVRSRV_BRIDGE_IN_GET_BUFFERCLASS_BUFFER *psGetBufferClassBufferIN,
					PVRSRV_BRIDGE_OUT_GET_BUFFERCLASS_BUFFER *psGetBufferClassBufferOUT,
					PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_VOID *pvBufClassInfo;
	IMG_HANDLE hBufferInt;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_GET_BUFFERCLASS_BUFFER);

	NEW_HANDLE_BATCH_OR_ERROR(psGetBufferClassBufferOUT->eError, psPerProc, 1)

	psGetBufferClassBufferOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &pvBufClassInfo,
						   psGetBufferClassBufferIN->hDeviceKM,
						   PVRSRV_HANDLE_TYPE_BUF_INFO);
	if(psGetBufferClassBufferOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psGetBufferClassBufferOUT->eError =
		PVRSRVGetBCBufferKM(pvBufClassInfo,
							psGetBufferClassBufferIN->ui32BufferIndex,
							&hBufferInt);

	if(psGetBufferClassBufferOUT->eError != PVRSRV_OK)
	{
		return 0;
	}


	PVRSRVAllocSubHandleNR(psPerProc->psHandleBase,
						 &psGetBufferClassBufferOUT->hBuffer,
						 hBufferInt,
						 PVRSRV_HANDLE_TYPE_BUF_BUFFER,
						 (PVRSRV_HANDLE_ALLOC_FLAG)(PVRSRV_HANDLE_ALLOC_FLAG_PRIVATE |  PVRSRV_HANDLE_ALLOC_FLAG_SHARED),
						 psGetBufferClassBufferIN->hDeviceKM);

	COMMIT_HANDLE_BATCH_OR_ERROR(psGetBufferClassBufferOUT->eError, psPerProc)

	return 0;
}


static IMG_INT
PVRSRVAllocSharedSysMemoryBW(IMG_UINT32 ui32BridgeID,
							 PVRSRV_BRIDGE_IN_ALLOC_SHARED_SYS_MEM *psAllocSharedSysMemIN,
							 PVRSRV_BRIDGE_OUT_ALLOC_SHARED_SYS_MEM *psAllocSharedSysMemOUT,
							 PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_KERNEL_MEM_INFO *psKernelMemInfo;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_ALLOC_SHARED_SYS_MEM);

	NEW_HANDLE_BATCH_OR_ERROR(psAllocSharedSysMemOUT->eError, psPerProc, 1)

	psAllocSharedSysMemOUT->eError =
		PVRSRVAllocSharedSysMemoryKM(psPerProc,
									 psAllocSharedSysMemIN->ui32Flags,
									 psAllocSharedSysMemIN->ui32Size,
									 &psKernelMemInfo);
	if(psAllocSharedSysMemOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	OSMemSet(&psAllocSharedSysMemOUT->sClientMemInfo,
			0,
			sizeof(psAllocSharedSysMemOUT->sClientMemInfo));

	psAllocSharedSysMemOUT->sClientMemInfo.pvLinAddrKM =
			psKernelMemInfo->pvLinAddrKM;

	psAllocSharedSysMemOUT->sClientMemInfo.pvLinAddr = 0;
	psAllocSharedSysMemOUT->sClientMemInfo.ui32Flags =
		psKernelMemInfo->ui32Flags;
	psAllocSharedSysMemOUT->sClientMemInfo.uAllocSize =
		psKernelMemInfo->uAllocSize;
#if defined (SUPPORT_SID_INTERFACE)
	if (psKernelMemInfo->sMemBlk.hOSMemHandle != IMG_NULL)
	{
		PVRSRVAllocHandleNR(psPerProc->psHandleBase,
							&psAllocSharedSysMemOUT->sClientMemInfo.hMappingInfo,
							psKernelMemInfo->sMemBlk.hOSMemHandle,
							PVRSRV_HANDLE_TYPE_SHARED_SYS_MEM_INFO,
							PVRSRV_HANDLE_ALLOC_FLAG_NONE);
	}
	else
	{
		psAllocSharedSysMemOUT->sClientMemInfo.hMappingInfo = 0;
	}
#else
	psAllocSharedSysMemOUT->sClientMemInfo.hMappingInfo = psKernelMemInfo->sMemBlk.hOSMemHandle;
#endif

	PVRSRVAllocHandleNR(psPerProc->psHandleBase,
					  &psAllocSharedSysMemOUT->sClientMemInfo.hKernelMemInfo,
					  psKernelMemInfo,
					  PVRSRV_HANDLE_TYPE_SHARED_SYS_MEM_INFO,
					  PVRSRV_HANDLE_ALLOC_FLAG_NONE);

	COMMIT_HANDLE_BATCH_OR_ERROR(psAllocSharedSysMemOUT->eError, psPerProc)

	return 0;
}

static IMG_INT
PVRSRVFreeSharedSysMemoryBW(IMG_UINT32 ui32BridgeID,
							PVRSRV_BRIDGE_IN_FREE_SHARED_SYS_MEM *psFreeSharedSysMemIN,
							PVRSRV_BRIDGE_OUT_FREE_SHARED_SYS_MEM *psFreeSharedSysMemOUT,
							PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_KERNEL_MEM_INFO *psKernelMemInfo;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_FREE_SHARED_SYS_MEM);

	psFreeSharedSysMemOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
						   (IMG_VOID **)&psKernelMemInfo,
#if defined (SUPPORT_SID_INTERFACE)
						   psFreeSharedSysMemIN->hKernelMemInfo,
#else
						   psFreeSharedSysMemIN->psKernelMemInfo,
#endif
						   PVRSRV_HANDLE_TYPE_SHARED_SYS_MEM_INFO);

	if(psFreeSharedSysMemOUT->eError != PVRSRV_OK)
		return 0;

	psFreeSharedSysMemOUT->eError =
		PVRSRVFreeSharedSysMemoryKM(psKernelMemInfo);
	if(psFreeSharedSysMemOUT->eError != PVRSRV_OK)
		return 0;
#if defined (SUPPORT_SID_INTERFACE)
	if (psFreeSharedSysMemIN->hMappingInfo != 0)
	{
		psFreeSharedSysMemOUT->eError =
			PVRSRVReleaseHandle(psPerProc->psHandleBase,
								psFreeSharedSysMemIN->hMappingInfo,
								PVRSRV_HANDLE_TYPE_SHARED_SYS_MEM_INFO);
		if(psFreeSharedSysMemOUT->eError != PVRSRV_OK)
		{
			return 0;
		}
	}
#endif

	psFreeSharedSysMemOUT->eError =
		PVRSRVReleaseHandle(psPerProc->psHandleBase,
#if defined (SUPPORT_SID_INTERFACE)
							psFreeSharedSysMemIN->hKernelMemInfo,
#else
							psFreeSharedSysMemIN->psKernelMemInfo,
#endif
							PVRSRV_HANDLE_TYPE_SHARED_SYS_MEM_INFO);
	return 0;
}

static IMG_INT
PVRSRVMapMemInfoMemBW(IMG_UINT32 ui32BridgeID,
					  PVRSRV_BRIDGE_IN_MAP_MEMINFO_MEM *psMapMemInfoMemIN,
					  PVRSRV_BRIDGE_OUT_MAP_MEMINFO_MEM *psMapMemInfoMemOUT,
					  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_KERNEL_MEM_INFO *psKernelMemInfo;
	PVRSRV_HANDLE_TYPE eHandleType;
#if defined (SUPPORT_SID_INTERFACE)
	IMG_SID     hParent;
#else
	IMG_HANDLE  hParent;
#endif
	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_MAP_MEMINFO_MEM);

	NEW_HANDLE_BATCH_OR_ERROR(psMapMemInfoMemOUT->eError, psPerProc, 2)

	psMapMemInfoMemOUT->eError =
		PVRSRVLookupHandleAnyType(psPerProc->psHandleBase,
						   (IMG_VOID **)&psKernelMemInfo,
						   &eHandleType,
						   psMapMemInfoMemIN->hKernelMemInfo);
	if(psMapMemInfoMemOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	switch (eHandleType)
	{
#if defined(PVR_SECURE_HANDLES) || defined (SUPPORT_SID_INTERFACE)
		case PVRSRV_HANDLE_TYPE_MEM_INFO:
		case PVRSRV_HANDLE_TYPE_MEM_INFO_REF:
		case PVRSRV_HANDLE_TYPE_SHARED_SYS_MEM_INFO:
#else
		case PVRSRV_HANDLE_TYPE_NONE:
#endif
			break;
		default:
			psMapMemInfoMemOUT->eError = PVRSRV_ERROR_INVALID_HANDLE_TYPE;
			return 0;
	}


	psMapMemInfoMemOUT->eError =
		PVRSRVGetParentHandle(psPerProc->psHandleBase,
					&hParent,
					psMapMemInfoMemIN->hKernelMemInfo,
					eHandleType);
	if (psMapMemInfoMemOUT->eError != PVRSRV_OK)
	{
		return 0;
	}
#if defined (SUPPORT_SID_INTERFACE)
	if (hParent == 0)
#else
	if (hParent == IMG_NULL)
#endif
	{
		hParent = psMapMemInfoMemIN->hKernelMemInfo;
	}

	OSMemSet(&psMapMemInfoMemOUT->sClientMemInfo,
			0,
			sizeof(psMapMemInfoMemOUT->sClientMemInfo));

	psMapMemInfoMemOUT->sClientMemInfo.pvLinAddrKM =
			psKernelMemInfo->pvLinAddrKM;

	psMapMemInfoMemOUT->sClientMemInfo.pvLinAddr = 0;
	psMapMemInfoMemOUT->sClientMemInfo.sDevVAddr =
		psKernelMemInfo->sDevVAddr;
	psMapMemInfoMemOUT->sClientMemInfo.ui32Flags =
		psKernelMemInfo->ui32Flags;
	psMapMemInfoMemOUT->sClientMemInfo.uAllocSize =
		psKernelMemInfo->uAllocSize;
#if defined (SUPPORT_SID_INTERFACE)
	if (psKernelMemInfo->sMemBlk.hOSMemHandle != IMG_NULL)
	{
		PVRSRVAllocSubHandleNR(psPerProc->psHandleBase,
						&psMapMemInfoMemOUT->sClientMemInfo.hMappingInfo,
						psKernelMemInfo->sMemBlk.hOSMemHandle,
						PVRSRV_HANDLE_TYPE_MEM_INFO_REF,
						PVRSRV_HANDLE_ALLOC_FLAG_MULTI,
						hParent);
	}
	else
	{
		psMapMemInfoMemOUT->sClientMemInfo.hMappingInfo = 0;
	}
#else
	psMapMemInfoMemOUT->sClientMemInfo.hMappingInfo = psKernelMemInfo->sMemBlk.hOSMemHandle;
#endif

	PVRSRVAllocSubHandleNR(psPerProc->psHandleBase,
					  &psMapMemInfoMemOUT->sClientMemInfo.hKernelMemInfo,
					  psKernelMemInfo,
					  PVRSRV_HANDLE_TYPE_MEM_INFO_REF,
					  PVRSRV_HANDLE_ALLOC_FLAG_MULTI,
					  hParent);

	if(psKernelMemInfo->ui32Flags & PVRSRV_MEM_NO_SYNCOBJ)
	{

		OSMemSet(&psMapMemInfoMemOUT->sClientSyncInfo,
				0,
				sizeof (PVRSRV_CLIENT_SYNC_INFO));
	}
	else
	{

#if !defined(PVRSRV_DISABLE_UM_SYNCOBJ_MAPPINGS)
		psMapMemInfoMemOUT->sClientSyncInfo.psSyncData =
			psKernelMemInfo->psKernelSyncInfo->psSyncData;
		psMapMemInfoMemOUT->sClientSyncInfo.sWriteOpsCompleteDevVAddr =
			psKernelMemInfo->psKernelSyncInfo->sWriteOpsCompleteDevVAddr;
		psMapMemInfoMemOUT->sClientSyncInfo.sReadOpsCompleteDevVAddr =
			psKernelMemInfo->psKernelSyncInfo->sReadOpsCompleteDevVAddr;

#if defined (SUPPORT_SID_INTERFACE)
		if (psKernelMemInfo->psKernelSyncInfo->psSyncDataMemInfoKM->sMemBlk.hOSMemHandle != IMG_NULL)
		{
			PVRSRVAllocSubHandleNR(psPerProc->psHandleBase,
								&psMapMemInfoMemOUT->sClientSyncInfo.hMappingInfo,
								psKernelMemInfo->psKernelSyncInfo->psSyncDataMemInfoKM->sMemBlk.hOSMemHandle,
								PVRSRV_HANDLE_TYPE_SYNC_INFO,
								PVRSRV_HANDLE_ALLOC_FLAG_MULTI,
								psMapMemInfoMemOUT->sClientMemInfo.hKernelMemInfo);
		}
		else
		{
			psMapMemInfoMemOUT->sClientSyncInfo.hMappingInfo = 0;
		}
#else
		psMapMemInfoMemOUT->sClientSyncInfo.hMappingInfo =
			psKernelMemInfo->psKernelSyncInfo->psSyncDataMemInfoKM->sMemBlk.hOSMemHandle;
#endif
#endif

		psMapMemInfoMemOUT->sClientMemInfo.psClientSyncInfo = &psMapMemInfoMemOUT->sClientSyncInfo;

		PVRSRVAllocSubHandleNR(psPerProc->psHandleBase,
							 &psMapMemInfoMemOUT->sClientSyncInfo.hKernelSyncInfo,
							 psKernelMemInfo->psKernelSyncInfo,
							 PVRSRV_HANDLE_TYPE_SYNC_INFO,
							 PVRSRV_HANDLE_ALLOC_FLAG_MULTI,
							 psMapMemInfoMemOUT->sClientMemInfo.hKernelMemInfo);
	}

	COMMIT_HANDLE_BATCH_OR_ERROR(psMapMemInfoMemOUT->eError, psPerProc)

	return 0;
}

IMG_INT
DummyBW(IMG_UINT32 ui32BridgeID,
		IMG_VOID *psBridgeIn,
		IMG_VOID *psBridgeOut,
		PVRSRV_PER_PROCESS_DATA *psPerProc)
{
#if !defined(DEBUG)
	PVR_UNREFERENCED_PARAMETER(ui32BridgeID);
#endif
	PVR_UNREFERENCED_PARAMETER(psBridgeIn);
	PVR_UNREFERENCED_PARAMETER(psBridgeOut);
	PVR_UNREFERENCED_PARAMETER(psPerProc);

#if defined(DEBUG_BRIDGE_KM)
	PVR_DPF((PVR_DBG_ERROR, "%s: BRIDGE ERROR: BridgeID %u (%s) mapped to "
			 "Dummy Wrapper (probably not what you want!)",
			 __FUNCTION__, ui32BridgeID, g_BridgeDispatchTable[ui32BridgeID].pszIOCName));
#else
	PVR_DPF((PVR_DBG_ERROR, "%s: BRIDGE ERROR: BridgeID %u mapped to "
			 "Dummy Wrapper (probably not what you want!)",
			 __FUNCTION__, ui32BridgeID));
#endif
	return -ENOTTY;
}


IMG_VOID
_SetDispatchTableEntry(IMG_UINT32 ui32Index,
                       const IMG_CHAR *pszIOCName,
                       BridgeWrapperFunction pfFunction,
                       const IMG_CHAR *pszFunctionName,
		       size_t in_size, size_t out_size)
{
	static IMG_UINT32 ui32PrevIndex = ~0U;     /* ~0U not ~0UL: on 64-bit LP64, ~0UL is
	   0xFFFFFFFFFFFFFFFF which truncates to 0xFFFFFFFF here, then the first-entry guard
	   `ui32PrevIndex != ~0UL` compares 0xFFFFFFFF != 0xFFFF...FFFF = true and the gap
	   warning fires spuriously on index 0. Keep the sentinel 32-bit to match the field. */
#if !defined(DEBUG)
	PVR_UNREFERENCED_PARAMETER(pszIOCName);
#endif
#if !defined(DEBUG_BRIDGE_KM_DISPATCH_TABLE) && !defined(DEBUG_BRIDGE_KM)
	PVR_UNREFERENCED_PARAMETER(pszFunctionName);
#endif

#if defined(DEBUG_BRIDGE_KM_DISPATCH_TABLE)

	PVR_DPF((PVR_DBG_WARNING, "%s: %d %s %s", __FUNCTION__, ui32Index, pszIOCName, pszFunctionName));
#endif


	if(g_BridgeDispatchTable[ui32Index].pfFunction)
	{
#if defined(DEBUG_BRIDGE_KM)
		PVR_DPF((PVR_DBG_ERROR,
				 "%s: BUG!: Adding dispatch table entry for %s clobbers an existing entry for %s",
				 __FUNCTION__, pszIOCName, g_BridgeDispatchTable[ui32Index].pszIOCName));
#else
		PVR_DPF((PVR_DBG_ERROR,
				 "%s: BUG!: Adding dispatch table entry for %s clobbers an existing entry (index=%u)",
				 __FUNCTION__, pszIOCName, ui32Index));
#endif
		PVR_DPF((PVR_DBG_ERROR, "NOTE: Enabling DEBUG_BRIDGE_KM_DISPATCH_TABLE may help debug this issue."));
	}


	if((ui32PrevIndex != ~0U) &&
	   ((ui32Index >= ui32PrevIndex + DISPATCH_TABLE_GAP_THRESHOLD) ||
		(ui32Index <= ui32PrevIndex)))
	{
#if defined(DEBUG_BRIDGE_KM)
		PVR_DPF((PVR_DBG_WARNING,
				 "%s: There is a gap in the dispatch table between indices %u (%s) and %u (%s)",
				 __FUNCTION__, ui32PrevIndex, g_BridgeDispatchTable[ui32PrevIndex].pszIOCName,
				 ui32Index, pszIOCName));
#else
		PVR_DPF((PVR_DBG_WARNING,
				 "%s: There is a gap in the dispatch table between indices %u and %u (%s)",
				 __FUNCTION__, (IMG_UINT)ui32PrevIndex, (IMG_UINT)ui32Index, pszIOCName));
#endif
		PVR_DPF((PVR_DBG_ERROR, "NOTE: Enabling DEBUG_BRIDGE_KM_DISPATCH_TABLE may help debug this issue."));
	}

	g_BridgeDispatchTable[ui32Index].pfFunction = pfFunction;
	g_BridgeDispatchTable[ui32Index].in_size = in_size;
	g_BridgeDispatchTable[ui32Index].out_size = out_size;
#if defined(DEBUG_BRIDGE_KM)
	g_BridgeDispatchTable[ui32Index].pszIOCName = pszIOCName;
	g_BridgeDispatchTable[ui32Index].pszFunctionName = pszFunctionName;
	g_BridgeDispatchTable[ui32Index].ui32CallCount = 0;
	g_BridgeDispatchTable[ui32Index].ui32CopyFromUserTotalBytes = 0;
#endif

	ui32PrevIndex = ui32Index;
}

static IMG_INT
PVRSRVInitSrvConnectBW(IMG_UINT32 ui32BridgeID,
					   IMG_VOID *psBridgeIn,
					   PVRSRV_BRIDGE_RETURN *psRetOUT,
					   PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVR_UNREFERENCED_PARAMETER(psBridgeIn);

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_INITSRV_CONNECT);
	PVR_UNREFERENCED_PARAMETER(psBridgeIn);


	if((OSProcHasPrivSrvInit() == IMG_FALSE) || PVRSRVGetInitServerState(PVRSRV_INIT_SERVER_RUNNING) || PVRSRVGetInitServerState(PVRSRV_INIT_SERVER_RAN))
	{
		psRetOUT->eError = PVRSRV_ERROR_SRV_CONNECT_FAILED;
		return 0;
	}

#if defined (__linux__) || defined(__QNXNTO__)
	PVRSRVSetInitServerState(PVRSRV_INIT_SERVER_RUNNING, IMG_TRUE);
#endif
	psPerProc->bInitProcess = IMG_TRUE;

	psRetOUT->eError = PVRSRV_OK;

	return 0;
}


static IMG_INT
PVRSRVInitSrvDisconnectBW(IMG_UINT32 ui32BridgeID,
						  PVRSRV_BRIDGE_IN_INITSRV_DISCONNECT *psInitSrvDisconnectIN,
						  PVRSRV_BRIDGE_RETURN *psRetOUT,
						  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_INITSRV_DISCONNECT);

	if(!psPerProc->bInitProcess)
	{
		psRetOUT->eError = PVRSRV_ERROR_SRV_DISCONNECT_FAILED;
		return 0;
	}

	psPerProc->bInitProcess = IMG_FALSE;

	PVRSRVSetInitServerState(PVRSRV_INIT_SERVER_RUNNING, IMG_FALSE);
	PVRSRVSetInitServerState(PVRSRV_INIT_SERVER_RAN, IMG_TRUE);

	psRetOUT->eError = PVRSRVFinaliseSystem(psInitSrvDisconnectIN->bInitSuccesful);

	PVRSRVSetInitServerState( PVRSRV_INIT_SERVER_SUCCESSFUL ,
				((psRetOUT->eError == PVRSRV_OK) && (psInitSrvDisconnectIN->bInitSuccesful))
				? IMG_TRUE : IMG_FALSE);

	return 0;
}


static IMG_INT
PVRSRVEventObjectWaitBW(IMG_UINT32 ui32BridgeID,
						  PVRSRV_BRIDGE_IN_EVENT_OBJECT_WAIT *psEventObjectWaitIN,
						  PVRSRV_BRIDGE_RETURN *psRetOUT,
						  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_HANDLE hOSEventKM;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_EVENT_OBJECT_WAIT);

	psRetOUT->eError = PVRSRVLookupHandle(psPerProc->psHandleBase,
						   &hOSEventKM,
						   psEventObjectWaitIN->hOSEventKM,
						   PVRSRV_HANDLE_TYPE_EVENT_OBJECT_CONNECT);

	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError = OSEventObjectWaitKM(hOSEventKM);

	return 0;
}


static IMG_INT
PVRSRVEventObjectOpenBW(IMG_UINT32 ui32BridgeID,
						  PVRSRV_BRIDGE_IN_EVENT_OBJECT_OPEN *psEventObjectOpenIN,
						  PVRSRV_BRIDGE_OUT_EVENT_OBJECT_OPEN *psEventObjectOpenOUT,
						  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
#if defined (SUPPORT_SID_INTERFACE)
	PVRSRV_EVENTOBJECT_KM sEventObject;
	IMG_HANDLE hOSEvent;
#endif

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_EVENT_OBJECT_OPEN);

	NEW_HANDLE_BATCH_OR_ERROR(psEventObjectOpenOUT->eError, psPerProc, 1)

	psEventObjectOpenOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
#if defined (SUPPORT_SID_INTERFACE)
						   &sEventObject.hOSEventKM,
#else
						   &psEventObjectOpenIN->sEventObject.hOSEventKM,
#endif
						   psEventObjectOpenIN->sEventObject.hOSEventKM,
						   PVRSRV_HANDLE_TYPE_SHARED_EVENT_OBJECT);

	if(psEventObjectOpenOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

#if defined (SUPPORT_SID_INTERFACE)
	OSMemCopy(&sEventObject.szName,
			  &psEventObjectOpenIN->sEventObject.szName,
			  EVENTOBJNAME_MAXLENGTH);

	psEventObjectOpenOUT->eError = OSEventObjectOpenKM(&sEventObject, &hOSEvent);
#else
	psEventObjectOpenOUT->eError = OSEventObjectOpenKM(&psEventObjectOpenIN->sEventObject, &psEventObjectOpenOUT->hOSEvent);
#endif

	if(psEventObjectOpenOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

#if defined (SUPPORT_SID_INTERFACE)
	PVRSRVAllocHandleNR(psPerProc->psHandleBase,
						&psEventObjectOpenOUT->hOSEvent,
						hOSEvent,
						PVRSRV_HANDLE_TYPE_EVENT_OBJECT_CONNECT,
						PVRSRV_HANDLE_ALLOC_FLAG_MULTI);
#else
	PVRSRVAllocHandleNR(psPerProc->psHandleBase,
					  &psEventObjectOpenOUT->hOSEvent,
					  psEventObjectOpenOUT->hOSEvent,
					  PVRSRV_HANDLE_TYPE_EVENT_OBJECT_CONNECT,
					  PVRSRV_HANDLE_ALLOC_FLAG_MULTI);
#endif

	COMMIT_HANDLE_BATCH_OR_ERROR(psEventObjectOpenOUT->eError, psPerProc)

	return 0;
}


static IMG_INT
PVRSRVEventObjectCloseBW(IMG_UINT32 ui32BridgeID,
						  PVRSRV_BRIDGE_IN_EVENT_OBJECT_CLOSE *psEventObjectCloseIN,
						  PVRSRV_BRIDGE_RETURN *psRetOUT,
						  PVRSRV_PER_PROCESS_DATA *psPerProc)
{
	IMG_HANDLE hOSEventKM;
#if defined (SUPPORT_SID_INTERFACE)
	PVRSRV_EVENTOBJECT_KM sEventObject;
#endif

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_EVENT_OBJECT_CLOSE);

	psRetOUT->eError =
		PVRSRVLookupHandle(psPerProc->psHandleBase,
#if defined (SUPPORT_SID_INTERFACE)
						   &sEventObject.hOSEventKM,
#else
						   &psEventObjectCloseIN->sEventObject.hOSEventKM,
#endif
						   psEventObjectCloseIN->sEventObject.hOSEventKM,
						   PVRSRV_HANDLE_TYPE_SHARED_EVENT_OBJECT);
	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psRetOUT->eError = PVRSRVLookupAndReleaseHandle(psPerProc->psHandleBase,
						   &hOSEventKM,
						   psEventObjectCloseIN->hOSEventKM,
						   PVRSRV_HANDLE_TYPE_EVENT_OBJECT_CONNECT);

	if(psRetOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

#if defined (SUPPORT_SID_INTERFACE)
	if(CopyFromUserWrapper(psPerProc, ui32BridgeID,
							&sEventObject.szName,
							&psEventObjectCloseIN->sEventObject.szName,
							EVENTOBJNAME_MAXLENGTH) != PVRSRV_OK)
	{

		return -EFAULT;
	}

	psRetOUT->eError = OSEventObjectCloseKM(&sEventObject, hOSEventKM);
#else
	psRetOUT->eError = OSEventObjectCloseKM(&psEventObjectCloseIN->sEventObject, hOSEventKM);
#endif

	return 0;
}


typedef struct _MODIFY_SYNC_OP_INFO
{
	IMG_HANDLE  hResItem;
	PVRSRV_KERNEL_SYNC_INFO *psKernelSyncInfo;
	IMG_UINT32  ui32ModifyFlags;
	IMG_UINT32  ui32ReadOpsPendingSnapShot;
	IMG_UINT32  ui32WriteOpsPendingSnapShot;
} MODIFY_SYNC_OP_INFO;


static PVRSRV_ERROR DoQuerySyncOpsSatisfied(PVRSRV_KERNEL_SYNC_INFO *psKernelSyncInfo,
                                            IMG_UINT32 ui32ReadOpsPendingSnapShot,
                                            IMG_UINT32 ui32WriteOpsPendingSnapShot)
{
	IMG_UINT32 ui32WriteOpsPending;
	IMG_UINT32 ui32ReadOpsPending;


	if (!psKernelSyncInfo)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}











	ui32WriteOpsPending = psKernelSyncInfo->psSyncData->ui32WriteOpsPending;
	ui32ReadOpsPending = psKernelSyncInfo->psSyncData->ui32ReadOpsPending;

	if((ui32WriteOpsPending - ui32WriteOpsPendingSnapShot >=
		ui32WriteOpsPending - psKernelSyncInfo->psSyncData->ui32WriteOpsComplete) &&
	   (ui32ReadOpsPending - ui32ReadOpsPendingSnapShot >=
			 ui32ReadOpsPending - psKernelSyncInfo->psSyncData->ui32ReadOpsComplete))
	{
#if defined(PDUMP) && !defined(SUPPORT_VGX)

		PDumpComment("Poll for read ops complete to reach value (pdump: %u, actual snapshot: %u)",
					 psKernelSyncInfo->psSyncData->ui32LastReadOpDumpVal,
					 ui32ReadOpsPendingSnapShot);
		PDumpMemPolKM(psKernelSyncInfo->psSyncDataMemInfoKM,
					  offsetof(PVRSRV_SYNC_DATA, ui32ReadOpsComplete),
					  psKernelSyncInfo->psSyncData->ui32LastReadOpDumpVal,
					  0xFFFFFFFF,
					  PDUMP_POLL_OPERATOR_EQUAL, 
					  0,
					  MAKEUNIQUETAG(psKernelSyncInfo->psSyncDataMemInfoKM));


		PDumpComment("Poll for write ops complete to reach value (pdump: %u, actual snapshot: %u)",
					 psKernelSyncInfo->psSyncData->ui32LastOpDumpVal,
					 ui32WriteOpsPendingSnapShot);
		PDumpMemPolKM(psKernelSyncInfo->psSyncDataMemInfoKM,
					  offsetof(PVRSRV_SYNC_DATA, ui32WriteOpsComplete),
					  psKernelSyncInfo->psSyncData->ui32LastOpDumpVal,
					  0xFFFFFFFF,
					  PDUMP_POLL_OPERATOR_EQUAL, 
					  0,
					  MAKEUNIQUETAG(psKernelSyncInfo->psSyncDataMemInfoKM));


#endif
		return PVRSRV_OK;
	}
	else
	{
		return PVRSRV_ERROR_RETRY;
	}
}


static PVRSRV_ERROR DoModifyCompleteSyncOps(MODIFY_SYNC_OP_INFO *psModSyncOpInfo)
{
	PVRSRV_KERNEL_SYNC_INFO *psKernelSyncInfo;

	psKernelSyncInfo = psModSyncOpInfo->psKernelSyncInfo;

	if (!psKernelSyncInfo)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}


	if((psModSyncOpInfo->ui32WriteOpsPendingSnapShot != psKernelSyncInfo->psSyncData->ui32WriteOpsComplete)
	   || (psModSyncOpInfo->ui32ReadOpsPendingSnapShot != psKernelSyncInfo->psSyncData->ui32ReadOpsComplete))
	{
		return PVRSRV_ERROR_BAD_SYNC_STATE;
	}


	if(psModSyncOpInfo->ui32ModifyFlags & PVRSRV_MODIFYSYNCOPS_FLAGS_WO_INC)
	{
		psKernelSyncInfo->psSyncData->ui32WriteOpsComplete++;
	}


	if(psModSyncOpInfo->ui32ModifyFlags & PVRSRV_MODIFYSYNCOPS_FLAGS_RO_INC)
	{
		psKernelSyncInfo->psSyncData->ui32ReadOpsComplete++;
	}

	return PVRSRV_OK;
}


static PVRSRV_ERROR ModifyCompleteSyncOpsCallBack(IMG_PVOID		pvParam,
                                                    IMG_UINT32  ui32Param,
                                                    IMG_BOOL    bDummy)
{
	MODIFY_SYNC_OP_INFO     *psModSyncOpInfo;

	PVR_UNREFERENCED_PARAMETER(ui32Param);
	PVR_UNREFERENCED_PARAMETER(bDummy);

	if (!pvParam)
	{
		PVR_DPF((PVR_DBG_ERROR, "ModifyCompleteSyncOpsCallBack: invalid parameter"));
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	psModSyncOpInfo = (MODIFY_SYNC_OP_INFO*)pvParam;

	if (psModSyncOpInfo->psKernelSyncInfo)
	{

		LOOP_UNTIL_TIMEOUT(MAX_HW_TIME_US)
		{
			if (DoQuerySyncOpsSatisfied(psModSyncOpInfo->psKernelSyncInfo,
										psModSyncOpInfo->ui32ReadOpsPendingSnapShot,
						psModSyncOpInfo->ui32WriteOpsPendingSnapShot) == PVRSRV_OK)
			{
				goto OpFlushedComplete;
			}
			PVR_DPF((PVR_DBG_WARNING, "ModifyCompleteSyncOpsCallBack: waiting for current Ops to flush"));
			OSSleepms(1);
		} END_LOOP_UNTIL_TIMEOUT();

		PVR_DPF((PVR_DBG_ERROR, "ModifyCompleteSyncOpsCallBack: timeout whilst waiting for current Ops to flush."));
		PVR_DPF((PVR_DBG_ERROR, "  Write ops pending snapshot = %d, write ops complete = %d",
				 psModSyncOpInfo->ui32WriteOpsPendingSnapShot,
				 psModSyncOpInfo->psKernelSyncInfo->psSyncData->ui32WriteOpsComplete));
		PVR_DPF((PVR_DBG_ERROR, "  Read ops pending snapshot = %d, write ops complete = %d",
				 psModSyncOpInfo->ui32ReadOpsPendingSnapShot,
				 psModSyncOpInfo->psKernelSyncInfo->psSyncData->ui32ReadOpsComplete));

		return PVRSRV_ERROR_TIMEOUT;

OpFlushedComplete:

		DoModifyCompleteSyncOps(psModSyncOpInfo);
	}

	OSFreeMem(PVRSRV_OS_PAGEABLE_HEAP,  sizeof(MODIFY_SYNC_OP_INFO), (IMG_VOID *)psModSyncOpInfo, 0);



	PVRSRVScheduleDeviceCallbacks();

	return PVRSRV_OK;
}


static IMG_INT
PVRSRVCreateSyncInfoModObjBW(IMG_UINT32                                         ui32BridgeID,
									 IMG_VOID                                           *psBridgeIn,
									 PVRSRV_BRIDGE_OUT_CREATE_SYNC_INFO_MOD_OBJ  *psCreateSyncInfoModObjOUT,
									 PVRSRV_PER_PROCESS_DATA		   		            *psPerProc)
{
	MODIFY_SYNC_OP_INFO     *psModSyncOpInfo;

	PVR_UNREFERENCED_PARAMETER(psBridgeIn);

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_CREATE_SYNC_INFO_MOD_OBJ);

	NEW_HANDLE_BATCH_OR_ERROR(psCreateSyncInfoModObjOUT->eError, psPerProc, 1)

	ASSIGN_AND_EXIT_ON_ERROR(psCreateSyncInfoModObjOUT->eError,
			  OSAllocMem(PVRSRV_OS_PAGEABLE_HEAP,
			  sizeof(MODIFY_SYNC_OP_INFO),
			  (IMG_VOID **)&psModSyncOpInfo, 0,
			  "ModSyncOpInfo (MODIFY_SYNC_OP_INFO)"));

	psModSyncOpInfo->psKernelSyncInfo = IMG_NULL; 

	psCreateSyncInfoModObjOUT->eError = PVRSRVAllocHandle(psPerProc->psHandleBase,
																  &psCreateSyncInfoModObjOUT->hKernelSyncInfoModObj,
																  psModSyncOpInfo,
																  PVRSRV_HANDLE_TYPE_SYNC_INFO_MOD_OBJ,
																  PVRSRV_HANDLE_ALLOC_FLAG_PRIVATE);

	if (psCreateSyncInfoModObjOUT->eError != PVRSRV_OK)
	{
		return 0;
	}

	psModSyncOpInfo->hResItem = ResManRegisterRes(psPerProc->hResManContext,
												  RESMAN_TYPE_MODIFY_SYNC_OPS,
												  psModSyncOpInfo,
												  0,
												  &ModifyCompleteSyncOpsCallBack);

	COMMIT_HANDLE_BATCH_OR_ERROR(psCreateSyncInfoModObjOUT->eError, psPerProc)

	return 0;
}


static IMG_INT
PVRSRVDestroySyncInfoModObjBW(IMG_UINT32                                          ui32BridgeID,
							  PVRSRV_BRIDGE_IN_DESTROY_SYNC_INFO_MOD_OBJ          *psDestroySyncInfoModObjIN,
							  PVRSRV_BRIDGE_RETURN                                *psDestroySyncInfoModObjOUT,
							  PVRSRV_PER_PROCESS_DATA		   		              *psPerProc)
{
	MODIFY_SYNC_OP_INFO     *psModSyncOpInfo;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_DESTROY_SYNC_INFO_MOD_OBJ);

	psDestroySyncInfoModObjOUT->eError = PVRSRVLookupHandle(psPerProc->psHandleBase,
																	(IMG_VOID**)&psModSyncOpInfo,
																	psDestroySyncInfoModObjIN->hKernelSyncInfoModObj,
																	PVRSRV_HANDLE_TYPE_SYNC_INFO_MOD_OBJ);
	if (psDestroySyncInfoModObjOUT->eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVDestroySyncInfoModObjBW: PVRSRVLookupHandle failed"));
		return 0;
	}

	if(psModSyncOpInfo->psKernelSyncInfo != IMG_NULL)
	{

		psDestroySyncInfoModObjOUT->eError = PVRSRV_ERROR_INVALID_PARAMS;
		return 0;
	}

	psDestroySyncInfoModObjOUT->eError = PVRSRVReleaseHandle(psPerProc->psHandleBase,
																	 psDestroySyncInfoModObjIN->hKernelSyncInfoModObj,
																	 PVRSRV_HANDLE_TYPE_SYNC_INFO_MOD_OBJ);

	if (psDestroySyncInfoModObjOUT->eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVDestroySyncInfoModObjBW: PVRSRVReleaseHandle failed"));
		return 0;
	}

	psDestroySyncInfoModObjOUT->eError = ResManFreeResByPtr(psModSyncOpInfo->hResItem, CLEANUP_WITH_POLL);
	if (psDestroySyncInfoModObjOUT->eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVDestroySyncInfoModObjBW: ResManFreeResByPtr failed"));
		return 0;
	}

	return 0;
}


static IMG_INT
PVRSRVModifyPendingSyncOpsBW(IMG_UINT32									ui32BridgeID,
						      PVRSRV_BRIDGE_IN_MODIFY_PENDING_SYNC_OPS	*psModifySyncOpsIN,
							  PVRSRV_BRIDGE_OUT_MODIFY_PENDING_SYNC_OPS	*psModifySyncOpsOUT,
							  PVRSRV_PER_PROCESS_DATA					*psPerProc)
{
	PVRSRV_KERNEL_SYNC_INFO *psKernelSyncInfo;
	MODIFY_SYNC_OP_INFO     *psModSyncOpInfo;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_MODIFY_PENDING_SYNC_OPS);

	psModifySyncOpsOUT->eError = PVRSRVLookupHandle(psPerProc->psHandleBase,
													(IMG_VOID**)&psModSyncOpInfo,
													psModifySyncOpsIN->hKernelSyncInfoModObj,
													PVRSRV_HANDLE_TYPE_SYNC_INFO_MOD_OBJ);
	if (psModifySyncOpsOUT->eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVModifyPendingSyncOpsBW: PVRSRVLookupHandle failed"));
		return 0;
	}

	psModifySyncOpsOUT->eError = PVRSRVLookupHandle(psPerProc->psHandleBase,
													(IMG_VOID**)&psKernelSyncInfo,
													psModifySyncOpsIN->hKernelSyncInfo,
													PVRSRV_HANDLE_TYPE_SYNC_INFO);
	if (psModifySyncOpsOUT->eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVModifyPendingSyncOpsBW: PVRSRVLookupHandle failed"));
		return 0;
	}

	if(psModSyncOpInfo->psKernelSyncInfo)
	{

		psModifySyncOpsOUT->eError = PVRSRV_ERROR_RETRY;
		PVR_DPF((PVR_DBG_VERBOSE, "PVRSRVModifyPendingSyncOpsBW: SyncInfo Modification object is not empty"));
		return 0;
	}


	psModSyncOpInfo->psKernelSyncInfo = psKernelSyncInfo;
	psModSyncOpInfo->ui32ModifyFlags = psModifySyncOpsIN->ui32ModifyFlags;
	psModSyncOpInfo->ui32ReadOpsPendingSnapShot = psKernelSyncInfo->psSyncData->ui32ReadOpsPending;
	psModSyncOpInfo->ui32WriteOpsPendingSnapShot = psKernelSyncInfo->psSyncData->ui32WriteOpsPending;



	psModifySyncOpsOUT->ui32ReadOpsPending = psKernelSyncInfo->psSyncData->ui32ReadOpsPending;
	psModifySyncOpsOUT->ui32WriteOpsPending = psKernelSyncInfo->psSyncData->ui32WriteOpsPending;

	if(psModifySyncOpsIN->ui32ModifyFlags & PVRSRV_MODIFYSYNCOPS_FLAGS_WO_INC)
	{
		psKernelSyncInfo->psSyncData->ui32WriteOpsPending++;
	}

	if(psModifySyncOpsIN->ui32ModifyFlags & PVRSRV_MODIFYSYNCOPS_FLAGS_RO_INC)
	{
		psKernelSyncInfo->psSyncData->ui32ReadOpsPending++;
	}


	psModifySyncOpsOUT->eError = ResManDissociateRes(psModSyncOpInfo->hResItem,
													 psPerProc->hResManContext);

	if (psModifySyncOpsOUT->eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVModifyPendingSyncOpsBW: PVRSRVLookupHandle failed"));
		return 0;
	}

	return 0;
}


static IMG_INT
PVRSRVModifyCompleteSyncOpsBW(IMG_UINT32							ui32BridgeID,
				      PVRSRV_BRIDGE_IN_MODIFY_COMPLETE_SYNC_OPS		*psModifySyncOpsIN,
					  PVRSRV_BRIDGE_RETURN							*psModifySyncOpsOUT,
					  PVRSRV_PER_PROCESS_DATA						*psPerProc)
{
	MODIFY_SYNC_OP_INFO     *psModSyncOpInfo;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_MODIFY_COMPLETE_SYNC_OPS);

	psModifySyncOpsOUT->eError = PVRSRVLookupHandle(psPerProc->psHandleBase,
													(IMG_VOID**)&psModSyncOpInfo,
													psModifySyncOpsIN->hKernelSyncInfoModObj,
													PVRSRV_HANDLE_TYPE_SYNC_INFO_MOD_OBJ);
	if (psModifySyncOpsOUT->eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVModifyCompleteSyncOpsBW: PVRSRVLookupHandle failed"));
		return 0;
	}

	if(psModSyncOpInfo->psKernelSyncInfo == IMG_NULL)
	{

		psModifySyncOpsOUT->eError = PVRSRV_ERROR_INVALID_PARAMS;
		return 0;
	}

	psModifySyncOpsOUT->eError = DoModifyCompleteSyncOps(psModSyncOpInfo);

	if (psModifySyncOpsOUT->eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVModifyCompleteSyncOpsBW: DoModifyCompleteSyncOps failed"));
		return 0;
	}

	psModSyncOpInfo->psKernelSyncInfo = IMG_NULL;


	PVRSRVScheduleDeviceCallbacks();

	return 0;
}


static IMG_INT
PVRSRVSyncOpsTakeTokenBW(IMG_UINT32									ui32BridgeID,
						 PVRSRV_BRIDGE_IN_SYNC_OPS_TAKE_TOKEN       *psSyncOpsTakeTokenIN,
						 PVRSRV_BRIDGE_OUT_SYNC_OPS_TAKE_TOKEN      *psSyncOpsTakeTokenOUT,
						 PVRSRV_PER_PROCESS_DATA					*psPerProc)
{
	PVRSRV_KERNEL_SYNC_INFO *psKernelSyncInfo;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_SYNC_OPS_TAKE_TOKEN);

	psSyncOpsTakeTokenOUT->eError = PVRSRVLookupHandle(psPerProc->psHandleBase,
													   (IMG_VOID**)&psKernelSyncInfo,
													   psSyncOpsTakeTokenIN->hKernelSyncInfo,
													   PVRSRV_HANDLE_TYPE_SYNC_INFO);
	if (psSyncOpsTakeTokenOUT->eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVSyncOpsTakeTokenBW: PVRSRVLookupHandle failed"));
		return 0;
	}



	psSyncOpsTakeTokenOUT->ui32ReadOpsPending = psKernelSyncInfo->psSyncData->ui32ReadOpsPending;
	psSyncOpsTakeTokenOUT->ui32WriteOpsPending = psKernelSyncInfo->psSyncData->ui32WriteOpsPending;

	return 0;
}


static IMG_INT
PVRSRVSyncOpsFlushToTokenBW(IMG_UINT32                                         ui32BridgeID,
							PVRSRV_BRIDGE_IN_SYNC_OPS_FLUSH_TO_TOKEN		   *psSyncOpsFlushToTokenIN,
							PVRSRV_BRIDGE_RETURN						       *psSyncOpsFlushToTokenOUT,
							PVRSRV_PER_PROCESS_DATA		   		               *psPerProc)
{
	PVRSRV_KERNEL_SYNC_INFO *psKernelSyncInfo;
	IMG_UINT32 ui32ReadOpsPendingSnapshot;
	IMG_UINT32 ui32WriteOpsPendingSnapshot;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_SYNC_OPS_FLUSH_TO_TOKEN);

	psSyncOpsFlushToTokenOUT->eError = PVRSRVLookupHandle(psPerProc->psHandleBase,
														  (IMG_VOID**)&psKernelSyncInfo,
														  psSyncOpsFlushToTokenIN->hKernelSyncInfo,
														  PVRSRV_HANDLE_TYPE_SYNC_INFO);
	if (psSyncOpsFlushToTokenOUT->eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVSyncOpsFlushToTokenBW: PVRSRVLookupHandle failed"));
		return 0;
	}

	ui32ReadOpsPendingSnapshot = psSyncOpsFlushToTokenIN->ui32ReadOpsPendingSnapshot;
	ui32WriteOpsPendingSnapshot = psSyncOpsFlushToTokenIN->ui32WriteOpsPendingSnapshot;

	psSyncOpsFlushToTokenOUT->eError = DoQuerySyncOpsSatisfied(psKernelSyncInfo,
															   ui32ReadOpsPendingSnapshot,
								ui32WriteOpsPendingSnapshot);

	if (psSyncOpsFlushToTokenOUT->eError != PVRSRV_OK && psSyncOpsFlushToTokenOUT->eError != PVRSRV_ERROR_RETRY)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVSyncOpsFlushToTokenBW: DoQuerySyncOpsSatisfied failed"));
		return 0;
	}

	return 0;
}


static IMG_INT
PVRSRVSyncOpsFlushToModObjBW(IMG_UINT32                                         ui32BridgeID,
							 PVRSRV_BRIDGE_IN_SYNC_OPS_FLUSH_TO_MOD_OBJ		    *psSyncOpsFlushToModObjIN,
							 PVRSRV_BRIDGE_RETURN						        *psSyncOpsFlushToModObjOUT,
							 PVRSRV_PER_PROCESS_DATA		   		            *psPerProc)
{
	MODIFY_SYNC_OP_INFO     *psModSyncOpInfo;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_SYNC_OPS_FLUSH_TO_MOD_OBJ);

	psSyncOpsFlushToModObjOUT->eError = PVRSRVLookupHandle(psPerProc->psHandleBase,
														   (IMG_VOID**)&psModSyncOpInfo,
														   psSyncOpsFlushToModObjIN->hKernelSyncInfoModObj,
														   PVRSRV_HANDLE_TYPE_SYNC_INFO_MOD_OBJ);
	if (psSyncOpsFlushToModObjOUT->eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVSyncOpsFlushToModObjBW: PVRSRVLookupHandle failed"));
		return 0;
	}

	if(psModSyncOpInfo->psKernelSyncInfo == IMG_NULL)
	{

		psSyncOpsFlushToModObjOUT->eError = PVRSRV_ERROR_INVALID_PARAMS;
		return 0;
	}

	psSyncOpsFlushToModObjOUT->eError = DoQuerySyncOpsSatisfied(psModSyncOpInfo->psKernelSyncInfo,
																psModSyncOpInfo->ui32ReadOpsPendingSnapShot,
								psModSyncOpInfo->ui32WriteOpsPendingSnapShot);

	if (psSyncOpsFlushToModObjOUT->eError != PVRSRV_OK && psSyncOpsFlushToModObjOUT->eError != PVRSRV_ERROR_RETRY)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVSyncOpsFlushToModObjBW: DoQuerySyncOpsSatisfied failed"));
		return 0;
	}

	return 0;
}


static IMG_INT
PVRSRVSyncOpsFlushToDeltaBW(IMG_UINT32                                         ui32BridgeID,
							PVRSRV_BRIDGE_IN_SYNC_OPS_FLUSH_TO_DELTA		   *psSyncOpsFlushToDeltaIN,
							PVRSRV_BRIDGE_RETURN						       *psSyncOpsFlushToDeltaOUT,
							PVRSRV_PER_PROCESS_DATA		   		               *psPerProc)
{
	PVRSRV_KERNEL_SYNC_INFO     *psSyncInfo;
	IMG_UINT32 ui32DeltaRead;
	IMG_UINT32 ui32DeltaWrite;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_SYNC_OPS_FLUSH_TO_DELTA);

	psSyncOpsFlushToDeltaOUT->eError = PVRSRVLookupHandle(psPerProc->psHandleBase,
														  (IMG_VOID**)&psSyncInfo,
														  psSyncOpsFlushToDeltaIN->hKernelSyncInfo,
														  PVRSRV_HANDLE_TYPE_SYNC_INFO);
	if (psSyncOpsFlushToDeltaOUT->eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVSyncOpsFlushToDeltaBW: PVRSRVLookupHandle failed"));
		return 0;
	}


	ui32DeltaRead = psSyncInfo->psSyncData->ui32ReadOpsPending - psSyncInfo->psSyncData->ui32ReadOpsComplete;
	ui32DeltaWrite = psSyncInfo->psSyncData->ui32WriteOpsPending - psSyncInfo->psSyncData->ui32WriteOpsComplete;

	if (ui32DeltaRead <= psSyncOpsFlushToDeltaIN->ui32Delta && ui32DeltaWrite <= psSyncOpsFlushToDeltaIN->ui32Delta)
	{
#if defined(PDUMP) && !defined(SUPPORT_VGX)

		PDumpComment("Poll for read ops complete to delta (%u)",
					 psSyncOpsFlushToDeltaIN->ui32Delta);
		psSyncOpsFlushToDeltaOUT->eError =
			PDumpMemPolKM(psSyncInfo->psSyncDataMemInfoKM,
						  offsetof(PVRSRV_SYNC_DATA, ui32ReadOpsComplete),
						  psSyncInfo->psSyncData->ui32LastReadOpDumpVal,
						  0xFFFFFFFF,
						  PDUMP_POLL_OPERATOR_GREATEREQUAL,
						  0,
						  MAKEUNIQUETAG(psSyncInfo->psSyncDataMemInfoKM));


		PDumpComment("Poll for write ops complete to delta (%u)",
					 psSyncOpsFlushToDeltaIN->ui32Delta);
		psSyncOpsFlushToDeltaOUT->eError =
			PDumpMemPolKM(psSyncInfo->psSyncDataMemInfoKM,
						  offsetof(PVRSRV_SYNC_DATA, ui32WriteOpsComplete),
						  psSyncInfo->psSyncData->ui32LastOpDumpVal,
						  0xFFFFFFFF,
						  PDUMP_POLL_OPERATOR_GREATEREQUAL,
						  0,
						  MAKEUNIQUETAG(psSyncInfo->psSyncDataMemInfoKM));
#endif

		psSyncOpsFlushToDeltaOUT->eError = PVRSRV_OK;
	}
	else
	{
		psSyncOpsFlushToDeltaOUT->eError = PVRSRV_ERROR_RETRY;
	}

	return 0;
}


static PVRSRV_ERROR
FreeSyncInfoCallback(IMG_PVOID pvParam,
                     IMG_UINT32 ui32Param,
                     IMG_BOOL	bDummy)
{
	PVRSRV_KERNEL_SYNC_INFO *psSyncInfo;
	PVRSRV_ERROR eError;

	PVR_UNREFERENCED_PARAMETER(ui32Param);
	PVR_UNREFERENCED_PARAMETER(bDummy);

	psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)pvParam;

	eError = PVRSRVFreeSyncInfoKM(psSyncInfo);
	if (eError != PVRSRV_OK)
	{
		return eError;
	}

	return PVRSRV_OK;
}


static IMG_INT
PVRSRVAllocSyncInfoBW(IMG_UINT32                                         ui32BridgeID,
					  PVRSRV_BRIDGE_IN_ALLOC_SYNC_INFO                  *psAllocSyncInfoIN,
					  PVRSRV_BRIDGE_OUT_ALLOC_SYNC_INFO                 *psAllocSyncInfoOUT,
					  PVRSRV_PER_PROCESS_DATA		   		            *psPerProc)
{
	PVRSRV_KERNEL_SYNC_INFO     *psSyncInfo;
	PVRSRV_ERROR eError;
	PVRSRV_DEVICE_NODE *psDeviceNode;
	IMG_HANDLE hDevMemContext;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_ALLOC_SYNC_INFO);

	NEW_HANDLE_BATCH_OR_ERROR(psAllocSyncInfoOUT->eError, psPerProc, 1)

	eError = PVRSRVLookupHandle(psPerProc->psHandleBase,
								(IMG_HANDLE *)&psDeviceNode,
								psAllocSyncInfoIN->hDevCookie,
								PVRSRV_HANDLE_TYPE_DEV_NODE);
	if(eError != PVRSRV_OK)
	{
		goto allocsyncinfo_errorexit;
	}

	hDevMemContext = psDeviceNode->sDevMemoryInfo.pBMKernelContext;

	eError = PVRSRVAllocSyncInfoKM(psDeviceNode,
								   hDevMemContext,
								   &psSyncInfo);

	if (eError != PVRSRV_OK)
	{
		goto allocsyncinfo_errorexit;
	}

	eError = PVRSRVAllocHandle(psPerProc->psHandleBase,
							   &psAllocSyncInfoOUT->hKernelSyncInfo,
							   psSyncInfo,
							   PVRSRV_HANDLE_TYPE_SYNC_INFO,
							   PVRSRV_HANDLE_ALLOC_FLAG_PRIVATE);

	if(eError != PVRSRV_OK)
	{
		goto allocsyncinfo_errorexit_freesyncinfo;
	}

	psSyncInfo->hResItem = ResManRegisterRes(psPerProc->hResManContext,
											   RESMAN_TYPE_SYNC_INFO,
											   psSyncInfo,
											   0,
											   FreeSyncInfoCallback);


	goto allocsyncinfo_commit;


allocsyncinfo_errorexit_freesyncinfo:
	PVRSRVFreeSyncInfoKM(psSyncInfo);

allocsyncinfo_errorexit:


allocsyncinfo_commit:
	psAllocSyncInfoOUT->eError = eError;
	COMMIT_HANDLE_BATCH_OR_ERROR(eError, psPerProc);

	return 0;
}


static IMG_INT
PVRSRVFreeSyncInfoBW(IMG_UINT32                                          ui32BridgeID,
					 PVRSRV_BRIDGE_IN_FREE_SYNC_INFO                     *psFreeSyncInfoIN,
					 PVRSRV_BRIDGE_RETURN                                *psFreeSyncInfoOUT,
					 PVRSRV_PER_PROCESS_DATA		   		             *psPerProc)
{
	PVRSRV_KERNEL_SYNC_INFO *psSyncInfo;
	PVRSRV_ERROR eError;

	PVRSRV_BRIDGE_ASSERT_CMD(ui32BridgeID, PVRSRV_BRIDGE_FREE_SYNC_INFO);

	eError = PVRSRVLookupHandle(psPerProc->psHandleBase,
								(IMG_VOID**)&psSyncInfo,
								psFreeSyncInfoIN->hKernelSyncInfo,
								PVRSRV_HANDLE_TYPE_SYNC_INFO);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVFreeSyncInfoBW: PVRSRVLookupHandle failed"));
		psFreeSyncInfoOUT->eError = eError;
		return 0;
	}

	eError = PVRSRVReleaseHandle(psPerProc->psHandleBase,
								 psFreeSyncInfoIN->hKernelSyncInfo,
								 PVRSRV_HANDLE_TYPE_SYNC_INFO);

	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVFreeSyncInfoBW: PVRSRVReleaseHandle failed"));
		psFreeSyncInfoOUT->eError = eError;
		return 0;
	}

	eError = ResManFreeResByPtr(psSyncInfo->hResItem, CLEANUP_WITH_POLL);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVFreeSyncInfoBW: ResManFreeResByPtr failed"));
		psFreeSyncInfoOUT->eError = eError;
		return 0;
	}

	return 0;
}


PVRSRV_ERROR
CommonBridgeInit(IMG_VOID)
{
	IMG_UINT32 i;

	PVR_IO_NSTD(ENUM_DEVICES, PVRSRVEnumerateDevicesBW, 0, sizeof(PVRSRV_BRIDGE_OUT_ENUMDEVICE));
	PVR_IO_RW(ACQUIRE_DEVICEINFO, PVRSRVAcquireDeviceDataBW);
	PVR_IO_INV(RELEASE_DEVICEINFO);
	PVR_IO_RW(CREATE_DEVMEMCONTEXT, PVRSRVCreateDeviceMemContextBW);
	PVR_IO_W(DESTROY_DEVMEMCONTEXT, PVRSRVDestroyDeviceMemContextBW);
	PVR_IO_RW(GET_DEVMEM_HEAPINFO, PVRSRVGetDeviceMemHeapInfoBW);
	PVR_IO_NSTD(ALLOC_DEVICEMEM, PVRSRVAllocDeviceMemBW,
			sizeof(PVRSRV_BRIDGE_IN_ALLOCDEVICEMEM),
			sizeof(PVRSRV_BRIDGE_OUT_ALLOCDEVICEMEM));

	PVR_IO_NSTD(FREE_DEVICEMEM, PVRSRVFreeDeviceMemBW,
			sizeof(PVRSRV_BRIDGE_IN_FREEDEVICEMEM),
			sizeof(PVRSRV_BRIDGE_RETURN));

	PVR_IO_NSTD(GETFREE_DEVICEMEM, PVRSRVGetFreeDeviceMemBW,
			sizeof(PVRSRV_BRIDGE_IN_GETFREEDEVICEMEM),
			sizeof(PVRSRV_BRIDGE_OUT_GETFREEDEVICEMEM));
	PVR_IO_INV(CREATE_COMMANDQUEUE);
	PVR_IO_INV(DESTROY_COMMANDQUEUE);
	PVR_IO_RW(MHANDLE_TO_MMAP_DATA, PVRMMapOSMemHandleToMMapDataBW);
	PVR_IO_RW(CONNECT_SERVICES, PVRSRVConnectBW);
	PVR_IO_NSTD(DISCONNECT_SERVICES, PVRSRVDisconnectBW,
			0, sizeof(PVRSRV_BRIDGE_RETURN));
	PVR_IO_INV(WRAP_DEVICE_MEM);
	PVR_IO_INV(GET_DEVICEMEMINFO);
	PVR_IO_INV(RESERVE_DEV_VIRTMEM);
	PVR_IO_INV(FREE_DEV_VIRTMEM);
	PVR_IO_INV(MAP_EXT_MEMORY);
	PVR_IO_INV(UNMAP_EXT_MEMORY);
	PVR_IO_RW(MAP_DEV_MEMORY, PVRSRVMapDeviceMemoryBW);
	PVR_IO_W(UNMAP_DEV_MEMORY, PVRSRVUnmapDeviceMemoryBW);
	PVR_IO_RW(MAP_DEVICECLASS_MEMORY, PVRSRVMapDeviceClassMemoryBW);
	PVR_IO_W(UNMAP_DEVICECLASS_MEMORY, PVRSRVUnmapDeviceClassMemoryBW);
	PVR_IO_INV(MAP_MEM_INFO_TO_USER);
	PVR_IO_INV(UNMAP_MEM_INFO_FROM_USER);
	PVR_IO_NSTD(EXPORT_DEVICEMEM, PVRSRVExportDeviceMemBW,
			sizeof(PVRSRV_BRIDGE_IN_EXPORTDEVICEMEM),
			sizeof(PVRSRV_BRIDGE_OUT_EXPORTDEVICEMEM));
	PVR_IO_RW(RELEASE_MMAP_DATA, PVRMMapReleaseMMapDataBW);

	PVR_IO_W(CHG_DEV_MEM_ATTRIBS, PVRSRVChangeDeviceMemoryAttributesBW);
	PVR_IO_NSTD(MAP_DEV_MEMORY_2, PVRSRVMapDeviceMemoryBW,
			sizeof(PVRSRV_BRIDGE_IN_MAP_DEV_MEMORY),
			sizeof(PVRSRV_BRIDGE_OUT_MAP_DEV_MEMORY));
	PVR_IO_NSTD(EXPORT_DEVICEMEM_2, PVRSRVExportDeviceMemBW,
			sizeof(PVRSRV_BRIDGE_IN_EXPORTDEVICEMEM),
			sizeof(PVRSRV_BRIDGE_OUT_EXPORTDEVICEMEM));

	PVR_IO_INV(PROCESS_SIMISR_EVENT);
	PVR_IO_INV(REGISTER_SIM_PROCESS);
	PVR_IO_INV(UNREGISTER_SIM_PROCESS);
	PVR_IO_INV(MAPPHYSTOUSERSPACE);
	PVR_IO_INV(UNMAPPHYSTOUSERSPACE);
	PVR_IO_INV(GETPHYSTOUSERSPACEMAP);
	PVR_IO_INV(GET_FB_STATS);
	PVR_IO_RW(GET_MISC_INFO, PVRSRVGetMiscInfoBW);
	PVR_IO_INV(RELEASE_MISC_INFO);

#if defined (SUPPORT_OVERLAY_ROTATE_BLIT)
	PVR_IO_INV(INIT_3D_OVL_BLT_RES);
	PVR_IO_INV(DEINIT_3D_OVL_BLT_RES);
#endif



#if defined(PDUMP)
	PVR_IO_INV(PDUMP_INIT);
	PVR_IO_W(PDUMP_MEMPOL, PDumpMemPolBW);
	PVR_IO_W(PDUMP_DUMPMEM, PDumpMemBW);
	PVR_IO_NSTD(PDUMP_REG, PDumpRegWithFlagsBW,
			sizeof(PVRSRV_BRIDGE_IN_PDUMP_DUMPREG),
			sizeof(PVRSRV_BRIDGE_RETURN));
	PVR_IO_W(PDUMP_REGPOL, PDumpRegPolBW);
	PVR_IO_W(PDUMP_COMMENT, PDumpCommentBW);
	PVR_IO_W(PDUMP_SETFRAME, PDumpSetFrameBW);
	PVR_IO_R(PDUMP_ISCAPTURING, PDumpIsCaptureFrameBW);
	PVR_IO_NSTD(PDUMP_DUMPBITMAP, PDumpBitmapBW,
			sizeof(PVRSRV_BRIDGE_IN_PDUMP_BITMAP),
			sizeof(PVRSRV_BRIDGE_RETURN));
	PVR_IO_NSTD(PDUMP_DUMPREADREG, PDumpReadRegBW,
			sizeof(PVRSRV_BRIDGE_IN_PDUMP_READREG),
			sizeof(PVRSRV_BRIDGE_RETURN));
	PVR_IO_W(PDUMP_SYNCPOL, PDumpSyncPolBW);
	PVR_IO_W(PDUMP_DUMPSYNC, PDumpSyncDumpBW);
	PVR_IO_W(PDUMP_MEMPAGES, PDumpMemPagesBW);
	PVR_IO_W(PDUMP_DRIVERINFO, PDumpDriverInfoBW);
	PVR_IO_W(PDUMP_DUMPPDDEVPADDR, PDumpPDDevPAddrBW);
	PVR_IO_W(PDUMP_CYCLE_COUNT_REG_READ, PDumpCycleCountRegReadBW);
	PVR_IO_NSTD(PDUMP_STARTINITPHASE, PDumpStartInitPhaseBW,
			0, sizeof(PVRSRV_BRIDGE_RETURN));
	PVR_IO_NSTD(PDUMP_STOPINITPHASE, PDumpStopInitPhaseBW,
			0, sizeof(PVRSRV_BRIDGE_RETURN));
#endif 

	PVR_IO_INV(GET_OEMJTABLE);
	PVR_IO_NSTD(ENUM_CLASS, PVRSRVEnumerateDCBW,
			sizeof(PVRSRV_BRIDGE_IN_ENUMCLASS),
			sizeof(PVRSRV_BRIDGE_OUT_ENUMCLASS));
	PVR_IO_RW(OPEN_DISPCLASS_DEVICE, PVRSRVOpenDCDeviceBW);
	PVR_IO_W(CLOSE_DISPCLASS_DEVICE, PVRSRVCloseDCDeviceBW);
	PVR_IO_RW(ENUM_DISPCLASS_FORMATS, PVRSRVEnumDCFormatsBW);
	PVR_IO_RW(ENUM_DISPCLASS_DIMS, PVRSRVEnumDCDimsBW);
	PVR_IO_RW(GET_DISPCLASS_SYSBUFFER, PVRSRVGetDCSystemBufferBW);
	PVR_IO_RW(GET_DISPCLASS_INFO, PVRSRVGetDCInfoBW);
	PVR_IO_RW(CREATE_DISPCLASS_SWAPCHAIN, PVRSRVCreateDCSwapChainBW);
	PVR_IO_W(DESTROY_DISPCLASS_SWAPCHAIN, PVRSRVDestroyDCSwapChainBW);
	PVR_IO_NSTD(SET_DISPCLASS_DSTRECT, PVRSRVSetDCDstRectBW,
			sizeof(PVRSRV_BRIDGE_IN_SET_DISPCLASS_RECT),
			sizeof(PVRSRV_BRIDGE_RETURN));
	PVR_IO_NSTD(SET_DISPCLASS_SRCRECT, PVRSRVSetDCSrcRectBW,
			sizeof(PVRSRV_BRIDGE_IN_SET_DISPCLASS_RECT),
			sizeof(PVRSRV_BRIDGE_RETURN));
	PVR_IO_NSTD(SET_DISPCLASS_DSTCOLOURKEY, PVRSRVSetDCDstColourKeyBW,
			sizeof(PVRSRV_BRIDGE_IN_SET_DISPCLASS_COLOURKEY),
			sizeof(PVRSRV_BRIDGE_RETURN));
	PVR_IO_NSTD(SET_DISPCLASS_SRCCOLOURKEY, PVRSRVSetDCSrcColourKeyBW,
			sizeof(PVRSRV_BRIDGE_IN_SET_DISPCLASS_COLOURKEY),
			sizeof(PVRSRV_BRIDGE_RETURN));
	PVR_IO_RW(GET_DISPCLASS_BUFFERS, PVRSRVGetDCBuffersBW);
	PVR_IO_W(SWAP_DISPCLASS_TO_BUFFER, PVRSRVSwapToDCBufferBW);
	PVR_IO_W(SWAP_DISPCLASS_TO_SYSTEM, PVRSRVSwapToDCSystemBW);
	PVR_IO_RW(OPEN_BUFFERCLASS_DEVICE, PVRSRVOpenBCDeviceBW);
	PVR_IO_W(CLOSE_BUFFERCLASS_DEVICE, PVRSRVCloseBCDeviceBW);
	PVR_IO_RW(GET_BUFFERCLASS_INFO, PVRSRVGetBCInfoBW);
	PVR_IO_RW(GET_BUFFERCLASS_BUFFER, PVRSRVGetBCBufferBW);
	PVR_IO_RW(WRAP_EXT_MEMORY, PVRSRVWrapExtMemoryBW);
	PVR_IO_W(UNWRAP_EXT_MEMORY, PVRSRVUnwrapExtMemoryBW);
	PVR_IO_RW(ALLOC_SHARED_SYS_MEM, PVRSRVAllocSharedSysMemoryBW);
	PVR_IO_RW(FREE_SHARED_SYS_MEM, PVRSRVFreeSharedSysMemoryBW);
	PVR_IO_RW(MAP_MEMINFO_MEM, PVRSRVMapMemInfoMemBW);

	PVR_IO_NSTD(INITSRV_CONNECT, PVRSRVInitSrvConnectBW,
			0, sizeof(PVRSRV_BRIDGE_RETURN));
	PVR_IO_W(INITSRV_DISCONNECT, PVRSRVInitSrvDisconnectBW);
	PVR_IO_W(EVENT_OBJECT_WAIT, PVRSRVEventObjectWaitBW);
	PVR_IO_RW(EVENT_OBJECT_OPEN, PVRSRVEventObjectOpenBW);
	PVR_IO_W(EVENT_OBJECT_CLOSE, PVRSRVEventObjectCloseBW);
	PVR_IO_NSTD(CREATE_SYNC_INFO_MOD_OBJ, PVRSRVCreateSyncInfoModObjBW,
			0, sizeof(PVRSRV_BRIDGE_OUT_CREATE_SYNC_INFO_MOD_OBJ));
	PVR_IO_W(DESTROY_SYNC_INFO_MOD_OBJ, PVRSRVDestroySyncInfoModObjBW);
	PVR_IO_RW(MODIFY_PENDING_SYNC_OPS, PVRSRVModifyPendingSyncOpsBW);
	PVR_IO_W(MODIFY_COMPLETE_SYNC_OPS, PVRSRVModifyCompleteSyncOpsBW);
	PVR_IO_RW(SYNC_OPS_TAKE_TOKEN, PVRSRVSyncOpsTakeTokenBW);
	PVR_IO_W(SYNC_OPS_FLUSH_TO_TOKEN, PVRSRVSyncOpsFlushToTokenBW);
	PVR_IO_W(SYNC_OPS_FLUSH_TO_MOD_OBJ, PVRSRVSyncOpsFlushToModObjBW);
	PVR_IO_W(SYNC_OPS_FLUSH_TO_DELTA, PVRSRVSyncOpsFlushToDeltaBW);
	PVR_IO_RW(ALLOC_SYNC_INFO, PVRSRVAllocSyncInfoBW);
	PVR_IO_W(FREE_SYNC_INFO, PVRSRVFreeSyncInfoBW);

#if defined (SUPPORT_SGX)
	SetSGXDispatchTableEntry();
#endif
#if defined (SUPPORT_VGX)
	SetVGXDispatchTableEntry();
#endif
#if defined (SUPPORT_MSVDX)
	SetMSVDXDispatchTableEntry();
#endif




	for(i=0;i<BRIDGE_DISPATCH_TABLE_ENTRY_COUNT;i++)
	{
		if(!g_BridgeDispatchTable[i].pfFunction)
		{
			g_BridgeDispatchTable[i].pfFunction = &DummyBW;
#if defined(DEBUG_BRIDGE_KM)
			g_BridgeDispatchTable[i].pszIOCName = "_PVRSRV_BRIDGE_DUMMY";
			g_BridgeDispatchTable[i].pszFunctionName = "DummyBW";
			g_BridgeDispatchTable[i].ui32CallCount = 0;
			g_BridgeDispatchTable[i].ui32CopyFromUserTotalBytes = 0;
			g_BridgeDispatchTable[i].ui32CopyToUserTotalBytes = 0;
#endif
		}
	}

	return PVRSRV_OK;
}

#if defined(CONFIG_COMPAT)
/*
 * 32-bit userspace (the i686 PowerVR DDK blobs) on a 64-bit kernel. The bridge
 * structs match the non-SID layout EXCEPT that IMG_HANDLE is 4 bytes in userspace
 * and 8 in the kernel (and the 8-byte alignment inserts padding the i686 struct
 * does not have). PVR secure handles are small table indices (< 256), so they
 * survive a 4<->8 round trip losslessly. The kernel struct is the authority; a
 * compat caller sends/receives a packed 4-byte-handle variant, so we translate
 * per bridge: expand the IN into the kernel layout after it is copied in, and
 * compact the OUT into the i686 layout before it is copied out. Bridges whose
 * structs carry no handle/pointer need no entry (their sizes already match).
 *
 * Add bridges here as the GPU userspace exercises them: sgxinit first
 * (CONNECT_SERVICES + a few SGX info calls), then gles2tri (mem/context/kick).
 * See [[c4-ea-sgx-wpe-64bit]].
 */
/* Bridges whose OUT is the generic PVRSRV_BRIDGE_RETURN {PVRSRV_ERROR eError;
 * IMG_VOID *pvData} — 16 B on x86_64 (4 + 4 pad + 8), 8 B on i686 (4 + 4). This
 * is the default OUT of every PVR_IO_W bridge, so one rule covers a whole class.
 * Their INs (if any) carry no handle, so only the OUT needs compacting. */
static IMG_BOOL PVRCompatBridgeGenericReturn(IMG_UINT32 ui32BridgeID)
{
	return (IMG_BOOL)(
		   ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_INITSRV_CONNECT)
		|| ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_INITSRV_DISCONNECT)
		|| ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_DISCONNECT_SERVICES));
}

/* Pack one kernel PVRSRV_CLIENT_MEM_INFO (88 B) into the i686 layout (48 B) at
 * pbyDst, returning the bytes written. Reads every field into locals before writing
 * so an in-place (overlapping src/dst) repack is safe. The pointer fields are
 * truncated to 32 bits: Linux sets pvLinAddr=0, and userspace treats pvLinAddrKM /
 * psClientSyncInfo / psNext as opaque (it calls back into the kernel with the
 * hKernelMemInfo handle, never these raw addresses). Handles are small indices
 * (8->4 lossless). IMG_CPU_PHYADDR.uiAddr is IMG_UINTPTR_T but a 32-bit phys space. */
static IMG_UINT32 PVRCompatPackClientMemInfo(IMG_PBYTE pbyDst,
					     const PVRSRV_CLIENT_MEM_INFO *psK)
{
	IMG_UINT32 ui32LinAddr   = (IMG_UINT32)(unsigned long)psK->pvLinAddr;
	IMG_UINT32 ui32LinAddrKM = (IMG_UINT32)(unsigned long)psK->pvLinAddrKM;
	IMG_UINT32 ui32DevVAddr  = psK->sDevVAddr.uiAddr;
	IMG_UINT32 ui32CpuPAddr  = (IMG_UINT32)psK->sCpuPAddr.uiAddr;
	IMG_UINT32 ui32Flags     = psK->ui32Flags;
	IMG_UINT32 ui32CFlags    = psK->ui32ClientFlags;
	IMG_UINT32 ui32AllocSize = (IMG_UINT32)psK->uAllocSize;
	IMG_UINT32 ui32SyncInfo  = (IMG_UINT32)(unsigned long)psK->psClientSyncInfo;
	IMG_UINT32 ui32MapInfo   = (IMG_UINT32)(unsigned long)psK->hMappingInfo;
	IMG_UINT32 ui32KMemInfo  = (IMG_UINT32)(unsigned long)psK->hKernelMemInfo;
	IMG_UINT32 ui32ResItem   = (IMG_UINT32)(unsigned long)psK->hResItem;
	IMG_UINT32 ui32Next      = (IMG_UINT32)(unsigned long)psK->psNext;
	IMG_UINT32 *p = (IMG_UINT32 *)pbyDst;

	p[0] = ui32LinAddr;  p[1] = ui32LinAddrKM; p[2]  = ui32DevVAddr; p[3]  = ui32CpuPAddr;
	p[4] = ui32Flags;    p[5] = ui32CFlags;    p[6]  = ui32AllocSize;p[7]  = ui32SyncInfo;
	p[8] = ui32MapInfo;  p[9] = ui32KMemInfo;  p[10] = ui32ResItem;  p[11] = ui32Next;
	return 12 * sizeof(IMG_UINT32);
}

/* Pack one kernel PVRSRV_CLIENT_SYNC_INFO (32 B) into the i686 layout (20 B). Same
 * read-all-then-write discipline. psSyncData is the kernel sync-counter pointer;
 * truncated here (not read back by the kernel). NOTE for gles2tri: userspace polls
 * sync counters through its own mmap, but if it ever dereferences psSyncData this
 * must become a real per-process mapping, not a truncation. */
static IMG_UINT32 PVRCompatPackClientSyncInfo(IMG_PBYTE pbyDst,
					      const PVRSRV_CLIENT_SYNC_INFO *psK)
{
	IMG_UINT32 ui32SyncData = (IMG_UINT32)(unsigned long)psK->psSyncData;
	IMG_UINT32 ui32WVAddr   = psK->sWriteOpsCompleteDevVAddr.uiAddr;
	IMG_UINT32 ui32RVAddr   = psK->sReadOpsCompleteDevVAddr.uiAddr;
	IMG_UINT32 ui32MapInfo  = (IMG_UINT32)(unsigned long)psK->hMappingInfo;
	IMG_UINT32 ui32KSync    = (IMG_UINT32)(unsigned long)psK->hKernelSyncInfo;
	IMG_UINT32 *p = (IMG_UINT32 *)pbyDst;

	p[0] = ui32SyncData; p[1] = ui32WVAddr; p[2] = ui32RVAddr;
	p[3] = ui32MapInfo;  p[4] = ui32KSync;
	return 5 * sizeof(IMG_UINT32);
}

/* Pack one kernel PVRSRV_HEAP_INFO (32 B: u32 + 4 pad + 8-byte handle + 3 u32) into
 * the i686 layout (24 B: 6 u32), returning bytes written. Recurs in every heap-array
 * OUT (SGXINFO_FOR_SRVINIT, CREATE_DEVMEMCONTEXT, GET_DEVMEM_HEAPINFO). Reads all fields
 * before writing so an in-place overlapping repack is safe. hDevMemHeap is an index. */
static IMG_UINT32 PVRCompatPackHeapInfo(IMG_PBYTE pbyDst, const PVRSRV_HEAP_INFO *psH)
{
	IMG_UINT32 ui32HeapID = psH->ui32HeapID;
	IMG_UINT32 ui32Heap   = (IMG_UINT32)(unsigned long)psH->hDevMemHeap;
	IMG_UINT32 ui32VAddr  = psH->sDevVAddrBase.uiAddr;
	IMG_UINT32 ui32Size   = psH->ui32HeapByteSize;
	IMG_UINT32 ui32Attr   = psH->ui32Attribs;
	IMG_UINT32 ui32XTile  = psH->ui32XTileStride;
	IMG_UINT32 *p = (IMG_UINT32 *)pbyDst;

	p[0] = ui32HeapID; p[1] = ui32Heap;  p[2] = ui32VAddr;
	p[3] = ui32Size;   p[4] = ui32Attr;  p[5] = ui32XTile;
	return 6 * sizeof(IMG_UINT32);
}

static IMG_BOOL PVRCompatBridge(IMG_UINT32 ui32BridgeID,
								IMG_UINT32 *pui32CompatIn,
								IMG_UINT32 *pui32CompatOut)
{
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_CONNECT_SERVICES))
	{
		/* IN {ui32BridgeFlags, ui32Flags} — no handle, same size.
		 * OUT {PVRSRV_ERROR eError; IMG_HANDLE hKernelServices} — i686 packs to
		 * {u32 eError, u32 handle} = 8, vs the kernel's 16 (pad + 8-byte handle). */
		*pui32CompatIn  = sizeof(PVRSRV_BRIDGE_IN_CONNECT_SERVICES);
		*pui32CompatOut = 2 * sizeof(IMG_UINT32);
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_ACQUIRE_DEVICEINFO))
	{
		/* IN {flags, devIndex, deviceType} — all u32, no handle (same size).
		 * OUT {PVRSRV_ERROR eError; IMG_HANDLE hDevCookie} -> i686 {u32, u32} = 8. */
		*pui32CompatIn  = sizeof(PVRSRV_BRIDGE_IN_ACQUIRE_DEVICEINFO);
		*pui32CompatOut = 2 * sizeof(IMG_UINT32);
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_ENUM_DEVICES))
	{
		/* IN has no handle (leave *pui32CompatIn = dte->in_size). OUT is
		 * {eError, ui32NumDevices, PVRSRV_DEVICE_IDENTIFIER[16]}; each identifier
		 * is i686 20 B (3 u32 + 2 four-byte ptrs) vs the kernel's 32 B (two 8-byte
		 * PDUMP-name ptrs + 8-align pad). i686 OUT = 8 + 16*20 = 328. */
		*pui32CompatOut = 2 * sizeof(IMG_UINT32)
			+ PVRSRV_MAX_DEVICES * (5 * sizeof(IMG_UINT32));
		return IMG_TRUE;
	}
	if (PVRCompatBridgeGenericReturn(ui32BridgeID))
	{
		/* IN (if present) is handle-free -> leave *pui32CompatIn = dte in_size.
		 * OUT PVRSRV_BRIDGE_RETURN 16 -> i686 {u32 eError, u32 pvData} = 8. */
		*pui32CompatOut = 2 * sizeof(IMG_UINT32);
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SGXINFO_FOR_SRVINIT))
	{
		/* SrvInit's big "get SGX init data" call.
		 * IN  {u32 ui32BridgeFlags; IMG_HANDLE hDevCookie} = 16 kernel / 8 i686.
		 * OUT {PVRSRV_ERROR eError; SGX_BRIDGE_INFO_FOR_SRVINIT sInitInfo}, where
		 * sInitInfo = {IMG_DEV_PHYADDR sPDDevPAddr; PVRSRV_HEAP_INFO asHeapInfo[32]}.
		 * i686 PVRSRV_HEAP_INFO = 6*u32 = 24 (kernel 32: u32 + 4 pad + 8-byte handle +
		 * 3*u32), sPDDevPAddr i686 4 / kernel 8. i686 OUT = 4 + 4 + 32*24 = 776. */
		*pui32CompatIn  = 2 * sizeof(IMG_UINT32);
		*pui32CompatOut = 2 * sizeof(IMG_UINT32)
			+ PVRSRV_MAX_CLIENT_HEAPS * (6 * sizeof(IMG_UINT32));
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_ALLOC_DEVICEMEM))
	{
		/* IN {flags; HANDLE hDevCookie; HANDLE hDevMemHeap; u32 attribs; SIZE_T size;
		 * SIZE_T align} = 24 i686 / 40 kernel (two handles + align pad). OUT
		 * {eError; KMEM_INFO *psKernelMemInfo; CLIENT_MEM_INFO; CLIENT_SYNC_INFO} =
		 * 4 + 4 + 48 + 20 = 76 i686 (kernel 136). */
		*pui32CompatIn  = 6 * sizeof(IMG_UINT32);
		*pui32CompatOut = 2 * sizeof(IMG_UINT32) + 48 + 20;
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_MHANDLE_TO_MMAP_DATA)
	    || ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_RELEASE_MMAP_DATA))
	{
		/* Both IN {flags; IMG_HANDLE hMHandle} = 8 i686 / 16 kernel. Their OUTs are all
		 * u32 (MMAP_DATA 5x=20, RELEASE 4x=16) so they match both arches -> leave OUT. */
		*pui32CompatIn = 2 * sizeof(IMG_UINT32);
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_FREE_DEVICEMEM))
	{
		/* IN {flags; HANDLE hDevCookie; KMEM_INFO *psKernelMemInfo; CLIENT_MEM_INFO
		 * sClientMemInfo} = 60 i686 / 112 kernel. OUT generic RETURN -> 8. The handler
		 * reads only hDevCookie + psKernelMemInfo (as handles); sClientMemInfo unused. */
		*pui32CompatIn  = 3 * sizeof(IMG_UINT32) + 48;
		*pui32CompatOut = 2 * sizeof(IMG_UINT32);
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SGX_DEVINITPART2))
	{
		/* The microkernel upload. IN i686 2068 / kernel 2192 (27 handles 4->8 plus
		 * 8-align pads, two opaque u32 blocks) — reproduced + BUILD_BUG_ON-checked by
		 * PVRCompatExpandDevInitPart2. OUT: the handler writes PVRSRV_BRIDGE_OUT_
		 * SGXDEVINITPART2 {eError@0; u32 ui32KMBuildOptions@4} = 8, whose first 8 bytes
		 * are already the i686 layout, BUT the dispatch over-declares out_size as
		 * sizeof(PVRSRV_BRIDGE_RETURN)=16. So force compat OUT=8 to pass the size check;
		 * no repack needed (CopyToUser sends the already-correct first 8 bytes). */
		*pui32CompatIn  = 2068;
		*pui32CompatOut = 2 * sizeof(IMG_UINT32);
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SGX_GETMISCINFO))
	{
		/* IN {flags; HANDLE hDevCookie; SGX_MISC_INFO *psMiscInfo} = 12 i686 / 24 kernel.
		 * psMiscInfo is an inner USER pointer, but SGX_MISC_INFO is arch-identical here
		 * (no EDM_MEMORY_DEBUG handle; all-u32 union) so it needs no translation, just
		 * zero-extension. OUT generic RETURN -> 8. */
		*pui32CompatIn  = 3 * sizeof(IMG_UINT32);
		*pui32CompatOut = 2 * sizeof(IMG_UINT32);
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_CREATE_DEVMEMCONTEXT))
	{
		/* IN {flags; HANDLE hDevCookie} = 8 i686 / 16 kernel. OUT {eError; HANDLE
		 * hDevMemContext; u32 ui32ClientHeapCount; PVRSRV_HEAP_INFO[32]} = i686
		 * 4+4+4+32*24 = 780 (kernel 1048). */
		*pui32CompatIn  = 2 * sizeof(IMG_UINT32);
		*pui32CompatOut = 3 * sizeof(IMG_UINT32)
			+ PVRSRV_MAX_CLIENT_HEAPS * (6 * sizeof(IMG_UINT32));
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_GET_DEVMEM_HEAPINFO))
	{
		/* IN {flags; HANDLE hDevCookie; HANDLE hDevMemContext} = 12 i686 / 24 kernel.
		 * OUT {eError; u32 ui32ClientHeapCount; PVRSRV_HEAP_INFO[32]} = 8 + 32*24 = 776
		 * i686 (kernel 1032; no header handle so heaps sit at @8 on both). */
		*pui32CompatIn  = 3 * sizeof(IMG_UINT32);
		*pui32CompatOut = 2 * sizeof(IMG_UINT32)
			+ PVRSRV_MAX_CLIENT_HEAPS * (6 * sizeof(IMG_UINT32));
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SGX_GETCLIENTINFO))
	{
		/* IN {flags; HANDLE hDevCookie} = 8 i686 / 16 kernel. OUT {SGX_CLIENT_INFO;
		 * eError}: i686 = {u32 ui32ProcessID; void* pvProcess(4); PVRSRV_MISC_INFO(136,
		 * UNUSED by the handler); u32 asDevData[24]; eError} = 4+4+136+96+4 = 244. The
		 * handler fills only ui32ProcessID + asDevData, so the OUT repack zeroes the
		 * sMiscInfo region (gles2tri reads only asDevData). 136 is the i686 sizeof
		 * PVRSRV_MISC_INFO, fixed by the observed out=244. */
		*pui32CompatIn  = 2 * sizeof(IMG_UINT32);
		*pui32CompatOut = 2 * sizeof(IMG_UINT32) + 136
			+ SGX_MAX_DEV_DATA * sizeof(IMG_UINT32) + sizeof(IMG_UINT32);
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SGX_RELEASECLIENTINFO))
	{
		/* IN {flags; HANDLE hDevCookie; SGX_CLIENT_INFO sClientInfo} = i686 4+4+240 = 248
		 * (kernel larger). The handler reads only hDevCookie, so the expand places just
		 * that handle and leaves sClientInfo untranslated. OUT generic RETURN -> 8. */
		*pui32CompatIn  = 2 * sizeof(IMG_UINT32)
			+ (2 * sizeof(IMG_UINT32) + 136 + SGX_MAX_DEV_DATA * sizeof(IMG_UINT32));
		*pui32CompatOut = 2 * sizeof(IMG_UINT32);
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_GET_MISC_INFO))
	{
		/* IN {flags; PVRSRV_MISC_INFO} = 4 + 136 = 140 i686 (kernel 200: misc 8-aligned
		 * at @8, 192 B). OUT {eError; PVRSRV_MISC_INFO} = 140 i686. Both translate the
		 * embedded misc struct. gles2tri uses this for the global event object. */
		*pui32CompatIn  = sizeof(IMG_UINT32) + 136;
		*pui32CompatOut = sizeof(IMG_UINT32) + 136;
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_DESTROY_DEVMEMCONTEXT))
	{
		/* IN {flags; HANDLE hDevCookie; HANDLE hDevMemContext} = 12 i686 / 24 kernel.
		 * OUT generic RETURN -> 8. */
		*pui32CompatIn  = 3 * sizeof(IMG_UINT32);
		*pui32CompatOut = 2 * sizeof(IMG_UINT32);
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_EVENT_OBJECT_WAIT))
	{
		/* IN {flags; HANDLE hOSEventKM} = 8 i686 / 16 kernel. OUT generic RETURN -> 8. */
		*pui32CompatIn  = 2 * sizeof(IMG_UINT32);
		*pui32CompatOut = 2 * sizeof(IMG_UINT32);
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_EVENT_OBJECT_OPEN))
	{
		/* IN {PVRSRV_EVENTOBJECT sEventObject} = i686 56 (szName[50] + handle@52) /
		 * kernel 64 (handle@56). OUT {HANDLE hOSEvent; eError} = i686 8 / kernel 16. */
		*pui32CompatIn  = 56;
		*pui32CompatOut = 2 * sizeof(IMG_UINT32);
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_EVENT_OBJECT_CLOSE))
	{
		/* IN {PVRSRV_EVENTOBJECT sEventObject; HANDLE hOSEventKM} = i686 56+4=60 /
		 * kernel 64+8=72. OUT generic RETURN -> 8. */
		*pui32CompatIn  = 56 + sizeof(IMG_UINT32);
		*pui32CompatOut = 2 * sizeof(IMG_UINT32);
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SGX_GETINTERNALDEVINFO))
	{
		/* IN {flags; HANDLE hDevCookie} = 8 i686 / 16 kernel. OUT {SGX_INTERNAL_DEVINFO
		 * {u32 ui32Flags; HANDLE hHostCtl; IMG_BOOL bForcePTOff}; eError} = i686 12+4=16
		 * (kernel 32). */
		*pui32CompatIn  = 2 * sizeof(IMG_UINT32);
		*pui32CompatOut = 4 * sizeof(IMG_UINT32);
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SYNC_OPS_TAKE_TOKEN))
	{
		/* IN {flags; HANDLE hKernelSyncInfo} = 8 i686 / 16 kernel. OUT {eError;
		 * ui32ReadOpsPending; ui32WriteOpsPending} = 12 on both arches -> leave OUT. */
		*pui32CompatIn = 2 * sizeof(IMG_UINT32);
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SGX_REGISTER_HW_TRANSFER_CONTEXT)
	    || ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SGX_REGISTER_HW_RENDER_CONTEXT))
	{
		/* Both IN {flags; HANDLE hDevCookie; IMG_CPU_VIRTADDR pCpuVAddr; u32 size; u32
		 * offset; HANDLE hDevMemContext} = i686 24 / kernel 40 (3 handles/ptrs widen).
		 * Both OUT {eError; HANDLE hCtx; IMG_DEV_VIRTADDR devvaddr} = i686 12 / kernel 20.
		 * Byte-identical layouts. */
		*pui32CompatIn  = 6 * sizeof(IMG_UINT32);
		*pui32CompatOut = 3 * sizeof(IMG_UINT32);
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SYNC_OPS_FLUSH_TO_TOKEN))
	{
		/* IN {flags; HANDLE hKernelSyncInfo; u32 readSnap; u32 writeSnap} = 16 i686 /
		 * 24 kernel. OUT generic RETURN -> 8. */
		*pui32CompatIn  = 4 * sizeof(IMG_UINT32);
		*pui32CompatOut = 2 * sizeof(IMG_UINT32);
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_OPEN_DISPCLASS_DEVICE))
	{
		/* IN {flags; u32 ui32DeviceID; HANDLE hDevCookie} = 12 i686 / 16 kernel (handle
		 * at @8 both; only its width differs). OUT {eError; HANDLE hDeviceKM} = 8/16. */
		*pui32CompatIn  = 3 * sizeof(IMG_UINT32);
		*pui32CompatOut = 2 * sizeof(IMG_UINT32);
		return IMG_TRUE;
	}
	/* ---- display-class surface cluster (gles2tri eglCreateWindowSurface) ---- */
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_CLOSE_DISPCLASS_DEVICE))
	{	/* IN {flags; HANDLE hDeviceKM} 8/16; OUT generic RETURN. */
		*pui32CompatIn = 2 * sizeof(IMG_UINT32); *pui32CompatOut = 2 * sizeof(IMG_UINT32);
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_ENUM_DISPCLASS_FORMATS))
	{	/* IN {flags; HANDLE hDeviceKM} 8/16; OUT {eError; count; DISPLAY_FORMAT[10]} =
		 * 4+4+40 = 48, pointer-free -> identical both arches (no repack). */
		*pui32CompatIn = 2 * sizeof(IMG_UINT32); *pui32CompatOut = 2 * sizeof(IMG_UINT32) + 10 * 4;
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_ENUM_DISPCLASS_DIMS))
	{	/* IN {flags; HANDLE hDeviceKM; DISPLAY_FORMAT sFormat(4)} 12/24; OUT {eError;
		 * count; DISPLAY_DIMS[10](12 each)} = 4+4+120 = 128, pointer-free (no repack). */
		*pui32CompatIn = 3 * sizeof(IMG_UINT32); *pui32CompatOut = 2 * sizeof(IMG_UINT32) + 10 * 12;
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_GET_DISPCLASS_SYSBUFFER))
	{	/* IN {flags; HANDLE hDeviceKM} 8/16; OUT {eError; HANDLE hBuffer} 8/16. */
		*pui32CompatIn = 2 * sizeof(IMG_UINT32); *pui32CompatOut = 2 * sizeof(IMG_UINT32);
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_GET_DISPCLASS_INFO))
	{	/* IN {flags; HANDLE hDeviceKM} 8/16; OUT {eError; DISPLAY_INFO(76)} = 80,
		 * pointer-free (no repack). */
		*pui32CompatIn = 2 * sizeof(IMG_UINT32); *pui32CompatOut = sizeof(IMG_UINT32) + 76;
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_CREATE_DISPCLASS_SWAPCHAIN))
	{	/* IN {flags; HANDLE hDeviceKM; u32 Flags; SURF_ATTR sDst(16); SURF_ATTR sSrc(16);
		 * u32 BufferCount; u32 OEMFlags; u32 SwapChainID} = 56 i686 / 64 kernel. OUT
		 * {eError; HANDLE hSwapChain; u32 SwapChainID} = 12 i686 / 20 kernel. */
		*pui32CompatIn = 14 * sizeof(IMG_UINT32); *pui32CompatOut = 3 * sizeof(IMG_UINT32);
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_GET_DISPCLASS_BUFFERS))
	{	/* IN {flags; HANDLE hDeviceKM; HANDLE hSwapChain} 12/24; OUT {eError; u32 count;
		 * HANDLE ahBuffer[9]} = 4+4+9*4 = 44 i686 / 4+4+9*8 = 80 kernel. */
		*pui32CompatIn = 3 * sizeof(IMG_UINT32); *pui32CompatOut = 2 * sizeof(IMG_UINT32) + 9 * 4;
		return IMG_TRUE;
	}
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_WRAP_EXT_MEMORY))
	{	/* IN {flags; HANDLE hDevCookie; HANDLE hDevMemContext; void *pvLinAddr; SIZE_T
		 * byteSize; SIZE_T pageOffset; IMG_BOOL bPhysContig; u32 numPTEs; SYS_PHYADDR
		 * *psSysPAddr; u32 flags} = 40 i686 / 64 kernel. OUT {eError; CLIENT_MEM_INFO(48);
		 * CLIENT_SYNC_INFO(20)} = 72 i686 / 128 kernel. (psSysPAddr array only read when
		 * numPTEs>0 — the contiguous fb path uses 0.) */
		*pui32CompatIn  = 10 * sizeof(IMG_UINT32);
		*pui32CompatOut = sizeof(IMG_UINT32) + 48 + 20;
		return IMG_TRUE;
	}
	return IMG_FALSE;
}

/* Expand the i686 IN (already copied into psBridgeIn, compat-sized) up to the
 * kernel layout, in place. Expands larger, so any multi-field bridge must rewrite
 * back-to-front; handle-free INs (like CONNECT_SERVICES) are a no-op. */
/* Expand SGX_DEVINITPART2's IN (the microkernel upload) from the i686 packed layout
 * (2068 B) to the kernel layout (2192 B), in place. The struct is a fixed sequence of
 * handles (4->8, each run 8-aligned in the kernel), u32s, and two pointer-free opaque
 * blocks (SGX_INIT_SCRIPTS 1728 B, SGX_MISCINFO_STRUCT_SIZES 52 B). A descriptor walk
 * computes both offsets (so they can't silently drift), then applies the moves highest
 * offset first so a write never lands on an i686 source word not yet consumed. The field
 * set matches THIS build's config (no SID/MP/VDM/HWPROFILING/FIX_HW_BRN; HWPERF on); the
 * BUILD_BUG_ON pins the kernel size so any struct/config drift fails the compile. */
static void PVRCompatExpandDevInitPart2(IMG_VOID *pvBridgeIn)
{
	static const struct { IMG_CHAR kind; IMG_UINT32 cnt; } aFields[] = {
		{'U', 1},    /* ui32BridgeFlags */
		{'H', 1},    /* hDevCookie */
		{'H', 6},    /* CCB, CCBCtl, CCBEventKicker, SGXHostCtl, SGXTA3DCtl, SGXMisc */
		{'U', 11},   /* aui32HostKickAddr[SGXMKIF_CMD_MAX] */
		{'B', 1728}, /* SGX_INIT_SCRIPTS (144 x 12-byte pointer-free commands) */
		{'U', 1},    /* ui32ClientBuildOptions */
		{'B', 52},   /* SGX_MISCINFO_STRUCT_SIZES (13 x u32) */
		{'H', 1},    /* hKernelHWPerfCBMemInfo (SUPPORT_SGX_HWPERF) */
		{'H', 2},    /* hKernelTASigBufferMemInfo, hKernel3DSigBufferMemInfo */
		{'U', 7},    /* EDMTaskReg0/1, ClkGateCtl/2, ClkGateStatusReg/Mask, CacheControl */
		{'U', 24},   /* asInitDevData[SGX_MAX_DEV_DATA] */
		{'H', 18},   /* asInitMemHandles[SGX_MAX_INIT_MEM_HANDLES] */
	};
	IMG_UINT32 nFields = sizeof(aFields) / sizeof(aFields[0]);
	IMG_UINT32 aSrc[16], aDst[16];
	IMG_UINT32 so = 0, ko = 0, i;
	IMG_PBYTE pby = pvBridgeIn;

	BUILD_BUG_ON(sizeof(PVRSRV_BRIDGE_IN_SGXDEVINITPART2) != 2192);

	for (i = 0; i < nFields; i++)
	{
		if (aFields[i].kind == 'H')
			ko = (ko + 7u) & ~7u;	/* 8-align the first handle of the run */
		aSrc[i] = so;
		aDst[i] = ko;
		if (aFields[i].kind == 'H')      { so += aFields[i].cnt * 4u; ko += aFields[i].cnt * 8u; }
		else if (aFields[i].kind == 'U') { so += aFields[i].cnt * 4u; ko += aFields[i].cnt * 4u; }
		else                             { so += aFields[i].cnt;      ko += aFields[i].cnt;      }
	}

	for (i = nFields; i-- > 0; )
	{
		IMG_PBYTE s = pby + aSrc[i];
		IMG_PBYTE d = pby + aDst[i];
		IMG_UINT32 c = aFields[i].cnt;
		IMG_UINT32 j;

		if (aFields[i].kind == 'H')
			for (j = c; j-- > 0; )	/* high index first: dst (8*j) never clobbers unread src (4*j) */
				*(IMG_HANDLE *)(d + j * 8u) = (IMG_HANDLE)(unsigned long)*(IMG_UINT32 *)(s + j * 4u);
		else if (aFields[i].kind == 'U')
			for (j = c; j-- > 0; )
				*(IMG_UINT32 *)(d + j * 4u) = *(IMG_UINT32 *)(s + j * 4u);
		else
			for (j = c; j-- > 0; )	/* byte copy backward (dst >= src) */
				d[j] = s[j];
	}
}

/* Field map of PVRSRV_MISC_INFO: i686 offset, kernel offset, kind ('U'=u32, 'P'=ptr or
 * handle 4<->8, 'B'=opaque byte block of size sz). i686 is 136 B, kernel 192 B
 * (EVENTOBJNAME_MAXLENGTH=50; pointers/handles 8-byte and 8-aligned, so the nested
 * PVRSRV_EVENTOBJECT and sCacheOpCtl shift). Fields are in ascending i686 order. */
struct pvr_misc_fld { IMG_UINT16 i6; IMG_UINT16 kn; IMG_CHAR k; IMG_UINT16 sz; };
static const struct pvr_misc_fld g_aMiscInfoFields[] = {
	{  0,   0, 'U', 0 },	/* ui32StateRequest */
	{  4,   4, 'U', 0 },	/* ui32StatePresent */
	{  8,   8, 'P', 0 },	/* pvSOCTimerRegisterKM */
	{ 12,  16, 'P', 0 },	/* pvSOCTimerRegisterUM */
	{ 16,  24, 'P', 0 },	/* hSOCTimerRegisterOSMemHandle */
	{ 20,  32, 'P', 0 },	/* hSOCTimerRegisterMappingInfo */
	{ 24,  40, 'P', 0 },	/* pvSOCClockGateRegs */
	{ 28,  48, 'U', 0 },	/* ui32SOCClockGateRegsSize */
	{ 32,  56, 'P', 0 },	/* pszMemoryStr */
	{ 36,  64, 'U', 0 },	/* ui32MemoryStrLen */
	{ 40,  72, 'B', 50 },	/* sGlobalEventObject.szName[EVENTOBJNAME_MAXLENGTH] */
	{ 92, 128, 'P', 0 },	/* sGlobalEventObject.hOSEventKM */
	{ 96, 136, 'P', 0 },	/* hOSGlobalEvent */
	{100, 144, 'B', 16 },	/* aui32DDKVersion[4] */
	{116, 160, 'U', 0 },	/* sCacheOpCtl.bDeferOp */
	{120, 164, 'U', 0 },	/* sCacheOpCtl.eCacheOpType */
	{124, 168, 'P', 0 },	/* sCacheOpCtl.u (psClientMemInfo/psKernelMemInfo) */
	{128, 176, 'P', 0 },	/* sCacheOpCtl.pvBaseVAddr */
	{132, 184, 'U', 0 },	/* sCacheOpCtl.ui32Length */
};

/* Translate one PVRSRV_MISC_INFO between the i686 and kernel layouts. pDst/pSrc point at
 * the misc struct's base in each layout (they may differ and overlap within one buffer).
 * Expand (i686->kernel) grows, so walk fields high-i686-offset first; compact shrinks, so
 * walk low first — either way a write never lands on a source word not yet consumed. */
static void PVRCompatXlateMiscInfo(IMG_PBYTE pDst, IMG_PBYTE pSrc, IMG_BOOL bExpand)
{
	IMG_UINT32 n = sizeof(g_aMiscInfoFields) / sizeof(g_aMiscInfoFields[0]);
	IMG_UINT32 ii, j;

	BUILD_BUG_ON(sizeof(PVRSRV_MISC_INFO) != 192);

	if (bExpand)
	{
		for (ii = n; ii-- > 0; )
		{
			const struct pvr_misc_fld *f = &g_aMiscInfoFields[ii];
			IMG_PBYTE s = pSrc + f->i6;
			IMG_PBYTE d = pDst + f->kn;

			if (f->k == 'U')      { *(IMG_UINT32 *)d = *(IMG_UINT32 *)s; }
			else if (f->k == 'P') { *(IMG_HANDLE *)d = (IMG_HANDLE)(unsigned long)*(IMG_UINT32 *)s; }
			else                  { for (j = f->sz; j-- > 0; ) d[j] = s[j]; }
		}
	}
	else
	{
		for (ii = 0; ii < n; ii++)
		{
			const struct pvr_misc_fld *f = &g_aMiscInfoFields[ii];
			IMG_PBYTE s = pSrc + f->kn;
			IMG_PBYTE d = pDst + f->i6;

			if (f->k == 'U')      { *(IMG_UINT32 *)d = *(IMG_UINT32 *)s; }
			else if (f->k == 'P') { *(IMG_UINT32 *)d = (IMG_UINT32)(unsigned long)*(IMG_HANDLE *)s; }
			else                  { for (j = 0; j < f->sz; j++) d[j] = s[j]; }
		}
	}
}

static void PVRCompatExpandIn(IMG_UINT32 ui32BridgeID, IMG_VOID *pvBridgeIn)
{
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SGXINFO_FOR_SRVINIT))
	{
		/* i686 {u32 flags@0, u32 hDevCookie@4} -> kernel {u32 flags@0, 4 pad,
		 * IMG_HANDLE hDevCookie@8}. Read the 4-byte cookie before writing the
		 * wider field (offset 8 is past it, so the read is safe either way). The
		 * cookie is a small index-handle; zero-extend is lossless. flags@0 stays. */
		PVRSRV_BRIDGE_IN_SGXINFO_FOR_SRVINIT *psK = pvBridgeIn;
		IMG_UINT32 ui32Cookie = ((IMG_UINT32 *)pvBridgeIn)[1];

		psK->hDevCookie = (IMG_HANDLE)(unsigned long)ui32Cookie;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_ALLOC_DEVICEMEM))
	{
		/* i686 {flags@0, hDevCookie@4, hDevMemHeap@8, attribs@12, size@16, align@20}
		 * -> kernel {flags@0, pad, hDevCookie@8, hDevMemHeap@16, attribs@24, size@28,
		 * align@32}. Read all six i686 words first, then write the wider layout. */
		IMG_UINT32 *p = pvBridgeIn;
		IMG_UINT32 ui32Flags  = p[0];
		IMG_UINT32 ui32Cookie = p[1];
		IMG_UINT32 ui32Heap   = p[2];
		IMG_UINT32 ui32Attribs= p[3];
		IMG_UINT32 ui32Size   = p[4];
		IMG_UINT32 ui32Align  = p[5];
		PVRSRV_BRIDGE_IN_ALLOCDEVICEMEM *psK = pvBridgeIn;

		psK->ui32BridgeFlags = ui32Flags;
		psK->hDevCookie      = (IMG_HANDLE)(unsigned long)ui32Cookie;
		psK->hDevMemHeap     = (IMG_HANDLE)(unsigned long)ui32Heap;
		psK->ui32Attribs     = ui32Attribs;
		psK->ui32Size        = ui32Size;
		psK->ui32Alignment   = ui32Align;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_MHANDLE_TO_MMAP_DATA)
		 || ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_RELEASE_MMAP_DATA))
	{
		/* Both: i686 {flags@0, hMHandle@4(4)} -> kernel {flags@0, pad, hMHandle@8(8)}.
		 * Byte-identical layout, so write the handle at kernel offset 8 by raw offset. */
		IMG_UINT32 ui32MHandle = ((IMG_UINT32 *)pvBridgeIn)[1];

		*(IMG_HANDLE *)((IMG_PBYTE)pvBridgeIn + 8) = (IMG_HANDLE)(unsigned long)ui32MHandle;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SGX_DEVINITPART2))
	{
		PVRCompatExpandDevInitPart2(pvBridgeIn);
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SGX_GETMISCINFO))
	{
		/* i686 {flags@0, hDevCookie@4, psMiscInfo@8} -> kernel {flags@0, pad,
		 * hDevCookie@8, psMiscInfo@16}. hDevCookie is an index-handle; psMiscInfo is a
		 * 32-bit user VA -> zero-extension == compat_ptr. Read both words first. */
		IMG_UINT32 *p = pvBridgeIn;
		IMG_UINT32 ui32Flags  = p[0];
		IMG_UINT32 ui32Cookie = p[1];
		IMG_UINT32 ui32Misc   = p[2];
		PVRSRV_BRIDGE_IN_SGXGETMISCINFO *psK = pvBridgeIn;

		psK->ui32BridgeFlags = ui32Flags;
		psK->hDevCookie      = (IMG_HANDLE)(unsigned long)ui32Cookie;
		psK->psMiscInfo      = (SGX_MISC_INFO *)(unsigned long)ui32Misc;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_CREATE_DEVMEMCONTEXT))
	{
		/* i686 {flags@0, hDevCookie@4} -> kernel {flags@0, pad, hDevCookie@8}. */
		IMG_UINT32 ui32Cookie = ((IMG_UINT32 *)pvBridgeIn)[1];
		PVRSRV_BRIDGE_IN_CREATE_DEVMEMCONTEXT *psK = pvBridgeIn;

		psK->hDevCookie = (IMG_HANDLE)(unsigned long)ui32Cookie;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_GET_DEVMEM_HEAPINFO))
	{
		/* i686 {flags@0, hDevCookie@4, hDevMemContext@8} -> kernel {flags@0, pad,
		 * hDevCookie@8, hDevMemContext@16}. Read both handles first. */
		IMG_UINT32 *p = pvBridgeIn;
		IMG_UINT32 ui32Flags  = p[0];
		IMG_UINT32 ui32Cookie = p[1];
		IMG_UINT32 ui32Ctx    = p[2];
		PVRSRV_BRIDGE_IN_GET_DEVMEM_HEAPINFO *psK = pvBridgeIn;

		psK->ui32BridgeFlags = ui32Flags;
		psK->hDevCookie      = (IMG_HANDLE)(unsigned long)ui32Cookie;
		psK->hDevMemContext  = (IMG_HANDLE)(unsigned long)ui32Ctx;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SGX_GETCLIENTINFO))
	{
		/* i686 {flags@0, hDevCookie@4} -> kernel {flags@0, pad, hDevCookie@8}. */
		IMG_UINT32 ui32Cookie = ((IMG_UINT32 *)pvBridgeIn)[1];
		PVRSRV_BRIDGE_IN_GETCLIENTINFO *psK = pvBridgeIn;

		psK->hDevCookie = (IMG_HANDLE)(unsigned long)ui32Cookie;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SGX_RELEASECLIENTINFO))
	{
		/* i686 {flags@0, hDevCookie@4, sClientInfo@8..248} -> kernel {flags@0, pad,
		 * hDevCookie@8, sClientInfo@16..}. Handler reads only hDevCookie; place it and
		 * leave sClientInfo unexpanded. */
		IMG_UINT32 ui32Cookie = ((IMG_UINT32 *)pvBridgeIn)[1];

		*(IMG_HANDLE *)((IMG_PBYTE)pvBridgeIn + 8) = (IMG_HANDLE)(unsigned long)ui32Cookie;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_FREE_DEVICEMEM))
	{
		/* i686 {flags@0, hDevCookie@4, psKernelMemInfo@8, sClientMemInfo@12..60} ->
		 * kernel {flags@0, pad, hDevCookie@8, psKernelMemInfo@16, sClientMemInfo@24..}.
		 * The handler reads only hDevCookie + psKernelMemInfo (as handle lookups), so
		 * leave the embedded sClientMemInfo unexpanded. Read the three head words first. */
		IMG_UINT32 *p = pvBridgeIn;
		IMG_UINT32 ui32Flags  = p[0];
		IMG_UINT32 ui32Cookie = p[1];
		IMG_UINT32 ui32KMI    = p[2];
		PVRSRV_BRIDGE_IN_FREEDEVICEMEM *psK = pvBridgeIn;

		psK->ui32BridgeFlags = ui32Flags;
		psK->hDevCookie      = (IMG_HANDLE)(unsigned long)ui32Cookie;
		psK->psKernelMemInfo = (PVRSRV_KERNEL_MEM_INFO *)(unsigned long)ui32KMI;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_GET_MISC_INFO))
	{
		/* IN {flags@0; PVRSRV_MISC_INFO sMiscInfo}: i686 misc@4 -> kernel misc@8 (8-align);
		 * flags@0 untouched (lowest misc write is @8). */
		PVRCompatXlateMiscInfo((IMG_PBYTE)pvBridgeIn + 8,
				       (IMG_PBYTE)pvBridgeIn + 4, IMG_TRUE);
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_DESTROY_DEVMEMCONTEXT))
	{
		/* i686 {flags@0, hDevCookie@4, hDevMemContext@8} -> kernel {flags@0, pad,
		 * hDevCookie@8, hDevMemContext@16}. Read both handles first. */
		IMG_UINT32 *p = pvBridgeIn;
		IMG_UINT32 ui32Flags  = p[0];
		IMG_UINT32 ui32Cookie = p[1];
		IMG_UINT32 ui32Ctx    = p[2];
		PVRSRV_BRIDGE_IN_DESTROY_DEVMEMCONTEXT *psK = pvBridgeIn;

		psK->ui32BridgeFlags = ui32Flags;
		psK->hDevCookie      = (IMG_HANDLE)(unsigned long)ui32Cookie;
		psK->hDevMemContext  = (IMG_HANDLE)(unsigned long)ui32Ctx;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_EVENT_OBJECT_WAIT))
	{
		/* i686 {flags@0, hOSEventKM@4} -> kernel {flags@0, pad, hOSEventKM@8}. */
		IMG_UINT32 ui32H = ((IMG_UINT32 *)pvBridgeIn)[1];

		*(IMG_HANDLE *)((IMG_PBYTE)pvBridgeIn + 8) = (IMG_HANDLE)(unsigned long)ui32H;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_EVENT_OBJECT_OPEN))
	{
		/* IN {PVRSRV_EVENTOBJECT}: szName[50]@0 stays; hOSEventKM i686@52 -> kernel@56. */
		IMG_PBYTE pby = pvBridgeIn;
		IMG_UINT32 ui32H = *(IMG_UINT32 *)(pby + 52);

		*(IMG_HANDLE *)(pby + 56) = (IMG_HANDLE)(unsigned long)ui32H;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_EVENT_OBJECT_CLOSE))
	{
		/* IN {PVRSRV_EVENTOBJECT sEventObject; HANDLE hOSEventKM}: szName@0 stays;
		 * EVENTOBJECT.hOSEventKM i686@52 -> kernel@56; trailing hOSEventKM i686@56 ->
		 * kernel@64. Read both handles before writing (kernel@56 clobbers i686@56). */
		IMG_PBYTE pby = pvBridgeIn;
		IMG_UINT32 ui32EvtH   = *(IMG_UINT32 *)(pby + 52);
		IMG_UINT32 ui32CloseH = *(IMG_UINT32 *)(pby + 56);

		*(IMG_HANDLE *)(pby + 56) = (IMG_HANDLE)(unsigned long)ui32EvtH;
		*(IMG_HANDLE *)(pby + 64) = (IMG_HANDLE)(unsigned long)ui32CloseH;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SGX_GETINTERNALDEVINFO))
	{
		/* i686 {flags@0, hDevCookie@4} -> kernel {flags@0, pad, hDevCookie@8}. */
		IMG_UINT32 ui32Cookie = ((IMG_UINT32 *)pvBridgeIn)[1];
		PVRSRV_BRIDGE_IN_GETINTERNALDEVINFO *psK = pvBridgeIn;

		psK->hDevCookie = (IMG_HANDLE)(unsigned long)ui32Cookie;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SYNC_OPS_TAKE_TOKEN))
	{
		/* i686 {flags@0, hKernelSyncInfo@4} -> kernel {flags@0, pad, hKernelSyncInfo@8}. */
		IMG_UINT32 ui32H = ((IMG_UINT32 *)pvBridgeIn)[1];
		PVRSRV_BRIDGE_IN_SYNC_OPS_TAKE_TOKEN *psK = pvBridgeIn;

		psK->hKernelSyncInfo = (IMG_HANDLE)(unsigned long)ui32H;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SGX_REGISTER_HW_TRANSFER_CONTEXT)
		 || ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SGX_REGISTER_HW_RENDER_CONTEXT))
	{
		/* Both (transfer + render ctx, byte-identical): i686 {flags@0, hDevCookie@4,
		 * pCpuVAddr@8, size@12, offset@16, hDevMemContext@20} -> kernel {flags@0, pad,
		 * hDevCookie@8, pCpuVAddr@16, size@24, offset@28, hDevMemContext@32}. 3
		 * handles/ptrs zero-extend; the 2 u32 shift up. Read all words, then write (raw
		 * offsets since the two structs differ only in field names). */
		IMG_UINT32 *p = pvBridgeIn;
		IMG_UINT32 ui32Flags  = p[0];
		IMG_UINT32 ui32Cookie = p[1];
		IMG_UINT32 ui32CpuVA  = p[2];
		IMG_UINT32 ui32Size   = p[3];
		IMG_UINT32 ui32Offset = p[4];
		IMG_UINT32 ui32MemCtx = p[5];
		IMG_PBYTE pby = pvBridgeIn;

		*(IMG_UINT32 *)(pby + 0)  = ui32Flags;
		*(IMG_HANDLE *)(pby + 8)  = (IMG_HANDLE)(unsigned long)ui32Cookie;
		*(IMG_HANDLE *)(pby + 16) = (IMG_HANDLE)(unsigned long)ui32CpuVA;
		*(IMG_UINT32 *)(pby + 24) = ui32Size;
		*(IMG_UINT32 *)(pby + 28) = ui32Offset;
		*(IMG_HANDLE *)(pby + 32) = (IMG_HANDLE)(unsigned long)ui32MemCtx;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SYNC_OPS_FLUSH_TO_TOKEN))
	{
		/* i686 {flags@0, hKernelSyncInfo@4, readSnap@8, writeSnap@12} -> kernel {flags@0,
		 * pad, hKernelSyncInfo@8, readSnap@16, writeSnap@20}. Read all first. */
		IMG_UINT32 *p = pvBridgeIn;
		IMG_UINT32 ui32Flags = p[0];
		IMG_UINT32 ui32H     = p[1];
		IMG_UINT32 ui32Read  = p[2];
		IMG_UINT32 ui32Write = p[3];
		PVRSRV_BRIDGE_IN_SYNC_OPS_FLUSH_TO_TOKEN *psK = pvBridgeIn;

		psK->ui32BridgeFlags             = ui32Flags;
		psK->hKernelSyncInfo             = (IMG_HANDLE)(unsigned long)ui32H;
		psK->ui32ReadOpsPendingSnapshot  = ui32Read;
		psK->ui32WriteOpsPendingSnapshot = ui32Write;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_OPEN_DISPCLASS_DEVICE))
	{
		/* i686 {flags@0, ui32DeviceID@4, hDevCookie@8} -> kernel {flags@0, devID@4,
		 * hDevCookie@8(8)}. flags+devID already right; just widen the handle at @8. */
		IMG_UINT32 ui32Cookie = ((IMG_UINT32 *)pvBridgeIn)[2];

		*(IMG_HANDLE *)((IMG_PBYTE)pvBridgeIn + 8) = (IMG_HANDLE)(unsigned long)ui32Cookie;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_CLOSE_DISPCLASS_DEVICE)
		 || ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_ENUM_DISPCLASS_FORMATS)
		 || ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_GET_DISPCLASS_SYSBUFFER)
		 || ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_GET_DISPCLASS_INFO))
	{
		/* IN {flags; HANDLE hDeviceKM} — widen the handle i686@4 -> kernel@8. */
		IMG_UINT32 ui32H = ((IMG_UINT32 *)pvBridgeIn)[1];

		*(IMG_HANDLE *)((IMG_PBYTE)pvBridgeIn + 8) = (IMG_HANDLE)(unsigned long)ui32H;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_ENUM_DISPCLASS_DIMS))
	{
		/* i686 {flags@0, hDeviceKM@4, sFormat@8(4)} -> kernel {flags@0, pad, hDeviceKM@8,
		 * sFormat@16(4)}. Read before writing (sFormat read first, then widen handle). */
		IMG_UINT32 ui32H   = ((IMG_UINT32 *)pvBridgeIn)[1];
		IMG_UINT32 ui32Fmt = ((IMG_UINT32 *)pvBridgeIn)[2];

		*(IMG_UINT32 *)((IMG_PBYTE)pvBridgeIn + 16) = ui32Fmt;
		*(IMG_HANDLE *)((IMG_PBYTE)pvBridgeIn + 8)  = (IMG_HANDLE)(unsigned long)ui32H;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_GET_DISPCLASS_BUFFERS))
	{
		/* i686 {flags@0, hDeviceKM@4, hSwapChain@8} -> kernel {flags@0, pad, hDeviceKM@8,
		 * hSwapChain@16}. Read both handles first. */
		IMG_UINT32 ui32Dev  = ((IMG_UINT32 *)pvBridgeIn)[1];
		IMG_UINT32 ui32Swap = ((IMG_UINT32 *)pvBridgeIn)[2];

		*(IMG_HANDLE *)((IMG_PBYTE)pvBridgeIn + 8)  = (IMG_HANDLE)(unsigned long)ui32Dev;
		*(IMG_HANDLE *)((IMG_PBYTE)pvBridgeIn + 16) = (IMG_HANDLE)(unsigned long)ui32Swap;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_CREATE_DISPCLASS_SWAPCHAIN))
	{
		/* i686 {flags@0, hDeviceKM@4, [48-byte tail @8..56]} -> kernel {flags@0, pad,
		 * hDeviceKM@8, [48-byte tail @16..64]}. Move the tail up (back-to-front vs the
		 * handle), then widen the handle. Snapshot handle first. */
		IMG_PBYTE pby = pvBridgeIn;
		IMG_UINT32 ui32Dev = ((IMG_UINT32 *)pby)[1];
		IMG_UINT32 j;

		for (j = 48; j-- > 0; ) pby[16 + j] = pby[8 + j];   /* tail @8..56 -> @16..64 */
		*(IMG_HANDLE *)(pby + 8) = (IMG_HANDLE)(unsigned long)ui32Dev;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_WRAP_EXT_MEMORY))
	{
		/* i686 40 {flags@0, hDevCookie@4, hDevMemContext@8, pvLinAddr@12, byteSize@16,
		 * pageOffset@20, bPhysContig@24, numPTEs@28, psSysPAddr@32, flags2@36} -> kernel
		 * 64 {flags@0, pad, hDevCookie@8, hDevMemContext@16, pvLinAddr@24, byteSize@32,
		 * pageOffset@36, bPhysContig@40, numPTEs@44, pad, psSysPAddr@56, flags2@60}. Read
		 * all 10 words first. NOTE: psSysPAddr points at a user IMG_SYS_PHYADDR[] that is
		 * 4-byte/elem on i686 vs 8 on kernel — only read by the handler when numPTEs>0
		 * (the contiguous fb path uses 0). If a count>0 path ever appears, that array
		 * needs its own translation. */
		IMG_UINT32 *p = pvBridgeIn;
		IMG_UINT32 ui32Flags = p[0], ui32Cookie = p[1], ui32MemCtx = p[2], ui32Lin = p[3];
		IMG_UINT32 ui32Size = p[4], ui32POff = p[5], ui32Phys = p[6], ui32N = p[7];
		IMG_UINT32 ui32Sys = p[8], ui32Flags2 = p[9];
		IMG_PBYTE pby = pvBridgeIn;

		*(IMG_UINT32 *)(pby + 0)  = ui32Flags;
		*(IMG_HANDLE *)(pby + 8)  = (IMG_HANDLE)(unsigned long)ui32Cookie;
		*(IMG_HANDLE *)(pby + 16) = (IMG_HANDLE)(unsigned long)ui32MemCtx;
		*(IMG_HANDLE *)(pby + 24) = (IMG_HANDLE)(unsigned long)ui32Lin;
		*(IMG_UINT32 *)(pby + 32) = ui32Size;
		*(IMG_UINT32 *)(pby + 36) = ui32POff;
		*(IMG_UINT32 *)(pby + 40) = ui32Phys;
		*(IMG_UINT32 *)(pby + 44) = ui32N;
		*(IMG_HANDLE *)(pby + 56) = (IMG_HANDLE)(unsigned long)ui32Sys;
		*(IMG_UINT32 *)(pby + 60) = ui32Flags2;
	}
	/* Handle-free INs (CONNECT_SERVICES, the generic-return bridges) need nothing. */
}

/* Compact the kernel OUT (in psBridgeOut) down to the i686 layout, in place,
 * before CopyToUser. */
static void PVRCompatCompactOut(IMG_UINT32 ui32BridgeID, IMG_VOID *pvBridgeOut)
{
	if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_CONNECT_SERVICES))
	{
		PVRSRV_BRIDGE_OUT_CONNECT_SERVICES *psK = pvBridgeOut;
		IMG_UINT32 ui32Err    = (IMG_UINT32)psK->eError;
		IMG_UINT32 ui32Handle = (IMG_UINT32)(unsigned long)psK->hKernelServices;
		IMG_UINT32 *pui32 = pvBridgeOut;

		pui32[0] = ui32Err;
		pui32[1] = ui32Handle;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_ACQUIRE_DEVICEINFO))
	{
		PVRSRV_BRIDGE_OUT_ACQUIRE_DEVICEINFO *psK = pvBridgeOut;
		IMG_UINT32 ui32Err    = (IMG_UINT32)psK->eError;
		IMG_UINT32 ui32Cookie = (IMG_UINT32)(unsigned long)psK->hDevCookie;
		IMG_UINT32 *pui32 = pvBridgeOut;

		pui32[0] = ui32Err;
		pui32[1] = ui32Cookie;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_ENUM_DEVICES))
	{
		/* Repack the identifier array from the kernel's 32-byte stride to the
		 * i686 20-byte stride. eError + ui32NumDevices stay at offsets 0,4. Read
		 * each source identifier into locals BEFORE writing the (lower-offset)
		 * destination, and go forward: dest[i] ends at 28+i*20, source[i+1] starts
		 * at 40+i*32, so a write never clobbers an unread source. The two PDUMP
		 * name pointers are kernel addresses a release userspace ignores -> zero. */
		PVRSRV_BRIDGE_OUT_ENUMDEVICE *psK = pvBridgeOut;
		IMG_PBYTE pbyBase = pvBridgeOut;
		IMG_UINT32 i;

		for (i = 0; i < PVRSRV_MAX_DEVICES; i++)
		{
			PVRSRV_DEVICE_IDENTIFIER *psId = &psK->asDeviceIdentifier[i];
			IMG_UINT32 ui32Type  = (IMG_UINT32)psId->eDeviceType;
			IMG_UINT32 ui32Class = (IMG_UINT32)psId->eDeviceClass;
			IMG_UINT32 ui32Index = psId->ui32DeviceIndex;
			IMG_UINT32 *pui32Dst = (IMG_UINT32 *)(pbyBase + 8 + i * 20);

			pui32Dst[0] = ui32Type;
			pui32Dst[1] = ui32Class;
			pui32Dst[2] = ui32Index;
			pui32Dst[3] = 0;
			pui32Dst[4] = 0;
		}
	}
	else if (PVRCompatBridgeGenericReturn(ui32BridgeID)
		 || ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_FREE_DEVICEMEM)
		 || ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SGX_GETMISCINFO)
		 || ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SGX_RELEASECLIENTINFO)
		 || ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_DESTROY_DEVMEMCONTEXT)
		 || ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_EVENT_OBJECT_WAIT)
		 || ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_EVENT_OBJECT_CLOSE)
		 || ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SYNC_OPS_FLUSH_TO_TOKEN)
		 || ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_CLOSE_DISPCLASS_DEVICE))
	{
		/* PVRSRV_BRIDGE_RETURN {PVRSRV_ERROR eError; IMG_VOID *pvData} ->
		 * i686 {u32 eError, u32 pvData}. pvData is unset by these handlers
		 * (INITSRV_CONNECT/DISCONNECT, DISCONNECT_SERVICES, FREE_DEVICEMEM) -> truncate. */
		PVRSRV_BRIDGE_RETURN *psK = pvBridgeOut;
		IMG_UINT32 ui32Err  = (IMG_UINT32)psK->eError;
		IMG_UINT32 ui32Data = (IMG_UINT32)(unsigned long)psK->pvData;
		IMG_UINT32 *pui32 = pvBridgeOut;

		pui32[0] = ui32Err;
		pui32[1] = ui32Data;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SGXINFO_FOR_SRVINIT))
	{
		/* Repack {eError; {IMG_DEV_PHYADDR sPDDevPAddr; PVRSRV_HEAP_INFO[32]}} from
		 * the kernel 1040-byte layout down to the i686 776-byte one. Header:
		 * eError@0 (same), sPDDevPAddr kernel u64@8 -> i686 u32@4 (phys addr, 32-bit
		 * space, truncation safe). Heaps: kernel 32-byte stride from @16, i686 24-byte
		 * stride from @8; per element {ui32HeapID, hDevMemHeap(8->4 index-handle),
		 * sDevVAddrBase.uiAddr, ui32HeapByteSize, ui32Attribs, ui32XTileStride}. Read
		 * each source element fully into locals before writing the lower-offset dest,
		 * and go forward -> no write clobbers an unread source. */
		PVRSRV_BRIDGE_OUT_SGXINFO_FOR_SRVINIT *psK = pvBridgeOut;
		IMG_PBYTE pbyBase = pvBridgeOut;
		IMG_UINT32 ui32Err = (IMG_UINT32)psK->eError;
		IMG_UINT32 ui32PD  = (IMG_UINT32)psK->sInitInfo.sPDDevPAddr.uiAddr;
		IMG_UINT32 i;

		((IMG_UINT32 *)pbyBase)[0] = ui32Err;	/* @0 */
		((IMG_UINT32 *)pbyBase)[1] = ui32PD;	/* @4 */

		for (i = 0; i < PVRSRV_MAX_CLIENT_HEAPS; i++)
		{
			PVRSRV_HEAP_INFO *psH = &psK->sInitInfo.asHeapInfo[i];
			IMG_UINT32 ui32HeapID = psH->ui32HeapID;
			IMG_UINT32 ui32Heap   = (IMG_UINT32)(unsigned long)psH->hDevMemHeap;
			IMG_UINT32 ui32VAddr  = psH->sDevVAddrBase.uiAddr;
			IMG_UINT32 ui32Size   = psH->ui32HeapByteSize;
			IMG_UINT32 ui32Attr   = psH->ui32Attribs;
			IMG_UINT32 ui32XTile  = psH->ui32XTileStride;
			IMG_UINT32 *pui32Dst  = (IMG_UINT32 *)(pbyBase + 8 + i * 24);

			pui32Dst[0] = ui32HeapID;
			pui32Dst[1] = ui32Heap;
			pui32Dst[2] = ui32VAddr;
			pui32Dst[3] = ui32Size;
			pui32Dst[4] = ui32Attr;
			pui32Dst[5] = ui32XTile;
		}
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_ALLOC_DEVICEMEM))
	{
		/* kernel {eError@0; pad; psKernelMemInfo*@8; CLIENT_MEM_INFO@16(88);
		 * CLIENT_SYNC_INFO@104(32)} 136 -> i686 {eError@0; psKernelMemInfo@4;
		 * CLIENT_MEM_INFO@8(48); CLIENT_SYNC_INFO@56(20)} 76. Forward order; each pack
		 * helper reads its whole source before writing the lower-offset dest, and the
		 * header is read first and written last, so no write clobbers an unread source. */
		PVRSRV_BRIDGE_OUT_ALLOCDEVICEMEM *psK = pvBridgeOut;
		IMG_PBYTE pby = pvBridgeOut;
		IMG_UINT32 ui32Err = (IMG_UINT32)psK->eError;
		IMG_UINT32 ui32KMI = (IMG_UINT32)(unsigned long)psK->psKernelMemInfo;

		(void)PVRCompatPackClientMemInfo (pby + 8,  &psK->sClientMemInfo);
		(void)PVRCompatPackClientSyncInfo(pby + 8 + 48, &psK->sClientSyncInfo);
		((IMG_UINT32 *)pby)[0] = ui32Err;
		((IMG_UINT32 *)pby)[1] = ui32KMI;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_CREATE_DEVMEMCONTEXT))
	{
		/* kernel {eError@0; pad; HANDLE hDevMemContext@8; u32 ui32ClientHeapCount@16;
		 * PVRSRV_HEAP_INFO[32]@24 (32B stride)} 1048 -> i686 {eError@0; ctx@4; count@8;
		 * heaps@12 (24B stride)} 780. Header read into locals first; heaps packed forward
		 * (helper reads each 32B element before writing the lower-offset 24B dest); header
		 * written last. */
		PVRSRV_BRIDGE_OUT_CREATE_DEVMEMCONTEXT *psK = pvBridgeOut;
		IMG_PBYTE pby = pvBridgeOut;
		IMG_UINT32 ui32Err   = (IMG_UINT32)psK->eError;
		IMG_UINT32 ui32Ctx   = (IMG_UINT32)(unsigned long)psK->hDevMemContext;
		IMG_UINT32 ui32Count = psK->ui32ClientHeapCount;
		IMG_UINT32 i;

		for (i = 0; i < PVRSRV_MAX_CLIENT_HEAPS; i++)
			(void)PVRCompatPackHeapInfo(pby + 12 + i * 24, &psK->sHeapInfo[i]);

		((IMG_UINT32 *)pby)[0] = ui32Err;
		((IMG_UINT32 *)pby)[1] = ui32Ctx;
		((IMG_UINT32 *)pby)[2] = ui32Count;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_GET_DEVMEM_HEAPINFO))
	{
		/* kernel {eError@0; u32 count@4; PVRSRV_HEAP_INFO[32]@8 (32B stride)} 1032 ->
		 * i686 {eError@0; count@4; heaps@8 (24B stride)} 776. Header (2 u32) sits at the
		 * same offsets on both; pack heaps forward then write the header last. */
		PVRSRV_BRIDGE_OUT_GET_DEVMEM_HEAPINFO *psK = pvBridgeOut;
		IMG_PBYTE pby = pvBridgeOut;
		IMG_UINT32 ui32Err   = (IMG_UINT32)psK->eError;
		IMG_UINT32 ui32Count = psK->ui32ClientHeapCount;
		IMG_UINT32 i;

		for (i = 0; i < PVRSRV_MAX_CLIENT_HEAPS; i++)
			(void)PVRCompatPackHeapInfo(pby + 8 + i * 24, &psK->sHeapInfo[i]);

		((IMG_UINT32 *)pby)[0] = ui32Err;
		((IMG_UINT32 *)pby)[1] = ui32Count;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SGX_GETCLIENTINFO))
	{
		/* kernel SGX_CLIENT_INFO {ui32ProcessID; void *pvProcess; PVRSRV_MISC_INFO
		 * sMiscInfo; u32 asDevData[24]} + eError -> i686 {ui32ProcessID@0; pvProcess@4;
		 * sMiscInfo@8 (136, zeroed); asDevData@144 (96); eError@240} = 244. The handler
		 * only wrote ui32ProcessID + asDevData; snapshot asDevData into a local (the
		 * kernel struct is larger, so the i686 dest overlaps the source) then rebuild. */
		PVRSRV_BRIDGE_OUT_GETCLIENTINFO *psK = pvBridgeOut;
		IMG_PBYTE pby = pvBridgeOut;
		IMG_UINT32 ui32PID  = psK->sClientInfo.ui32ProcessID;
		IMG_UINT32 ui32Proc = (IMG_UINT32)(unsigned long)psK->sClientInfo.pvProcess;
		IMG_UINT32 ui32Err  = (IMG_UINT32)psK->eError;
		IMG_UINT32 aui32DevData[SGX_MAX_DEV_DATA];

		OSMemCopy(aui32DevData, psK->sClientInfo.asDevData, sizeof(aui32DevData));
		((IMG_UINT32 *)pby)[0] = ui32PID;
		((IMG_UINT32 *)pby)[1] = ui32Proc;
		OSMemSet(pby + 8, 0, 136);
		OSMemCopy(pby + 8 + 136, aui32DevData, sizeof(aui32DevData));
		*(IMG_UINT32 *)(pby + 8 + 136 + sizeof(aui32DevData)) = ui32Err;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_GET_MISC_INFO))
	{
		/* OUT {eError@0; PVRSRV_MISC_INFO sMiscInfo}: kernel misc@8 -> i686 misc@4.
		 * eError@0 is untouched (lowest misc write is @4). */
		PVRCompatXlateMiscInfo((IMG_PBYTE)pvBridgeOut + 4,
				       (IMG_PBYTE)pvBridgeOut + 8, IMG_FALSE);
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_EVENT_OBJECT_OPEN))
	{
		/* kernel {HANDLE hOSEvent@0; eError@8} 16 -> i686 {hOSEvent@0(4); eError@4} 8. */
		PVRSRV_BRIDGE_OUT_EVENT_OBJECT_OPEN *psK = pvBridgeOut;
		IMG_UINT32 ui32Evt = (IMG_UINT32)(unsigned long)psK->hOSEvent;
		IMG_UINT32 ui32Err = (IMG_UINT32)psK->eError;
		IMG_UINT32 *p = pvBridgeOut;

		p[0] = ui32Evt;
		p[1] = ui32Err;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SGX_GETINTERNALDEVINFO))
	{
		/* kernel {SGX_INTERNAL_DEVINFO{u32 ui32Flags@0; HANDLE hHostCtl@8; IMG_BOOL
		 * bForcePTOff@16}; eError@24} 32 -> i686 {ui32Flags@0; hHostCtl@4; bForcePTOff@8;
		 * eError@12} 16. */
		PVRSRV_BRIDGE_OUT_GETINTERNALDEVINFO *psK = pvBridgeOut;
		IMG_UINT32 ui32Flags = psK->sSGXInternalDevInfo.ui32Flags;
		IMG_UINT32 ui32Host  = (IMG_UINT32)(unsigned long)psK->sSGXInternalDevInfo.hHostCtlKernelMemInfoHandle;
		IMG_UINT32 ui32Force = (IMG_UINT32)psK->sSGXInternalDevInfo.bForcePTOff;
		IMG_UINT32 ui32Err   = (IMG_UINT32)psK->eError;
		IMG_UINT32 *p = pvBridgeOut;

		p[0] = ui32Flags; p[1] = ui32Host; p[2] = ui32Force; p[3] = ui32Err;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SGX_REGISTER_HW_TRANSFER_CONTEXT)
		 || ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_SGX_REGISTER_HW_RENDER_CONTEXT))
	{
		/* Both: kernel {eError@0; HANDLE hCtx@8; IMG_DEV_VIRTADDR devvaddr@16} 20 ->
		 * i686 {eError@0; hCtx@4; devvaddr@8} 12. Raw offsets (structs differ only in
		 * field names); read the handle+devaddr (kernel@8/@16) before writing @4/@8. */
		IMG_PBYTE pby = pvBridgeOut;
		IMG_UINT32 ui32Err = *(IMG_UINT32 *)(pby + 0);
		IMG_UINT32 ui32Ctx = (IMG_UINT32)(unsigned long)*(IMG_HANDLE *)(pby + 8);
		IMG_UINT32 ui32VA  = *(IMG_UINT32 *)(pby + 16);

		((IMG_UINT32 *)pby)[0] = ui32Err;
		((IMG_UINT32 *)pby)[1] = ui32Ctx;
		((IMG_UINT32 *)pby)[2] = ui32VA;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_OPEN_DISPCLASS_DEVICE))
	{
		/* kernel {eError@0; HANDLE hDeviceKM@8} 16 -> i686 {eError@0; hDeviceKM@4} 8. */
		PVRSRV_BRIDGE_OUT_OPEN_DISPCLASS_DEVICE *psK = pvBridgeOut;
		IMG_UINT32 ui32Err = (IMG_UINT32)psK->eError;
		IMG_UINT32 ui32Dev = (IMG_UINT32)(unsigned long)psK->hDeviceKM;
		IMG_UINT32 *p = pvBridgeOut;

		p[0] = ui32Err;
		p[1] = ui32Dev;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_GET_DISPCLASS_SYSBUFFER))
	{
		/* kernel {eError@0; HANDLE hBuffer@8} 16 -> i686 {eError@0; hBuffer@4} 8. */
		PVRSRV_BRIDGE_OUT_GET_DISPCLASS_SYSBUFFER *psK = pvBridgeOut;
		IMG_UINT32 ui32Err = (IMG_UINT32)psK->eError;
		IMG_UINT32 ui32Buf = (IMG_UINT32)(unsigned long)psK->hBuffer;
		IMG_UINT32 *p = pvBridgeOut;

		p[0] = ui32Err; p[1] = ui32Buf;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_CREATE_DISPCLASS_SWAPCHAIN))
	{
		/* kernel {eError@0; HANDLE hSwapChain@8; u32 SwapChainID@16} 20 -> i686
		 * {eError@0; hSwapChain@4; SwapChainID@8} 12. */
		PVRSRV_BRIDGE_OUT_CREATE_DISPCLASS_SWAPCHAIN *psK = pvBridgeOut;
		IMG_UINT32 ui32Err = (IMG_UINT32)psK->eError;
		IMG_UINT32 ui32SC  = (IMG_UINT32)(unsigned long)psK->hSwapChain;
		IMG_UINT32 ui32ID  = psK->ui32SwapChainID;
		IMG_UINT32 *p = pvBridgeOut;

		p[0] = ui32Err; p[1] = ui32SC; p[2] = ui32ID;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_GET_DISPCLASS_BUFFERS))
	{
		/* kernel {eError@0; u32 count@4; HANDLE ahBuffer[9]@8 (8 each)} 80 -> i686
		 * {eError@0; count@4; ahBuffer[9]@8 (4 each)} 44. Pack handles forward (dest
		 * below src), header read first. */
		PVRSRV_BRIDGE_OUT_GET_DISPCLASS_BUFFERS *psK = pvBridgeOut;
		IMG_PBYTE pby = pvBridgeOut;
		IMG_UINT32 ui32Err   = (IMG_UINT32)psK->eError;
		IMG_UINT32 ui32Count = psK->ui32BufferCount;
		IMG_UINT32 i;

		for (i = 0; i < PVRSRV_MAX_DC_SWAPCHAIN_BUFFERS; i++)
			((IMG_UINT32 *)(pby + 8))[i] = (IMG_UINT32)(unsigned long)psK->ahBuffer[i];
		((IMG_UINT32 *)pby)[0] = ui32Err;
		((IMG_UINT32 *)pby)[1] = ui32Count;
	}
	else if (ui32BridgeID == PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_WRAP_EXT_MEMORY))
	{
		/* kernel {eError@0; pad; CLIENT_MEM_INFO@8(88); CLIENT_SYNC_INFO@96(32)} 128 ->
		 * i686 {eError@0; CLIENT_MEM_INFO@4(48); CLIENT_SYNC_INFO@52(20)} 72. Helpers read
		 * their whole source before writing the lower dest; header read first. */
		PVRSRV_BRIDGE_OUT_WRAP_EXT_MEMORY *psK = pvBridgeOut;
		IMG_PBYTE pby = pvBridgeOut;
		IMG_UINT32 ui32Err = (IMG_UINT32)psK->eError;

		(void)PVRCompatPackClientMemInfo (pby + 4,  &psK->sClientMemInfo);
		(void)PVRCompatPackClientSyncInfo(pby + 4 + 48, &psK->sClientSyncInfo);
		((IMG_UINT32 *)pby)[0] = ui32Err;
	}
}
#endif /* CONFIG_COMPAT */

IMG_INT BridgedDispatchKM(PVRSRV_PER_PROCESS_DATA * psPerProc,
					  PVRSRV_BRIDGE_PACKAGE   * psBridgePackageKM)
{

	IMG_VOID   * psBridgeIn;
	IMG_VOID   * psBridgeOut;
	BridgeWrapperFunction pfBridgeHandler;
	PVRSRV_BRIDGE_DISPATCH_TABLE_ENTRY *dte;
	IMG_UINT32   ui32BridgeID = psBridgePackageKM->ui32BridgeID;
	IMG_INT      err          = -EFAULT;
	IMG_UINT32   ui32ExpectIn;
	IMG_UINT32   ui32ExpectOut;
#if defined(CONFIG_COMPAT)
	IMG_BOOL     bCompat      = IMG_FALSE;
#endif

	if(ui32BridgeID >= (BRIDGE_DISPATCH_TABLE_ENTRY_COUNT))
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: ui32BridgeID = %d is out if range!",
					__FUNCTION__, ui32BridgeID));
		goto return_fault;
	}

	dte = &g_BridgeDispatchTable[ui32BridgeID];

	/* The kernel struct sizes are the default expectation. A 32-bit caller sends
	 * the packed i686 variant, so a bridge with a compat descriptor expects the
	 * compat sizes instead (handle-free bridges keep the kernel sizes, which
	 * already match). */
	ui32ExpectIn  = dte->in_size;
	ui32ExpectOut = dte->out_size;
#if defined(CONFIG_COMPAT)
	if (in_compat_syscall())
	{
		bCompat = IMG_TRUE;
		(void)PVRCompatBridge(ui32BridgeID, &ui32ExpectIn, &ui32ExpectOut);
	}
#endif

#if defined(DEBUG_TRACE_BRIDGE_KM)
	PVR_DPF((PVR_DBG_ERROR, "%s: %s",
			 __FUNCTION__,
			 dte->pszIOCName));
#endif

#if defined(DEBUG_BRIDGE_KM)
	dte->ui32CallCount++;
	g_BridgeGlobalStats.ui32IOCTLCount++;
#endif

	if (psBridgePackageKM->ui32InBufferSize != ui32ExpectIn ||
			psBridgePackageKM->ui32OutBufferSize != ui32ExpectOut) {
		PVR_DPF((PVR_DBG_ERROR, "pvr: invalid param size for IOCTL#%d:\n"
					"     kern/user in,out: %d/%d,%d/%d\n",
					ui32BridgeID,
					ui32ExpectIn, psBridgePackageKM->ui32InBufferSize,
					ui32ExpectOut, psBridgePackageKM->ui32OutBufferSize));
		err = -EINVAL;
		goto return_fault;
	}

#if defined(CONFIG_COMPAT)
	if (bCompat)
		printk(KERN_INFO "pvr-compat: id=%u sizes-ok in=%u out=%u\n",
		       ui32BridgeID, ui32ExpectIn, ui32ExpectOut);
#endif

	if(!psPerProc->bInitProcess)
	{
		if(PVRSRVGetInitServerState(PVRSRV_INIT_SERVER_RAN))
		{
			if(!PVRSRVGetInitServerState(PVRSRV_INIT_SERVER_SUCCESSFUL))
			{
				PVR_DPF((PVR_DBG_ERROR, "%s: Initialisation failed.  Driver unusable.",
							__FUNCTION__));
				goto return_fault;
			}
		}
		else
		{
			if(PVRSRVGetInitServerState(PVRSRV_INIT_SERVER_RUNNING))
			{
				PVR_DPF((PVR_DBG_ERROR, "%s: Initialisation is in progress",
							__FUNCTION__));
				goto return_fault;
			}
			else
			{

				switch(ui32BridgeID)
				{
					case PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_CONNECT_SERVICES):
					case PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_DISCONNECT_SERVICES):
					case PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_INITSRV_CONNECT):
					case PVRSRV_GET_BRIDGE_ID(PVRSRV_BRIDGE_INITSRV_DISCONNECT):
						break;
					default:
						PVR_DPF((PVR_DBG_ERROR, "%s: Driver initialisation not completed yet.",
								 __FUNCTION__));
						goto return_fault;
				}
			}
		}
	}



#if defined(__linux__)
	{

		SYS_DATA *psSysData;

		SysAcquireData(&psSysData);


		psBridgeIn = ((ENV_DATA *)psSysData->pvEnvSpecificData)->pvBridgeData;
		psBridgeOut = (IMG_PVOID)((IMG_PBYTE)psBridgeIn + PVRSRV_MAX_BRIDGE_IN_SIZE);


		if((psBridgePackageKM->ui32InBufferSize > PVRSRV_MAX_BRIDGE_IN_SIZE) || 
				(psBridgePackageKM->ui32OutBufferSize > PVRSRV_MAX_BRIDGE_OUT_SIZE))
		{
			goto return_fault;
		}


		if(psBridgePackageKM->ui32InBufferSize > 0)
		{
			if(!OSAccessOK(PVR_VERIFY_READ,
							psBridgePackageKM->pvParamIn,
							psBridgePackageKM->ui32InBufferSize))
			{
				PVR_DPF((PVR_DBG_ERROR, "%s: Invalid pvParamIn pointer", __FUNCTION__));
			}

			if(CopyFromUserWrapper(psPerProc,
					               ui32BridgeID,
								   psBridgeIn,
								   psBridgePackageKM->pvParamIn,
								   psBridgePackageKM->ui32InBufferSize)
			  != PVRSRV_OK)
			{
				goto return_fault;
			}
		}
	}
#else
	psBridgeIn  = psBridgePackageKM->pvParamIn;
	psBridgeOut = psBridgePackageKM->pvParamOut;
#endif

#if defined(CONFIG_COMPAT)
	/* psBridgeIn now holds the i686 IN as sent; expand it to the kernel layout
	 * the handler expects (no-op for handle-free INs). */
	if (bCompat)
	{
		PVRCompatExpandIn(ui32BridgeID, psBridgeIn);
	}
#endif

#if defined(CONFIG_COMPAT)
	if (bCompat)
		printk(KERN_INFO "pvr-compat: id=%u pre-handler pfn=%p in=%p out=%p\n",
		       ui32BridgeID, dte->pfFunction, psBridgeIn, psBridgeOut);
#endif

	pfBridgeHandler = (BridgeWrapperFunction)dte->pfFunction;
	err = pfBridgeHandler(ui32BridgeID,
						  psBridgeIn,
						  psBridgeOut,
						  psPerProc);
	if(err < 0)
	{
		goto return_fault;
	}

#if defined(CONFIG_COMPAT)
	if (bCompat)
		printk(KERN_INFO "pvr-compat: id=%u post-handler err=%d\n",
		       ui32BridgeID, err);
#endif


#if defined(__linux__)

#if defined(CONFIG_COMPAT)
	/* Compact the kernel OUT down to the i686 layout before it goes back to the
	 * 32-bit caller; ui32OutBufferSize already carries the compat size. */
	if (bCompat)
	{
		PVRCompatCompactOut(ui32BridgeID, psBridgeOut);
	}
#endif

	if(CopyToUserWrapper(psPerProc,
						 ui32BridgeID,
						 psBridgePackageKM->pvParamOut,
						 psBridgeOut,
						 psBridgePackageKM->ui32OutBufferSize)
	   != PVRSRV_OK)
	{
		goto return_fault;
	}
#endif

	err = 0;
return_fault:

	ReleaseHandleBatch(psPerProc);

	if (err)
		PVR_DPF((PVR_DBG_ERROR, "pvr: ioctl#%d failed (%d)\n",
					ui32BridgeID, err));

	return err;
}

