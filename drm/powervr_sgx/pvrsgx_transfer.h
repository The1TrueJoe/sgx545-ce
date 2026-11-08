/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_TRANSFER_H
#define PVRSGX_TRANSFER_H

#include <linux/types.h>

#include "powervr_sgx_stream.h"

struct drm_file;
struct drm_powervr_sgx_job;
struct pvrsgx_context;
struct pvrsgx_device;
struct pvrsgx_job;
struct pvrsgx_kobj;
struct seq_buf;

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

int pvrsgx_transfer_ctx_init(struct pvrsgx_device *sdev, struct pvrsgx_context *ctx);
void pvrsgx_transfer_ctx_fini(struct pvrsgx_device *sdev, struct pvrsgx_context *ctx);
int pvrsgx_transfer_job_init(struct drm_file *file, struct pvrsgx_job *job,
			     const struct drm_powervr_sgx_job *in);
void pvrsgx_transfer_job_build(struct pvrsgx_job *job, u32 data[6]);
void pvrsgx_transfer_dump(struct seq_buf *sb, struct pvrsgx_job *job);

#endif /* PVRSGX_TRANSFER_H */
