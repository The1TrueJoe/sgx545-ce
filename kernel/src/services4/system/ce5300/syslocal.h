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
 * Contact Information:
 * Imagination Technologies Ltd. <gpl-support@imgtec.com>
 *
 ******************************************************************************/

#if !defined(__SYSLOCAL_H__)
#define __SYSLOCAL_H__

#include <linux/pci.h>

/*
 * Teardown ledger. SysDeinitialise runs unconditionally on any init failure,
 * so every acquisition records itself here and every release checks. Derived
 * from Intel's Cedarview layer with the MSVDX, host-port and power-management
 * bits dropped: on CE5300 the SGX is its own PCI function, the video decoder
 * is a separate driver entirely, and there is no host port.
 */
#define SYS_SPECIFIC_DATA_PCI_ACQUIRE_DEV			0x00000001
#define SYS_SPECIFIC_DATA_PCI_REQUEST_SGX_ADDR_RANGE		0x00000002
#define SYS_SPECIFIC_DATA_SGX_INITIALISED			0x00000040
#define SYS_SPECIFIC_DATA_MISR_INSTALLED			0x00000100
#define SYS_SPECIFIC_DATA_LISR_INSTALLED			0x00000200
#define SYS_SPECIFIC_DATA_PDUMP_INIT				0x00000400
#define SYS_SPECIFIC_DATA_IRQ_ENABLED				0x00000800
#define SYS_SPECIFIC_DATA_PM_UNMAP_SGX_REGS			0x00001000

#define SYS_SPECIFIC_DATA_SET(psSysSpecData, flag) \
	((IMG_VOID)((psSysSpecData)->ui32SysSpecificData |= (flag)))
#define SYS_SPECIFIC_DATA_CLEAR(psSysSpecData, flag) \
	((IMG_VOID)((psSysSpecData)->ui32SysSpecificData &= ~(flag)))
#define SYS_SPECIFIC_DATA_TEST(psSysSpecData, flag) \
	(((psSysSpecData)->ui32SysSpecificData & (flag)) != 0)

typedef struct _SYS_SPECIFIC_DATA_TAG_
{
	IMG_UINT32				ui32SysSpecificData;
	PVRSRV_PCI_DEV_HANDLE	hSGXPCI;
	struct pci_dev			*psPCIDev;
} SYS_SPECIFIC_DATA;

#endif	/* __SYSLOCAL_H__ */
