/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_RENDER_H
#define PVRSGX_RENDER_H

#include <linux/kref.h>
#include <linux/types.h>

#include <uapi/drm/powervr_sgx_drm.h>

#include "pvrsgx_stream_defs.h"

struct drm_device;
struct drm_file;
struct pvrsgx_context;
struct pvrsgx_device;
struct pvrsgx_file;
struct pvrsgx_job;
struct pvrsgx_kobj;
struct pvrsgx_vm;

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
	/** @awake: Holds a runtime PM reference for the context's life. */
	bool awake;
};

/* 16 MiB: the device's one PB holds every client's scenes (pvrsgx_render.c). */
#define PVRSGX_PB_DEFAULT_PAGES	0x1000

void pvrsgx_pb_put(struct pvrsgx_pb *pb);
void pvrsgx_pb_unmap(struct pvrsgx_vm *vm);

/* A render target's tiling numbers (docs/STATE.md section 6). */
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

/* Each context's command ring. */
#define CCB_RING		0x10000

/* A firmware sync object: pending and complete counts for writes and reads. */
#define SYNC_WRITE_PENDING	0
#define SYNC_WRITE_COMPLETE	1
#define SYNC_READ_PENDING	2
#define SYNC_READ_COMPLETE	3
#define SYNC_DATA_BYTES		0x18

#define PVRSGX_MAX_TA_KICKS	2

/* What a render job carries from SUBMIT_JOBS to run_job. */
struct pvrsgx_render_job {
	struct pvrsgx_rt_dataset *rtds;
	struct pvrsgx_stream_render render;
	struct pvrsgx_stream_ta_kick kick[PVRSGX_MAX_TA_KICKS];
	u32 nkicks;
	/** @cmd_off: Where the last kick's TA command went in the context's ring. */
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

int pvrsgx_render_job_build(struct pvrsgx_job *job, u32 k, u32 data[6]);
struct seq_buf;
void pvrsgx_job_dump(struct seq_buf *sb, struct pvrsgx_job *job);

/* The stream decoder's hooks, shared with transfer jobs. */
bool pvrsgx_stream_mapped(const void *priv, u64 addr, u64 size);
bool pvrsgx_stream_no_quirk(const void *priv, u32 quirk);
int pvrsgx_stream_take(const struct pvrsgx_stream_cmd_defs *defs,
		       struct pvrsgx_stream_ctx *sctx, struct pvrsgx_vm *vm,
		       u64 uptr, u32 len, void *dest);

#endif /* PVRSGX_RENDER_H */
