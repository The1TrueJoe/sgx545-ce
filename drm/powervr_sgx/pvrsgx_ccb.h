/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_CCB_H
#define PVRSGX_CCB_H

#include <linux/types.h>

struct pvrsgx_device;

/* SGXMKIF command types (UKERNEL.md section 3). */
enum pvrsgx_cmd {
	PVRSGX_CMD_TA = 0,
	PVRSGX_CMD_TRANSFER = 1,
	PVRSGX_CMD_2D = 2,
	PVRSGX_CMD_POWER = 3,
	PVRSGX_CMD_CONTEXTSUSPEND = 4,
	PVRSGX_CMD_CLEANUP = 5,
	PVRSGX_CMD_GETMISCINFO = 6,
	PVRSGX_CMD_PROCESS_QUEUES = 7,
	PVRSGX_CMD_SETHWPERFSTATUS = 9,
	PVRSGX_CMD_FLUSHPDCACHE = 10,
	PVRSGX_CMD_MAX = 11
};

/* ui32CacheControl: invalidates the microkernel runs before the command. */
#define PVRSGX_CC_INVAL_BIF_PT	0x1
#define PVRSGX_CC_INVAL_BIF_PD	0x2
#define PVRSGX_CC_INVAL_BIF_SL	0x4
#define PVRSGX_CC_INVAL_DATA	0x8

/* SGXMKIF_COMMAND: service address, cache control, six data words. */
#define PVRSGX_CCB_SLOTS	256
#define PVRSGX_CCB_CMD_WORDS	8

int pvrsgx_ccb_send(struct pvrsgx_device *sdev, enum pvrsgx_cmd type,
		    const u32 data[6], u64 *seq);
u64 pvrsgx_ccb_consumed(struct pvrsgx_device *sdev);
void pvrsgx_ccb_reset(struct pvrsgx_device *sdev);

#endif /* PVRSGX_CCB_H */
