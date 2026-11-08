// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

#include <drm/drm_file.h>

#include <linux/seq_buf.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <uapi/drm/powervr_sgx_drm.h>

#include "powervr_sgx_stream.h"
#include "pvrsgx_device.h"
#include "pvrsgx_fw.h"
#include "pvrsgx_job.h"
#include "pvrsgx_kobj.h"
#include "pvrsgx_mmu.h"
#include "pvrsgx_render.h"
#include "pvrsgx_transfer.h"
#include "pvrsgx_vm.h"

/*
 * A transfer is a 3D-only render: the background object covers the
 * destination, its pixel program samples the source and its end-of-tile
 * program writes the destination, all client memory (TEX.md 3, 10). The
 * kernel owns the HW transfer context, its command ring and the completion
 * sync, and writes the command from the client's CMD_TRANSFER stream.
 */
#define XFER_HWCTX_BYTES	0xb4
#define XFER_CCB_BYTES		0x104c8	/* 0x10000 ring + slack for a command that wraps */
#define XFER_CMD_BYTES		0x1c0
#define XFER_SHARED		0xac	/* the shared part of the command */
#define XFER_TILES_X_MAX	127	/* 4096 pixels: G14 has the real limit */
#define XFER_TILES_Y_MAX	255

int pvrsgx_transfer_ctx_init(struct pvrsgx_device *sdev, struct pvrsgx_context *ctx)
{
	struct pvrsgx_transfer_ctx *x = kzalloc_obj(*x);
	u32 *hw;
	int err;

	if (!x)
		return -ENOMEM;
	ctx->transfer = x;

	err = pvrsgx_kobj_new(sdev, &x->ccb, PVRSGX_KRANGE_KDATA, XFER_CCB_BYTES, 0, NULL);
	err = err ?: pvrsgx_kobj_new(sdev, &x->ccbctl, PVRSGX_KRANGE_KDATA, 8, 0, NULL);
	err = err ?: pvrsgx_kobj_new(sdev, &x->done, PVRSGX_KRANGE_SYNC, SYNC_DATA_BYTES, 0, NULL);
	err = err ?: pvrsgx_kobj_new(sdev, &x->hwctx, PVRSGX_KRANGE_KCODE, XFER_HWCTX_BYTES, 0,
				     NULL);
	if (err)
		goto fail;

	/* SGXCreateTransferContext's words; the rest stays zero. */
	hw = kobj_cpu(x->hwctx);
	hw[0] = 1;
	hw[2] = pvrsgx_mmu_pd_addr(ctx->vm->mmu);
	hw[3] = kobj_va(x->ccb);
	hw[4] = kobj_va(x->ccbctl);
	hw[0x2a] = task_tgid_nr(current);
	return 0;

fail:
	pvrsgx_transfer_ctx_fini(sdev, ctx);
	return err;
}

void pvrsgx_transfer_ctx_fini(struct pvrsgx_device *sdev, struct pvrsgx_context *ctx)
{
	struct pvrsgx_transfer_ctx *x = ctx->transfer;

	if (!x)
		return;
	pvrsgx_kobj_drop(sdev, &x->hwctx);
	pvrsgx_kobj_drop(sdev, &x->done);
	pvrsgx_kobj_drop(sdev, &x->ccbctl);
	pvrsgx_kobj_drop(sdev, &x->ccb);
	kfree(x);
	ctx->transfer = NULL;
}

int pvrsgx_transfer_job_init(struct drm_file *file, struct pvrsgx_job *job,
			     const struct drm_powervr_sgx_job *in)
{
	struct pvrsgx_transfer_job *tj = &job->transfer;
	struct powervr_sgx_stream_ctx sctx = {};
	struct pvrsgx_vm *vm = job->ctx->vm;
	int err;

	if (in->flags & ~DRM_POWERVR_SGX_SUBMIT_JOB_TRANSFER_FLAGS_MASK)
		return -EINVAL;
	if (!job->ctx->transfer || in->ta_kicks.count || in->rt_dataset_handle)
		return -EINVAL;
	tj->dst_twiddled = in->flags & DRM_POWERVR_SGX_SUBMIT_JOB_TRANSFER_DST_TWIDDLED;

	sctx.heaps[POWERVR_SGX_STREAM_HEAP_PDS_PIXEL].base = PVRSGX_HEAP_PDS_PIXEL;
	sctx.heaps[POWERVR_SGX_STREAM_HEAP_PDS_PIXEL].size =
		PVRSGX_HEAP_KERNEL_CODE - PVRSGX_HEAP_PDS_PIXEL;
	sctx.limits[POWERVR_SGX_STREAM_LIMIT_TILES_X] = XFER_TILES_X_MAX;
	sctx.limits[POWERVR_SGX_STREAM_LIMIT_TILES_Y] = XFER_TILES_Y_MAX;
	sctx.has_quirk = pvrsgx_stream_no_quirk;
	sctx.mapped = pvrsgx_stream_mapped;
	sctx.priv = vm;

	err = pvrsgx_stream_take(&powervr_sgx_stream_transfer_defs, &sctx, vm, in->cmd_stream,
				 in->cmd_stream_len, &tj->xfer);
	if (err)
		return err;
	return powervr_sgx_check_transfer(&tj->xfer);
}

/* Write the 0x1c0-byte transfer command into the context's ring (UAPI.md 3.5). */
void pvrsgx_transfer_job_build(struct pvrsgx_job *job, u32 data[6])
{
	struct pvrsgx_transfer_ctx *x = job->ctx->transfer;
	const struct powervr_sgx_stream_transfer *in = &job->transfer.xfer;
	u32 woff = x->ccb_woff, base = (u32)in->bg_block_addr & 0xfff00000;
	u32 *cmd = (u32 *)((u8 *)kobj_cpu(x->ccb) + woff), *sh, *done;

	job->transfer.cmd_off = woff;
	memset(cmd, 0, XFER_CMD_BYTES);
	cmd[0] = 1u << 25 | ((u32)in->bg_block_addr - base) >> 4;
	cmd[1] = 0x200;
	cmd[2] = in->bg_depth;
	cmd[3] = 3;
	cmd[5] = 0x100;
	cmd[6] = base;
	cmd[7] = 0x80000000;
	cmd[8] = 0x88;
	cmd[9] = (u32)in->pixel_event_pds_addr;
	cmd[10] = in->pixel_event_pds_rows;
	cmd[11] = 1;
	cmd[12] = in->first_tile_x << 16 | in->first_tile_y;
	cmd[13] = in->last_tile_x << 16 | in->last_tile_y;
	cmd[14] = 0x80;
	cmd[15] = PVRSGX_HEAP_PDS_PIXEL;	/* EUR_CR_PDS_EXEC_BASE: never the client's */
	cmd[0xa4 / 4] = XFER_CMD_BYTES;
	cmd[0xa8 / 4] = job->transfer.dst_twiddled ? 8 : 0;
	cmd[0x194 / 4] = 0x07c00000;

	/* No source syncs; one destination, the kernel's completion sync. */
	sh = cmd + XFER_SHARED / 4;
	done = kobj_cpu(x->done);
	sh[0x84 / 4] = 1;			/* ui32NumDstSyncs */
	sh[0x88 / 4] = done[SYNC_READ_PENDING];
	sh[0x8c / 4] = kobj_va(x->done) + SYNC_READ_COMPLETE * 4;
	sh[0x90 / 4] = done[SYNC_WRITE_PENDING]++;
	sh[0x94 / 4] = kobj_va(x->done) + SYNC_WRITE_COMPLETE * 4;
	/* The microkernel sets write-complete to the pending value plus one. */
	job->done_cpu = &done[SYNC_WRITE_COMPLETE];
	job->done_val = sh[0x90 / 4] + 1;

	x->ccb_woff = (woff + XFER_CMD_BYTES) & (CCB_RING - 1);
	/* The command is in memory before the write offset that hands it over. */
	wmb();
	kobj_cpu(x->ccbctl)[0] = x->ccb_woff;

	memset(data, 0, 6 * sizeof(u32));
	data[1] = kobj_va(x->hwctx);
}

/* The transfer command and the structures it names, for a devcoredump. */
void pvrsgx_transfer_dump(struct seq_buf *sb, struct pvrsgx_job *job)
{
	struct pvrsgx_transfer_ctx *x = job->ctx->transfer;

	if (!x)
		return;
	pvrsgx_kobj_dump(sb, "xfer_hwctx", x->hwctx, 0, XFER_HWCTX_BYTES);
	pvrsgx_kobj_dump(sb, "xfer_ccbctl", x->ccbctl, 0, 8);
	pvrsgx_kobj_dump(sb, "xfer_cmd", x->ccb, job->transfer.cmd_off, XFER_CMD_BYTES);
	pvrsgx_kobj_dump(sb, "xfer_done", x->done, 0, SYNC_DATA_BYTES);
}
