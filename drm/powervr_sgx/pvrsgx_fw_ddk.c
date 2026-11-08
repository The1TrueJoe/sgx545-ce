// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * The vendor (DDK 1.7.862890) microkernel, for A/B testing against the open
 * one: its fixed size, host-kick entries, entry programs and patch sites
 * (docs/SRVINIT.md). It needs every VM to map the firmware's heaps
 * (pvrsgx_vm.c), so it is no use where clients must be isolated, and the
 * whole file goes with CONFIG_DRM_POWERVR_SGX_DDK_COMPAT.
 */

#include <drm/drm_print.h>

#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "pvrsgx_ccb.h"
#include "pvrsgx_device.h"
#include "pvrsgx_fw.h"
#include "pvrsgx_pds.h"

/* libsrv_init's .rodata 0x2000..0xef68. */
#define DDK_UKERNEL_SIZE	0xcf68

MODULE_FIRMWARE("powervr_sgx/sgx545-ukernel-1.7.862890.bin");

/* Host-kick entries in instructions, per command type. */
static const u32 host_kick[PVRSGX_CMD_MAX] = {
	[PVRSGX_CMD_TA] = 0x11d,
	[PVRSGX_CMD_TRANSFER] = 0x103,
	[PVRSGX_CMD_POWER] = 0xd4,
	[PVRSGX_CMD_CONTEXTSUSPEND] = 0xeb,
	[PVRSGX_CMD_CLEANUP] = 0x13a,
	[PVRSGX_CMD_GETMISCINFO] = 0x173,
	[PVRSGX_CMD_PROCESS_QUEUES] = 0x156,
	[PVRSGX_CMD_SETHWPERFSTATUS] = 0x157,
};

/* pds1: if in0 == 0, DMA the control block into sa first (DOUTD 0x3c0003f); then enter at 0xdf0. */
static void pds_ta3d(struct pvrsgx_device *sdev)
{
	struct pvrsgx_pds p = pvrsgx_pds_prog(sdev, PVRSGX_FW_PDS1, 8);
	u32 off = sdev->fw.addr[PVRSGX_FW_UKERNEL] + 0xdf0 - PVRSGX_HEAP_KERNEL_CODE;

	pvrsgx_pds_ds(&p, PDS_D0(0), sdev->fw.addr[PVRSGX_FW_TA3DCTL]);
	pvrsgx_pds_ds(&p, PDS_D0(1), 0x3c0003f);
	/* No bit 27 and no segment subtracted here, unlike pvrsgx_doutu(). */
	pvrsgx_pds_ds(&p, PDS_D0(2), ((off * 2) & 0xffff0) | (((off >> 19) + 15) & 15));
	pvrsgx_pds_ds(&p, PDS_D0(3), 2);
	pvrsgx_pds_tst(&p, PDS_CC_ALWAYS, PDS_IN(0));
	pvrsgx_pds_bra(&p, PDS_CC_P0, 3);
	pvrsgx_pds_bra(&p, PDS_CC_ALWAYS, 4);
	pvrsgx_pds_movs(&p, PDS_DOUTD, PDS_D0(0), PDS_D0(1), PDS_D0(0), PDS_D0(0));
	pvrsgx_pds_movs(&p, PDS_DOUTU, PDS_D0(2), PDS_D0(3), PDS_D1(0), PDS_D0(2));
	pvrsgx_pds_halt(&p);
}

/*
 * pds3, the event dispatcher: in0, in2, in4 to PA0-PA2; event bits in in1 pick
 * the handler. Any of 0x100 -> 0x30. Otherwise the first of 0x400090, 0x200,
 * 0x300400 with a bit set selects 0x7050, 0x2cd0 or 0x5670, if no other bit is
 * set and in0 is 0. Anything else -> 0x30.
 */
static void pds_events(struct pvrsgx_device *sdev)
{
	static const struct { u32 mask, entry; } ev[3] = {
		{ 0x400090, 0x7050 }, { 0x200, 0x2cd0 }, { 0x300400, 0x5670 },
	};
	struct pvrsgx_pds p = pvrsgx_pds_prog(sdev, PVRSGX_FW_PDS3, 24);
	u32 uk = sdev->fw.addr[PVRSGX_FW_UKERNEL];

	pvrsgx_pds_ds(&p, PDS_D0(0), 2);
	pvrsgx_pds_ds(&p, PDS_D1(0), 0x000);
	pvrsgx_pds_ds(&p, PDS_D1(1), 0x100);
	pvrsgx_pds_ds(&p, PDS_D1(2), 0x200);
	pvrsgx_pds_ds(&p, PDS_D1(6), pvrsgx_doutu(uk + 0x30));
	pvrsgx_pds_ds(&p, PDS_D1(7), 0xffffff);
	pvrsgx_pds_ds(&p, PDS_D1(8), 0x100);
	for (int i = 0; i < 3; i++) {
		pvrsgx_pds_ds(&p, PDS_D1(3 + i), pvrsgx_doutu(uk + ev[i].entry));
		pvrsgx_pds_ds(&p, PDS_D1(9 + i), ev[i].mask);
	}
	pvrsgx_pds_movs(&p, PDS_DOUTA, PDS_IN(0), PDS_D1(0), PDS_IN(0), PDS_IN(0));
	pvrsgx_pds_movs(&p, PDS_DOUTA, PDS_IN(2), PDS_D1(1), PDS_IN(2), PDS_IN(2));
	pvrsgx_pds_movs(&p, PDS_DOUTA, PDS_IN(4), PDS_D1(2), PDS_IN(4), PDS_IN(4));
	pvrsgx_pds_alu(&p, PDS_CC_ALWAYS, PDS_ALU_AND, PDS_T0, PDS_IN(1), PDS_D1(7));
	pvrsgx_pds_alu(&p, PDS_CC_ALWAYS, PDS_ALU_AND, PDS_T1, PDS_T0, PDS_D1(8));
	pvrsgx_pds_tst(&p, PDS_CC_ALWAYS, PDS_T1);
	pvrsgx_pds_bra(&p, PDS_CC_P0, 8);
	pvrsgx_pds_bra(&p, PDS_CC_ALWAYS, 38);
	for (int i = 0; i < 3; i++) {
		unsigned int next = p.pc + 10;

		pvrsgx_pds_alu(&p, PDS_CC_ALWAYS, PDS_ALU_AND, PDS_T1, PDS_T0, PDS_D1(9 + i));
		pvrsgx_pds_tst(&p, PDS_CC_ALWAYS, PDS_T1);
		pvrsgx_pds_bra(&p, PDS_CC_P0, i < 2 ? next : 38);
		if (i == 0) {
			pvrsgx_pds_alu(&p, PDS_CC_ALWAYS, PDS_ALU_XOR, PDS_T1, PDS_T0, PDS_T1);
			pvrsgx_pds_tst(&p, PDS_CC_ALWAYS, PDS_T1);
			pvrsgx_pds_tst(&p, PDS_CC_P0, PDS_IN(0));
		} else {
			pvrsgx_pds_tst(&p, PDS_CC_ALWAYS, PDS_IN(0));
			pvrsgx_pds_alu(&p, PDS_CC_P0, PDS_ALU_XOR, PDS_T1, PDS_T0, PDS_T1);
			pvrsgx_pds_tst(&p, PDS_CC_P0, PDS_T1);
		}
		pvrsgx_pds_bra(&p, PDS_CC_P0, p.pc + 2);
		pvrsgx_pds_bra(&p, PDS_CC_ALWAYS, 38);
		pvrsgx_pds_movs(&p, PDS_DOUTU, PDS_D1(3 + i), PDS_D0(0), PDS_D0(1), PDS_D0(0));
		pvrsgx_pds_halt(&p);
	}
	pvrsgx_pds_movs(&p, PDS_DOUTU, PDS_D1(6), PDS_D0(0), PDS_D0(1), PDS_D0(0));
	pvrsgx_pds_halt(&p);
}

/* pds4: in0, in4 to PA0, PA2; in0 & 0x4000f -> 0x73a0, in0 & 0x120080 -> 0x2ea0, else 0x5740. */
static void pds_events2(struct pvrsgx_device *sdev)
{
	struct pvrsgx_pds p = pvrsgx_pds_prog(sdev, PVRSGX_FW_PDS4, 12);
	u32 uk = sdev->fw.addr[PVRSGX_FW_UKERNEL];

	pvrsgx_pds_ds(&p, PDS_D0(0), pvrsgx_doutu(uk + 0x73a0));
	pvrsgx_pds_ds(&p, PDS_D0(1), 2);
	pvrsgx_pds_ds(&p, PDS_D0(2), pvrsgx_doutu(uk + 0x2ea0));
	pvrsgx_pds_ds(&p, PDS_D0(3), 2);
	pvrsgx_pds_ds(&p, PDS_D0(4), pvrsgx_doutu(uk + 0x5740));
	pvrsgx_pds_ds(&p, PDS_D0(5), 2);
	pvrsgx_pds_ds(&p, PDS_D1(0), 0x000);
	pvrsgx_pds_ds(&p, PDS_D1(1), 0x200);
	pvrsgx_pds_ds(&p, PDS_D1(3), 0x4000f);
	pvrsgx_pds_ds(&p, PDS_D1(4), 0x120080);
	pvrsgx_pds_movs(&p, PDS_DOUTA, PDS_IN(0), PDS_D1(0), PDS_IN(0), PDS_IN(0));
	pvrsgx_pds_movs(&p, PDS_DOUTA, PDS_IN(4), PDS_D1(1), PDS_IN(4), PDS_IN(4));
	for (int i = 0; i < 2; i++) {
		pvrsgx_pds_alu(&p, PDS_CC_ALWAYS, PDS_ALU_AND, PDS_T0, PDS_IN(0), PDS_D1(3 + i));
		pvrsgx_pds_tst(&p, PDS_CC_ALWAYS, PDS_T0);
		pvrsgx_pds_bra(&p, PDS_CC_P0, p.pc + 3);
		pvrsgx_pds_movs(&p, PDS_DOUTU, PDS_D0(2 * i), PDS_D0(2 * i + 1), PDS_D1(2),
				PDS_D0(2 * i));
		pvrsgx_pds_halt(&p);
	}
	pvrsgx_pds_movs(&p, PDS_DOUTU, PDS_D0(4), PDS_D0(5), PDS_D1(2), PDS_D0(4));
	pvrsgx_pds_halt(&p);
}

int pvrsgx_fw_ddk_load(struct pvrsgx_device *sdev, const struct firmware *fw)
{
	struct pvrsgx_fw *f = &sdev->fw;

	if (fw->size != DDK_UKERNEL_SIZE) {
		drm_err(&sdev->base, "%s: %zu bytes, expected %u\n", f->name, fw->size,
			DDK_UKERNEL_SIZE);
		return -EINVAL;
	}
	f->image_size = DDK_UKERNEL_SIZE;
	f->image = kmemdup(fw->data, DDK_UKERNEL_SIZE, GFP_KERNEL);
	if (!f->image)
		return -ENOMEM;
	f->ddk = true;
	return 0;
}

/* Its entry programs and patches; the image is in place already. */
int pvrsgx_fw_ddk_build(struct pvrsgx_device *sdev)
{
	struct pvrsgx_fw *f = &sdev->fw;
	int err;

	pvrsgx_pds_entry(sdev, PVRSGX_FW_PDS0, 0xd60);	/* the event program */
	pds_ta3d(sdev);
	pvrsgx_pds_entry(sdev, PVRSGX_FW_PDS2, 0xe20);
	pds_events(sdev);
	pds_events2(sdev);

	err = pvrsgx_fw_limm(sdev, 0xbaf8, 0x1800100);
	err = err ?: pvrsgx_fw_limm(sdev, 0xbb10, f->addr[PVRSGX_FW_PDS4] >> 4);
	err = err ?: pvrsgx_fw_limm(sdev, 0x0d60, 0x1001000);
	err = err ?: pvrsgx_fw_limm(sdev, 0x0d70, f->addr[PVRSGX_FW_PDS1] >> 4);
	err = err ?: pvrsgx_fw_limm(sdev, 0x0d78, f->addr[PVRSGX_FW_TA3DCTL]);
	err = err ?: pvrsgx_fw_limm(sdev, 0x0db0, 0x800100);
	err = err ?: pvrsgx_fw_limm(sdev, 0x0dc0, f->addr[PVRSGX_FW_PDS2] >> 4);
	err = err ?: pvrsgx_fw_limm(sdev, 0x0e40, f->addr[PVRSGX_FW_PDS3] & ~0xfu);
	err = err ?: pvrsgx_fw_limm(sdev, 0x0e50, 6);
	if (err)
		return err;

	memcpy(f->host_kick, host_kick, sizeof(host_kick));
	return 0;
}
