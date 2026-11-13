/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_RENDER_H
#define PVRSGX_RENDER_H

#include <linux/kref.h>
#include <linux/types.h>

#include <uapi/drm/powervr_sgx_drm.h>

#include "pvrsgx_cmd_defs.h"

struct drm_device;
struct drm_file;
struct pvrsgx_context;
struct pvrsgx_device;
struct pvrsgx_file;
struct pvrsgx_job;
struct pvrsgx_kobj;
struct pvrsgx_vm;

/* The firmware half of a render context. */
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
	/* A command for it was built: its objects may be the GPU's. */
	bool sent;
};

/* 16 MiB: the device's one PB holds every client's scenes (pvrsgx_render.c). */
#define PVRSGX_PB_DEFAULT_PAGES	0x1000

/* The PB a context gets without asking: the default, or the core's largest. */
#define pvrsgx_pb_default_pages(sdev) \
	min_t(u32, PVRSGX_PB_DEFAULT_PAGES, pvrsgx_heaps(sdev)->pb_max_pages)

void pvrsgx_pb_put(struct pvrsgx_pb *pb);
void pvrsgx_pb_unmap(struct pvrsgx_vm *vm);

/* A render target's tiling numbers. */
struct pvrsgx_rt_numbers {
	u32 xt, yt, mx, my, padx, pady, zls_xt;
	u32 mtile1, mtile2, te_screen, mt_tiles, rt_mt_tiles, region_bytes, tailptr_bytes;
	u32 te_aa, rt_flags;
};

struct pvrsgx_rt_dataset {
	struct kref ref;
	struct pvrsgx_context *ctx;
	u32 w, h, samples;
	struct pvrsgx_rt_numbers n;
	/*
	 * Per RT-data entry, which alternates by frame parity, so one frame's TA
	 * can be built while the other's 3D still reads its own: render details,
	 * their words, the dependency block and its syncs, and the background
	 * object, which the 3D fetches per tile.
	 */
	struct pvrsgx_kobj *rd[2], *rd_w8[2], *rd_w4[2], *hwsync[2];
	struct pvrsgx_kobj *rd_sync[2], *hwsync_access[2], *bgblock[2];
	struct pvrsgx_kobj *rtword, *rtdata, *entword[2], *done;
	struct pvrsgx_kobj *tailptr, *ta40, *ta50, *rgn[2], *ta88[2];
	/** @frames: Renders into this dataset so far. */
	u32 frames;
	/** @sent: A kick into it was built: its objects may be the GPU's. */
	bool sent;
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
	struct pvrsgx_cmd_render_all render;
	struct pvrsgx_cmd_ta_kick_all kick[PVRSGX_MAX_TA_KICKS];
	u32 nkicks;
	/** @cmd_off: Where the last kick's TA command went in the context's ring. */
	u32 cmd_off;
	/** @entry: The RT-data entry (frame parity) the render used. */
	u32 entry;
};

int pvrsgx_render_ctx_init(struct pvrsgx_device *sdev, struct pvrsgx_context *ctx, u32 pb_pages);
void pvrsgx_render_ctx_fini(struct pvrsgx_device *sdev, struct pvrsgx_context *ctx);

struct pvrsgx_rt_dataset *pvrsgx_rtds_create(struct pvrsgx_context *ctx, u32 w, u32 h, u32 samples);
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
#if IS_ENABLED(CONFIG_DEBUG_FS)
void pvrsgx_render_dump_last(struct seq_buf *sb, struct pvrsgx_device *sdev);
void pvrsgx_render_dbg_fini(struct pvrsgx_device *sdev);
#else
static inline void pvrsgx_render_dbg_fini(struct pvrsgx_device *sdev) { }
#endif

/* Command checks shared with transfer jobs. */
bool pvrsgx_cmd_mapped(const void *priv, u64 addr, u64 size);
const struct pvrsgx_cmd_def *pvrsgx_cmd_def(struct pvrsgx_device *sdev, enum pvrsgx_cmd_type type);
int pvrsgx_cmd_take(struct pvrsgx_device *sdev, const struct pvrsgx_cmd_def *def,
		    const struct pvrsgx_cmd_ctx *ctx, struct pvrsgx_vm *vm, const void *all,
		    u64 present);

#endif /* PVRSGX_RENDER_H */
