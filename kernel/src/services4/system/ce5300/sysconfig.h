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

#if !defined(__SOCCONFIG_H__)
#define __SOCCONFIG_H__

#include "syscommon.h"

#define VS_PRODUCT_NAME		"SGX545 Intel CE5300"

/*
 * The power lock timeout exists to catch a wedged display driver on systems
 * where the display and the SGX share a power island. On CE5300 they do not
 * -- the VDC/VPU (01:08.x) is a separate PCI function with its own driver --
 * so nothing else can hold the lock and the timeout only creates false
 * failures under a debugger. Intel disabled it on Cedarview for the same
 * reason.
 */
#define SYS_NO_POWER_LOCK_TIMEOUT

/*
 * PCI identity. Intel Atom CE5310 (CE5300 family, "Berryville"):
 *
 *   01:02.0  8086:089b  VGA compatible controller  -- PowerVR SGX545  <- us
 *   01:08.0  8086:2e61  VDC   (display controller) -- ce5300-fb
 *   01:08.1  8086:2e62  VPU
 *   01:08.2  8086:2e63  HDMI transmitter
 *   01:16.0  8086:070a  Vivante GC300 2D engine    -- galcore
 *
 * Only 01:02.0 is ours. Everything else on that bus belongs to another
 * driver, which is the main structural difference from Cedarview, where the
 * SGX lived behind the same BAR as the display engine.
 */
#define SYS_SGX_DEV_VENDOR_ID		0x8086
#define SYS_SGX_DEV_DEVICE_ID		0x089b

#define SYS_SGX_DEV_NAME		"sgx545-ce"

/*
 * Register window.
 *
 * BAR0 on 01:02.0 is 16 MB (0xdc000000-0xdcffffff on a stock EA-3). The SGX
 * core registers sit at the very start of it, not at an offset -- this is the
 * one place CE5300 genuinely differs from every other Intel SGX part. On
 * Poulsbo/Moorestown/Cedarview the SGX shares the display BAR and lives at
 * +0x40000 or +0x80000; here it has its own function and starts at zero.
 *
 * Established by disassembling the stock Control4 pvrsrvkm.ko (DDK 1.12,
 * vermagic 3.12.74): its SysLocateDevices stores the return of
 * OSPCIAddrRangeStart(hSGXPCI, 0) straight into both sRegsSysPBase and
 * sRegsCpuPBase with no add in between, then writes 0x4000 to ui32RegsSize.
 * Cedarview's equivalent adds SGX_REGS_OFFSET at that point; the CE build has
 * no such instruction.
 *
 * Verify on hardware with tools/sgxregs.py before trusting this.
 */
#define SGX_REGS_OFFSET			0x00000
#define SGX_REG_SIZE			0x4000

#define SYS_SGX_ADDR_RANGE_INDEX	0		/* BAR0 */
#define SYS_SGX_MAX_OFFSET		(SGX_REGS_OFFSET + SGX_REG_SIZE)

/*
 * Interrupt routing. The SGX raises its own PCI interrupt on this part, so
 * unlike Poulsbo there is no shared SoC interrupt-identity register to
 * demultiplex -- SysGetInterruptSource returns 0 and the SGX device layer
 * reads EUR_CR_EVENT_STATUS itself. Kept as a bit so the DDK's external
 * device plumbing still compiles.
 */
#define DEVICE_SGX_INTERRUPT		(1<<0)
#define DEVICE_DISP_INTERRUPT		(1<<2)

/* Runtime half of FIX_HW_BRN_SAMPLE_CACHE; see sysconfig.c. */
extern bool need_sample_cache_workaround;

#endif	/* __SOCCONFIG_H__ */
