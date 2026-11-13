// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * The kernel CCB: a ring of 256 commands the host writes and the microkernel
 * reads. The host advances the write offset and rings
 * EUR_CR_EVENT_KICK; the microkernel advances the read offset as it takes each
 * command. Neither raises an interrupt by itself, so a command is complete, as
 * far as the ring is concerned, once the read offset has passed it.
 */

#include <drm/drm_print.h>

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
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
	spin_lock_irq(&sdev->ccb_spin);
	ccbctl(sdev)[CCB_WO] = 0;
	ccbctl(sdev)[CCB_RO] = 0;
	sdev->ccb_reset_seq = sdev->ccb_issued;
	spin_unlock_irq(&sdev->ccb_spin);
	*(u32 *)sdev->fw.cpu[PVRSGX_FW_KICKER] = 0;
	/*
	 * @ccb_issued carries on: a job sent after a restart has a higher sequence
	 * than any count read before it, so a stale count can't complete it.
	 */
}

/* Commands the microkernel has taken off the ring since the driver loaded. */
u64 pvrsgx_ccb_consumed(struct pvrsgx_device *sdev)
{
	unsigned long flags;
	u64 pending;
	u64 consumed;

	spin_lock_irqsave(&sdev->ccb_spin, flags);
	/*
	 * The read offset is GPU-written: trust it only as an index, read with
	 * the write offset and @ccb_issued held, so the distance is the ring's.
	 * Never more than sent since the ring's reset: all before it is taken.
	 */
	pending = (ccbctl(sdev)[CCB_WO] - READ_ONCE(ccbctl(sdev)[CCB_RO])) &
		  (PVRSGX_CCB_SLOTS - 1);
	pending = min(pending, sdev->ccb_issued - sdev->ccb_reset_seq);
	consumed = sdev->ccb_issued - pending;
	spin_unlock_irqrestore(&sdev->ccb_spin, flags);
	return consumed;
}

/* Write one command and ring the doorbell, with ccb_lock held. */
static int ccb_write(struct pvrsgx_device *sdev, enum pvrsgx_kcmd type, u32 cache_control,
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
 * have taken it before the data part is sent. An invalidate waiter
 * (pvrsgx_ccb_after_invalidate()) waits for the first, which carries the BIF
 * part.
 */
static int ccb_split_inval(struct pvrsgx_device *sdev, u32 *cache_control)
{
	static const u32 none[6];
	u64 seq, c;
	int err;

	if (!(sdev->family->quirks & PVRSGX_QUIRK_BRN_28889) ||
	    !(*cache_control & PVRSGX_CC_INVAL_DATA) ||
	    !(*cache_control & ~PVRSGX_CC_INVAL_DATA))
		return 0;
	err = ccb_write(sdev, PVRSGX_KCMD_PROCESS_QUEUES, *cache_control & ~PVRSGX_CC_INVAL_DATA,
			none, &seq);
	if (err)
		return err;
	/* Not a job, but a counting microkernel counts it (pvrsgx_job_process_done()). */
	WRITE_ONCE(sdev->noops_sent, sdev->noops_sent + 1);
	/* The time starts once it is sent, and the count is read again at the end. */
	err = read_poll_timeout(pvrsgx_ccb_consumed, c, c >= seq, 100, 500 * USEC_PER_MSEC,
				false, sdev);
	if (err) {
		drm_err(&sdev->base, "BIF invalidate not taken (BRN_28889)\n");
		return err;
	}
	*cache_control = PVRSGX_CC_INVAL_DATA;
	return 0;
}

int pvrsgx_ccb_send(struct pvrsgx_device *sdev, enum pvrsgx_kcmd type,
		    const u32 data[6], u64 *seq)
{
	u32 cache_control;
	int err;

	/* Types past the microkernel's command table are refused. */
	if (type >= PVRSGX_KCMD_MAX)
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

void pvrsgx_ccb_after_invalidate(struct pvrsgx_device *sdev, struct pvrsgx_inval_wait *w,
				 u32 cache_control)
{
	unsigned long flags;
	bool lost;

	/*
	 * Queued with its sequence, before the command that carries it can be
	 * taken; @lost read under the lock unbind's last reap takes after it.
	 */
	mutex_lock(&sdev->ccb_lock);
	spin_lock_irqsave(&sdev->inval_lock, flags);
	lost = READ_ONCE(sdev->lost);
	if (!lost) {
		sdev->fw.cache_control |= cache_control;
		w->seq = sdev->ccb_issued + 1;
		list_add_tail(&w->node, &sdev->inval_waits);
	}
	spin_unlock_irqrestore(&sdev->inval_lock, flags);
	mutex_unlock(&sdev->ccb_lock);
	if (lost)
		w->done(w);
}

/* For status dumps: how much work still waits for an invalidate. */
u32 pvrsgx_ccb_inval_pending(struct pvrsgx_device *sdev)
{
	struct pvrsgx_inval_wait *w;
	unsigned long flags;
	u32 n = 0;

	spin_lock_irqsave(&sdev->inval_lock, flags);
	list_for_each_entry(w, &sdev->inval_waits, node)
		n++;
	spin_unlock_irqrestore(&sdev->inval_lock, flags);
	return n;
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

#if IS_ENABLED(CONFIG_DRM_POWERVR_SGX_KUNIT_TEST)
#include "tests/pvrsgx_ccb_test.c"
#endif
