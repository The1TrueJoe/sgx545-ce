/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_CCB_H
#define PVRSGX_CCB_H

#include <linux/list.h>
#include <linux/types.h>

struct pvrsgx_device;

/**
 * struct pvrsgx_inval_wait - Work that waits for a BIF invalidate to run
 * @node: In pvrsgx_device.inval_waits.
 * @seq: The kernel CCB command that carries the invalidate.
 * @done: Called, in process context, once the microkernel has taken it.
 */
struct pvrsgx_inval_wait {
	struct list_head node;
	u64 seq;
	void (*done)(struct pvrsgx_inval_wait *w);
};

/* Kernel CCB command types. */
enum pvrsgx_kcmd {
	PVRSGX_KCMD_TA = 0,
	PVRSGX_KCMD_TRANSFER = 1,
	PVRSGX_KCMD_POWER = 3,
	PVRSGX_KCMD_PROCESS_QUEUES = 7,
	PVRSGX_KCMD_MAX = 11
};

/* Cache control: invalidates the microkernel runs before the command. */
#define PVRSGX_CC_INVAL_BIF_PT	0x1
#define PVRSGX_CC_INVAL_BIF_PD	0x2
#define PVRSGX_CC_INVAL_BIF_SL	0x4
#define PVRSGX_CC_INVAL_DATA	0x8

/* A kernel CCB command: entry, cache control, six data words. */
#define PVRSGX_CCB_SLOTS	256
#define PVRSGX_CCB_CMD_WORDS	8

int pvrsgx_ccb_send(struct pvrsgx_device *sdev, enum pvrsgx_kcmd type,
		    const u32 data[6], u64 *seq);
u64 pvrsgx_ccb_consumed(struct pvrsgx_device *sdev);
void pvrsgx_ccb_reset(struct pvrsgx_device *sdev);
void pvrsgx_ccb_after_invalidate(struct pvrsgx_device *sdev, struct pvrsgx_inval_wait *w,
				 u32 cache_control);
void pvrsgx_ccb_inval_reap(struct pvrsgx_device *sdev, bool all);
u32 pvrsgx_ccb_inval_pending(struct pvrsgx_device *sdev);

#endif /* PVRSGX_CCB_H */
