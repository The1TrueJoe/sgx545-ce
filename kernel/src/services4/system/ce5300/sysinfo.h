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

#if !defined(__SYSINFO_H__)
#define __SYSINFO_H__

/* Longest the driver will spin waiting on hardware, in microseconds. */
#define MAX_HW_TIME_US				(500000)
#define WAIT_TRY_COUNT				(10000)

typedef enum _SYS_DEVICE_TYPE_
{
	SYS_DEVICE_SGX						= 0,
	SYS_DEVICE_FORCE_I16				= 0x7fff
} SYS_DEVICE_TYPE;

#define SYS_DEVICE_COUNT			3

#endif	/* __SYSINFO_H__ */
