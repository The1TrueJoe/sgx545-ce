/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_RENDER_H
#define PVRSGX_RENDER_H

#include <linux/kref.h>
#include <linux/types.h>

#include <uapi/drm/powervr_sgx_drm.h>

#include "powervr_sgx_stream_defs.h"

struct drm_device;
struct drm_file;
struct pvrsgx_context;
struct pvrsgx_device;
struct pvrsgx_file;
struct pvrsgx_job;
struct pvrsgx_kobj;

/* The firmware half of a render context (STATE.md section 6). */
/* A VM's parameter buffer, shared by its render contexts (pvrsgx_render.c). */
struct pvrsgx_pb {
	struct kref ref;
	struct pvrsgx_device *sdev;
	struct pvrsgx_kobj *buf, *freelist, *desc, *blocks;
	u32 pages;
	/** @base: The 3D request base, PB & 0xfff00000. */
	u32 base;
	/** @fw_gen: The microkernel run (pvrsgx_device.fw_gen) it was last set up for. */
	u32 fw_gen;
};

struct pvrsgx_render_ctx {
	struct pvrsgx_kobj *hwctx, *ccb, *ccbctl, *scratch, *sync3d;
	struct pvrsgx_pb *pb;
	u32 ccb_woff;
};

void pvrsgx_pb_put(struct pvrsgx_pb *pb);

/* SGXAddRenderTarget's numbers (STATE.md section 6). */
struct pvrsgx_rt_numbers {
	u32 xt, yt, mx, my, padx, pady;
	u32 mtile1, mtile2, te_screen, mt_tiles, region_bytes, tailptr_bytes;
};

struct pvrsgx_rt_dataset {
	struct kref ref;
	struct pvrsgx_context *ctx;
	u32 w, h;
	struct pvrsgx_rt_numbers n;
	struct pvrsgx_kobj *rd, *rd_w8, *rd_w4, *hwsync, *rtword, *rtdata, *entword[2];
	struct pvrsgx_kobj *rd_sync, *hwsync_access, *done;
	struct pvrsgx_kobj *tailptr, *ta40, *ta50, *rgn[2], *ta88[2], *bgblock;
	/** @frames: Renders into this dataset so far. */
	u32 frames;
};

#define PVRSGX_MAX_TA_KICKS	2

/* What a render job carries from SUBMIT_JOBS to run_job. */
struct pvrsgx_render_job {
	struct pvrsgx_rt_dataset *rtds;
	struct powervr_sgx_stream_render render;
	struct powervr_sgx_stream_ta_kick kick[PVRSGX_MAX_TA_KICKS];
	u32 nkicks;
	/** @cmd_off: Where the last kick's TA command went in the context's ring. */
	u32 cmd_off;
};

/* A transfer context's firmware half: HW transfer context, command ring, completion sync. */
struct pvrsgx_transfer_ctx {
	struct pvrsgx_kobj *hwctx, *ccb, *ccbctl, *done;
	u32 ccb_woff;
};

struct pvrsgx_transfer_job {
	struct powervr_sgx_stream_transfer xfer;
	bool dst_twiddled;
	u32 cmd_off;
};

int pvrsgx_render_ctx_init(struct pvrsgx_device *sdev, struct pvrsgx_context *ctx, u32 pb_pages);
void pvrsgx_render_ctx_fini(struct pvrsgx_device *sdev, struct pvrsgx_context *ctx);

void pvrsgx_rtds_put(struct pvrsgx_rt_dataset *rtds);
void pvrsgx_rtds_destroy_all(struct pvrsgx_file *sfile);
int pvrsgx_ioctl_create_rt_dataset(struct drm_device *drm, void *data, struct drm_file *file);
int pvrsgx_ioctl_destroy_rt_dataset(struct drm_device *drm, void *data, struct drm_file *file);

int pvrsgx_render_job_init(struct drm_file *file, struct pvrsgx_job *job,
			   const struct drm_powervr_sgx_job *in);
void pvrsgx_render_job_fini(struct pvrsgx_job *job);

int pvrsgx_transfer_ctx_init(struct pvrsgx_device *sdev, struct pvrsgx_context *ctx);
void pvrsgx_transfer_ctx_fini(struct pvrsgx_device *sdev, struct pvrsgx_context *ctx);
int pvrsgx_transfer_job_init(struct drm_file *file, struct pvrsgx_job *job,
			     const struct drm_powervr_sgx_job *in);
void pvrsgx_transfer_job_build(struct pvrsgx_job *job, u32 data[6]);
int pvrsgx_render_job_build(struct pvrsgx_job *job, u32 k, u32 data[6]);
struct seq_buf;
void pvrsgx_job_dump(struct seq_buf *sb, struct pvrsgx_job *job);

#endif /* PVRSGX_RENDER_H */
