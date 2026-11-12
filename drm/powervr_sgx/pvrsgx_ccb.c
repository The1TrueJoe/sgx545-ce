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

int pvrsgx_ccb_send(struct pvrsgx_device *sdev, enum pvrsgx_cmd type,
		    const u32 data[6], u64 *seq)
{
	u32 *ctl = ccbctl(sdev), *cmd, wo;
	unsigned long timeout = jiffies + msecs_to_jiffies(500);

	/* The DDK image has no entry for some types; the open one's table is the identity. */
	if (type >= PVRSGX_CMD_MAX)
		return -EINVAL;

	mutex_lock(&sdev->ccb_lock);
	wo = ctl[CCB_WO];
	while (((wo + 1) & (PVRSGX_CCB_SLOTS - 1)) ==
	       (READ_ONCE(ctl[CCB_RO]) & (PVRSGX_CCB_SLOTS - 1))) {
		if (time_after(jiffies, timeout)) {
			mutex_unlock(&sdev->ccb_lock);
			drm_err(&sdev->base, "kernel CCB full (wo %u ro %u)\n", wo, ctl[CCB_RO]);
			return -EBUSY;
		}
		usleep_range(500, 1000);
	}

	cmd = (u32 *)sdev->fw.cpu[PVRSGX_FW_CCB] + wo * PVRSGX_CCB_CMD_WORDS;
	cmd[0] = type;
	cmd[1] = sdev->fw.cache_control;
	memcpy(&cmd[2], data, 6 * sizeof(u32));
	sdev->fw.cache_control = 0;

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
	mutex_unlock(&sdev->ccb_lock);
	return 0;
}

/*
 * Unmapped is not unreachable: the BIF may keep a translation until it is
 * invalidated, and the microkernel invalidates only for a command it takes,
 * before it moves the read offset on. So what an unmap freed waits for that
 * command: the next one sent, since cache_control rides on it. After the GPU
 * is lost (stopped for good) nothing can translate any more.
 */
void pvrsgx_ccb_after_invalidate(struct pvrsgx_device *sdev, struct pvrsgx_inval_wait *w,
				 u32 cache_control)
{
	unsigned long flags;

	if (READ_ONCE(sdev->lost)) {
		w->done(w);
		return;
	}
	mutex_lock(&sdev->ccb_lock);
	sdev->fw.cache_control |= cache_control;
	w->seq = sdev->ccb_issued + 1;
	mutex_unlock(&sdev->ccb_lock);
	spin_lock_irqsave(&sdev->inval_lock, flags);
	list_add_tail(&w->node, &sdev->inval_waits);
	spin_unlock_irqrestore(&sdev->inval_lock, flags);
}

/*
 * Run what waited for an invalidate the microkernel has now taken; all of it
 * once the GPU is stopped. A restart empties the ring, so everything sent
 * before it counts as taken: the GPU was reset, and the first command after
 * the start carries an invalidate of its own.
 */
void pvrsgx_ccb_inval_reap(struct pvrsgx_device *sdev, bool all)
{
	struct pvrsgx_inval_wait *w, *tmp;
	unsigned long flags;
	LIST_HEAD(done);
	u64 consumed;

	spin_lock_irqsave(&sdev->inval_lock, flags);
	consumed = all || READ_ONCE(sdev->lost) ? U64_MAX : pvrsgx_ccb_consumed(sdev);
	list_for_each_entry_safe(w, tmp, &sdev->inval_waits, node) {
		if (w->seq <= consumed)
			list_move_tail(&w->node, &done);
	}
	spin_unlock_irqrestore(&sdev->inval_lock, flags);
	list_for_each_entry_safe(w, tmp, &done, node) {
		list_del(&w->node);
		w->done(w);
	}
}
