// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * The kernel CCB: a ring of 256 commands the host writes and the microkernel
 * reads (UKERNEL.md section 3). The host advances the write offset and rings
 * EUR_CR_EVENT_KICK; the microkernel advances the read offset as it takes each
 * command. Neither raises an interrupt by itself, so a command is complete, as
 * far as the ring is concerned, once the read offset has passed it.
 */

#include <drm/drm_print.h>

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/string.h>

#include "pvrsgx_ccb.h"
#include "pvrsgx_device.h"

#define CCB_WO 0
#define CCB_RO 1

static u32 *ccbctl(struct pvrsgx_device *sdev)
{
	return sdev->fw.cpu[PVRSGX_FW_CCBCTL];
}

void pvrsgx_ccb_reset(struct pvrsgx_device *sdev)
{
	memset(sdev->fw.cpu[PVRSGX_FW_CCB], 0, PVRSGX_CCB_SLOTS * PVRSGX_CCB_CMD_WORDS * 4);
	ccbctl(sdev)[CCB_WO] = 0;
	ccbctl(sdev)[CCB_RO] = 0;
	*(u32 *)sdev->fw.cpu[PVRSGX_FW_KICKER] = 0;
	sdev->ccb_issued = 0;
}

/* Commands the microkernel has taken off the ring since it started. */
u64 pvrsgx_ccb_consumed(struct pvrsgx_device *sdev)
{
	u32 ro = READ_ONCE(ccbctl(sdev)[CCB_RO]);
	unsigned long flags;
	u64 consumed;

	/* The read offset is GPU-written: trust it only as an index. */
	spin_lock_irqsave(&sdev->ccb_spin, flags);
	consumed = sdev->ccb_issued - ((ccbctl(sdev)[CCB_WO] - ro) & (PVRSGX_CCB_SLOTS - 1));
	spin_unlock_irqrestore(&sdev->ccb_spin, flags);
	return consumed;
}

/* Write one command and ring the doorbell, with ccb_lock held. */
static int ccb_write(struct pvrsgx_device *sdev, enum pvrsgx_cmd type, u32 cache_control,
		     const u32 data[6], u64 *seq)
{
	u32 *ctl = ccbctl(sdev), *cmd, wo;
	unsigned long timeout = jiffies + msecs_to_jiffies(500);

	wo = ctl[CCB_WO];
	while (((wo + 1) & (PVRSGX_CCB_SLOTS - 1)) ==
	       (READ_ONCE(ctl[CCB_RO]) & (PVRSGX_CCB_SLOTS - 1))) {
		if (time_after(jiffies, timeout)) {
			drm_err(&sdev->base, "kernel CCB full (wo %u ro %u)\n", wo, ctl[CCB_RO]);
			return -EBUSY;
		}
		usleep_range(500, 1000);
	}

	cmd = (u32 *)sdev->fw.cpu[PVRSGX_FW_CCB] + wo * PVRSGX_CCB_CMD_WORDS;
	cmd[0] = type;
	cmd[1] = cache_control;
	memcpy(&cmd[2], data, 6 * sizeof(u32));

	/* Command, then offset, then doorbell. */
	wmb();
	spin_lock_irq(&sdev->ccb_spin);
	ctl[CCB_WO] = (wo + 1) & (PVRSGX_CCB_SLOTS - 1);
	sdev->ccb_issued++;
	if (seq)
		*seq = sdev->ccb_issued;
	spin_unlock_irq(&sdev->ccb_spin);
	*(u32 *)sdev->fw.cpu[PVRSGX_FW_KICKER] =
		(*(u32 *)sdev->fw.cpu[PVRSGX_FW_KICKER] + 1) & 0xff;
	/* The command and the kicker count reach memory before the kick. */
	wmb();
	sgx_wr(sdev, SGX_CR_EVENT_KICK, SGX_CR_EVENT_KICK_NOW);
	return 0;
}

/*
 * BRN_28889: a BIF directory or table invalidate and a data invalidate never
 * go in one command. The BIF part goes first, alone, and the microkernel must
 * have taken it before the data part is sent.
 */
static int ccb_split_inval(struct pvrsgx_device *sdev, u32 *cache_control)
{
	static const u32 none[6];
	unsigned long timeout = jiffies + msecs_to_jiffies(500);
	u64 seq;
	int err;

	if (!(sdev->family->quirks & PVRSGX_QUIRK_BRN_28889) ||
	    !(*cache_control & PVRSGX_CC_INVAL_DATA) ||
	    !(*cache_control & ~PVRSGX_CC_INVAL_DATA))
		return 0;
	err = ccb_write(sdev, PVRSGX_CMD_PROCESS_QUEUES, *cache_control & ~PVRSGX_CC_INVAL_DATA,
			none, &seq);
	if (err)
		return err;
	while (pvrsgx_ccb_consumed(sdev) < seq) {
		if (time_after(jiffies, timeout)) {
			drm_err(&sdev->base, "BIF invalidate not taken (BRN_28889)\n");
			return -ETIMEDOUT;
		}
		usleep_range(100, 200);
	}
	*cache_control = PVRSGX_CC_INVAL_DATA;
	return 0;
}

int pvrsgx_ccb_send(struct pvrsgx_device *sdev, enum pvrsgx_cmd type,
		    const u32 data[6], u64 *seq)
{
	u32 cache_control;
	int err;

	/* The DDK image has no entry for some types; the open one's table is the identity. */
	if (type >= PVRSGX_CMD_MAX)
		return -EINVAL;

	mutex_lock(&sdev->ccb_lock);
	cache_control = sdev->fw.cache_control;
	err = ccb_split_inval(sdev, &cache_control);
	if (!err)
		err = ccb_write(sdev, type, cache_control, data, seq);
	if (!err)
		sdev->fw.cache_control = 0;
	mutex_unlock(&sdev->ccb_lock);
	return err;
}
