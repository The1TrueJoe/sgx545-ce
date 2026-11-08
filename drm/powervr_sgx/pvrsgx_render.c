// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * Render contexts, RT datasets and render jobs: everything the microkernel
 * needs for a TA kick and the 3D after it, built by the kernel from the
 * client's CMD_RENDER and CMD_TA_KICK streams.
 *
 * Layouts and values: docs/STATE.md sections 1 and 6, docs/UAPI.md section 4.
 * A render job is one scene of one TA kick, which starts the scene and the 3D
 * (TA flags 0x63). Scenes of several kicks need the middle-kick flags, which no
 * capture shows yet (G12).
 */

#include <drm/drm_file.h>
#include <drm/drm_gpuvm.h>
#include <drm/drm_print.h>

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/seq_buf.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <uapi/drm/powervr_sgx_drm.h>

#include "powervr_sgx_stream.h"
#include "pvrsgx_device.h"
#include "pvrsgx_drv.h"
#include "pvrsgx_fw.h"
#include "pvrsgx_job.h"
#include "pvrsgx_kobj.h"
#include "pvrsgx_mmu.h"
#include "pvrsgx_render.h"
#include "pvrsgx_transfer.h"
#include "pvrsgx_vm.h"

#define PB_DEFAULT_PAGES	0x400
#define PB_EXTRA_PAGES		0x22
#define FREELIST_BYTES		0x40000	/* one word per page of the 256 MiB range */
#define CCB_BYTES		0x10420
#define TA_CMD_BYTES		0x2c0	/* 0x2b0 rounded up to 64 */
/* The background object's middle PDS word, a constant. */
#define BG_PDS_MIDDLE		0x00030001
#define TA_CMD_SHARED		0x58
#define TA_CMD_TAIL		0x238
#define RT_MAX_DIM		2048

static u32 f32_one(void)
{
	return 0x3f800000;
}

/* ---------------------------------------------------------------- render context */

/* The PB's free list and descriptor as new: at creation, and after a reset (pb_rearm). */
static void pb_init(struct pvrsgx_pb *pb)
{
	u32 pages = pb->pages, first, last, zls, ta, pds, *fl, *pd;

	/* Free list: prev<<16 | next per page, indexed from the range's base. */
	first = (kobj_va(pb->buf) - PVRSGX_HEAP_3D_PARAMS) >> PAGE_SHIFT;
	last = first + pages - 1;
	fl = kobj_cpu(pb->freelist);
	for (u32 i = first; i <= last; i++)
		fl[i] = (i == first ? 0xffffu : i - 1) << 16 | (i == last ? 0xffffu : i + 1);

	kobj_cpu(pb->blocks)[0] = pages;
	kobj_cpu(pb->blocks)[1] = first | last << 16;
	kobj_cpu(pb->blocks)[2] = kobj_va(pb->desc);

	zls = pages - PB_EXTRA_PAGES;
	ta = pages + 0x10;
	pds = min(zls, ta);
	pd = kobj_cpu(pb->desc);
	memset(pd, 0, 0x3c);
	pd[1] = pages;
	pd[2] = last << 16 | first;	/* free list head */
	pd[3] = (last - 1) << 16;
	pd[4] = kobj_va(pb->freelist);
	pd[6] = pd[2];
	pd[7] = pd[3];
	pd[10] = ta;			/* TA threshold */
	pd[11] = zls;			/* ZLS threshold */
	pd[12] = zls;			/* global threshold */
	pd[13] = pds > 0x100 ? pds - 0x100 : 0;	/* PDS threshold */
	pd[14] = kobj_va(pb->blocks);
}

/*
 * The VM's parameter buffer: PB, free list, PB descriptor and block list. One
 * per VM, shared by its render contexts, as the DDK shares one PB descriptor
 * (pb.c): the DPM numbers PB pages from the 3D-params heap base, so a VM's PB
 * must start there and a second one can't (UAPI.md G15). The first context
 * sets the size; later requests are ignored, as the DDK does.
 */
static struct pvrsgx_pb *pb_create(struct pvrsgx_device *sdev, struct pvrsgx_vm *vm, u32 req_pages)
{
	struct pvrsgx_pb *pb = kzalloc_obj(*pb);
	u32 pages = (req_pages ?: PB_DEFAULT_PAGES) + PB_EXTRA_PAGES;
	int err;

	if (!pb)
		return ERR_PTR(-ENOMEM);
	kref_init(&pb->ref);
	pb->sdev = sdev;

	/* The PB, and what is addressed from its 1 MiB-aligned base, in the VM. */
	err = pvrsgx_kobj_new(sdev, &pb->buf, PVRSGX_KRANGE_3DPARAMS, pages << PAGE_SHIFT, 0, vm);
	if (err)
		goto fail;
	pb->base = kobj_va(pb->buf) & 0xfff00000;
	err = pvrsgx_kobj_new(sdev, &pb->freelist, PVRSGX_KRANGE_3DPARAMS, FREELIST_BYTES, pb->base,
			      vm);
	err = err ?: pvrsgx_kobj_new(sdev, &pb->desc, PVRSGX_KRANGE_KDATA, 0x3c, 0, NULL);
	err = err ?: pvrsgx_kobj_new(sdev, &pb->blocks, PVRSGX_KRANGE_KDATA, 0x10, 0, NULL);
	if (err)
		goto fail;

	pb->pages = pages;
	pb_init(pb);
	pb->fw_gen = sdev->fw_gen;
	return pb;

fail:
	pvrsgx_pb_put(pb);
	return ERR_PTR(err);
}

static void pb_release(struct kref *ref)
{
	struct pvrsgx_pb *pb = container_of(ref, struct pvrsgx_pb, ref);
	struct pvrsgx_device *sdev = pb->sdev;

	pvrsgx_kobj_drop(sdev, &pb->blocks);
	pvrsgx_kobj_drop(sdev, &pb->desc);
	pvrsgx_kobj_drop(sdev, &pb->freelist);
	pvrsgx_kobj_drop(sdev, &pb->buf);
	kfree(pb);
}

void pvrsgx_pb_put(struct pvrsgx_pb *pb)
{
	if (pb)
		kref_put(&pb->ref, pb_release);
}

/* The VM's PB, created by its first render context; the VM keeps one reference until closed. */
static struct pvrsgx_pb *vm_pb_get(struct pvrsgx_device *sdev, struct pvrsgx_vm *vm, u32 req_pages)
{
	struct pvrsgx_pb *pb;

	mutex_lock(&vm->lock);
	if (!vm->pb) {
		pb = pb_create(sdev, vm, req_pages);
		if (IS_ERR(pb)) {
			mutex_unlock(&vm->lock);
			return pb;
		}
		vm->pb = pb;
	}
	pb = vm->pb;
	kref_get(&pb->ref);
	mutex_unlock(&vm->lock);
	return pb;
}

int pvrsgx_render_ctx_init(struct pvrsgx_device *sdev, struct pvrsgx_context *ctx, u32 req_pages)
{
	struct pvrsgx_render_ctx *r = kzalloc_obj(*r);
	struct pvrsgx_vm *vm = ctx->vm;
	u32 *hw;
	int err = -ENOMEM;

	if (!r)
		return -ENOMEM;
	ctx->render = r;

	r->pb = vm_pb_get(sdev, vm, req_pages);
	if (IS_ERR(r->pb)) {
		err = PTR_ERR(r->pb);
		r->pb = NULL;
		goto fail;
	}

	err = pvrsgx_kobj_new(sdev, &r->scratch, PVRSGX_KRANGE_3DPARAMS, 0x40, r->pb->base, vm);
	err = err ?: pvrsgx_kobj_new(sdev, &r->ccb, PVRSGX_KRANGE_KDATA, CCB_BYTES, 0, NULL);
	err = err ?: pvrsgx_kobj_new(sdev, &r->ccbctl, PVRSGX_KRANGE_KDATA, 8, 0, NULL);
	err = err ?: pvrsgx_kobj_new(sdev, &r->sync3d, PVRSGX_KRANGE_SYNC, SYNC_DATA_BYTES, 0,
				     NULL);
	err = err ?: pvrsgx_kobj_new(sdev, &r->hwctx, PVRSGX_KRANGE_KCODE, 0x44, 0, NULL);
	if (err)
		goto fail;

	/* HW render context: the microkernel points list 0 at word 2's directory. */
	hw = kobj_cpu(r->hwctx);
	hw[0] = 0x21;			/* b0, b5 = per-context PB */
	hw[2] = pvrsgx_mmu_pd_addr(vm->mmu);
	hw[3] = kobj_va(r->ccb);
	hw[4] = kobj_va(r->ccbctl);
	hw[6] = kobj_va(r->pb->desc);
	hw[7] = kobj_va(r->scratch);
	hw[16] = task_tgid_nr(current);
	return 0;

fail:
	pvrsgx_render_ctx_fini(sdev, ctx);
	return err;
}

void pvrsgx_render_ctx_fini(struct pvrsgx_device *sdev, struct pvrsgx_context *ctx)
{
	struct pvrsgx_render_ctx *r = ctx->render;

	if (!r)
		return;
	pvrsgx_kobj_drop(sdev, &r->hwctx);
	pvrsgx_kobj_drop(sdev, &r->sync3d);
	pvrsgx_kobj_drop(sdev, &r->ccbctl);
	pvrsgx_kobj_drop(sdev, &r->ccb);
	pvrsgx_kobj_drop(sdev, &r->scratch);
	pvrsgx_pb_put(r->pb);
	kfree(r);
	ctx->render = NULL;
}

/* ---------------------------------------------------------------- RT datasets */

static void rt_numbers(struct pvrsgx_rt_numbers *r, u32 w, u32 h)
{
	u32 n, big, m;

	r->xt = (w + 31) >> 5;
	r->yt = (h + 15) >> 4;
	if (w * h < 0x4b000) {
		r->mx = (((r->xt + 1) >> 1) + 3) & ~3u;
		r->my = (((r->yt + 1) >> 1) + 3) & ~3u;
		r->mtile1 = r->mx << 22 | r->mx << 12 | r->mx;
		r->mtile2 = r->my << 22 | r->my << 12 | r->my;
		n = 2;
	} else {
		r->mx = (((r->xt + 3) >> 2) + 3) & ~3u;
		r->my = (((r->yt + 3) >> 2) + 3) & ~3u;
		r->mtile1 = BIT(31) | r->mx << 22 | 2 * r->mx << 12 | 3 * r->mx;
		r->mtile2 = r->my << 22 | 2 * r->my << 12 | 3 * r->my;
		n = 4;
	}
	r->padx = r->mx * n;
	r->pady = r->my * n;
	r->te_screen = (r->yt - 1) << 12 | (r->xt - 1);
	r->mt_tiles = r->mx * r->my;
	r->region_bytes = r->padx * r->pady * 12;
	m = max(r->padx, r->pady) - 1;
	for (big = 1; m; m >>= 1)
		big <<= 1;
	r->tailptr_bytes = big * big * 4;
}

static void rtds_release(struct kref *ref)
{
	struct pvrsgx_rt_dataset *s = container_of(ref, struct pvrsgx_rt_dataset, ref);
	struct pvrsgx_device *sdev = s->ctx->sdev;

	pvrsgx_kobj_drop(sdev, &s->rd);
	pvrsgx_kobj_drop(sdev, &s->rd_w8);
	pvrsgx_kobj_drop(sdev, &s->rd_w4);
	pvrsgx_kobj_drop(sdev, &s->hwsync);
	pvrsgx_kobj_drop(sdev, &s->rtword);
	pvrsgx_kobj_drop(sdev, &s->rtdata);
	pvrsgx_kobj_drop(sdev, &s->rd_sync);
	pvrsgx_kobj_drop(sdev, &s->hwsync_access);
	pvrsgx_kobj_drop(sdev, &s->done);
	pvrsgx_kobj_drop(sdev, &s->tailptr);
	pvrsgx_kobj_drop(sdev, &s->ta40);
	pvrsgx_kobj_drop(sdev, &s->ta50);
	pvrsgx_kobj_drop(sdev, &s->bgblock);
	for (int i = 0; i < 2; i++) {
		pvrsgx_kobj_drop(sdev, &s->entword[i]);
		pvrsgx_kobj_drop(sdev, &s->rgn[i]);
		pvrsgx_kobj_drop(sdev, &s->ta88[i]);
	}
	pvrsgx_context_put(s->ctx);
	kfree(s);
}

void pvrsgx_rtds_put(struct pvrsgx_rt_dataset *rtds)
{
	if (rtds)
		kref_put(&rtds->ref, rtds_release);
}

/* The dataset's structures as new, frame count 0. */
static void rtds_init(struct pvrsgx_rt_dataset *s)
{
	struct pvrsgx_render_ctx *r = s->ctx->render;
	u32 w = s->w, h = s->h, *rt, *b, *rd;

	memset(kobj_cpu(s->rtdata), 0, 2 * 0x38 + 0x18);
	memset(kobj_cpu(s->bgblock), 0, 0x60);
	memset(kobj_cpu(s->rd), 0, 0x1ec);
	for (int i = 0; i < 2; i++)
		kobj_cpu(s->entword[i])[0] = 0;
	s->frames = 0;

	/* RT data: header, then two entries of 14 words. */
	kobj_cpu(s->rtword)[0] = 1;
	rt = kobj_cpu(s->rtdata);
	rt[0] = kobj_va(s->rtword);
	rt[2] = s->n.padx;
	rt[5] = 2;
	for (int i = 0; i < 2; i++) {
		u32 *e = rt + 6 + 14 * i;

		e[0] = kobj_va(r->hwctx);
		e[2] = s->n.mt_tiles >> 4;
		e[3] = s->n.mt_tiles * 0xc;
		e[4] = kobj_va(s->bgblock);
		e[5] = kobj_va(s->ta88[i]);
		e[6] = kobj_va(s->ta40);
		e[7] = kobj_va(s->ta50);
		e[8] = kobj_va(s->tailptr);
		e[9] = s->n.tailptr_bytes;
		e[10] = 1;
		e[12] = kobj_va(s->entword[i]);
		e[13] = 0x10;
	}

	/* Background object: screen-space vertices, fixed point offset by 0x1000. */
	b = kobj_cpu(s->bgblock);
	b[5] = f32_one();
	b[8] = f32_one();
	b[10] = 0x202;
	b[15] = 0x100000;
	b[16] = 0x1000;
	b[17] = 0x3f80;
	b[18] = ((w + 0x1000) * 0x100) << 8 | 0x10;
	b[19] = f32_one();
	b[20] = (h + 0x1000) * 0x100;
	b[21] = 0x1000;
	b[22] = 0x3f80;

	/* Render details; the per-render words are written at each job. */
	rd = kobj_cpu(s->rd);
	rd[0x00 / 4] = r->pb->base;
	rd[0x08 / 4] = 0x80;
	rd[0x0c / 4] = 0x000f0000;
	rd[0x10 / 4] = 0x80000000;
	rd[0x38 / 4] = 0x80000000;
	rd[0x3c / 4] = 0x80000000;
	rd[0x40 / 4] = f32_one();
	rd[0x44 / 4] = 0x300;
	rd[0x48 / 4] = 0x2000000 | (kobj_va(s->bgblock) - r->pb->base) >> 4;
	rd[0x50 / 4] = 0x88;
	rd[0x58 / 4] = 0x30;
	rd[0x64 / 4] = 1;
	rd[0x68 / 4] = 1;
	rd[0xcc / 4] = 0x100;
	rd[0xd0 / 4] = kobj_va(s->rd_sync);
	rd[0xd8 / 4] = kobj_va(s->rtdata);
	rd[0x104 / 4] = 1;
	rd[0x110 / 4] = kobj_va(s->rd_w8);
	rd[0x114 / 4] = kobj_va(s->rd_w4);
	rd[0x13c / 4] = 0x07c00000;
	kobj_cpu(s->rd_sync)[0] = 1;
	kobj_cpu(s->hwsync)[0] = kobj_va(s->hwsync_access);
	kobj_cpu(s->hwsync_access)[0] = 1;
}

static struct pvrsgx_rt_dataset *rtds_create(struct pvrsgx_context *ctx, u32 w, u32 h)
{
	struct pvrsgx_device *sdev = ctx->sdev;
	struct pvrsgx_render_ctx *r = ctx->render;
	struct pvrsgx_vm *vm = ctx->vm;
	struct pvrsgx_rt_dataset *s = kzalloc_obj(*s);
	int err;

	if (!s)
		return ERR_PTR(-ENOMEM);
	kref_init(&s->ref);
	kref_get(&ctx->ref);
	s->ctx = ctx;
	s->w = w;
	s->h = h;
	rt_numbers(&s->n, w, h);

	err = pvrsgx_kobj_new(sdev, &s->rd, PVRSGX_KRANGE_KDATA, 0x1ec, 0, NULL);
	err = err ?: pvrsgx_kobj_new(sdev, &s->rd_w8, PVRSGX_KRANGE_KDATA, 8, 0, NULL);
	err = err ?: pvrsgx_kobj_new(sdev, &s->rd_w4, PVRSGX_KRANGE_KDATA, 4, 0, NULL);
	err = err ?: pvrsgx_kobj_new(sdev, &s->hwsync, PVRSGX_KRANGE_KDATA, 0x28, 0, NULL);
	err = err ?: pvrsgx_kobj_new(sdev, &s->rtword, PVRSGX_KRANGE_KDATA, 4, 0, NULL);
	err = err ?: pvrsgx_kobj_new(sdev, &s->rtdata, PVRSGX_KRANGE_KDATA, 2 * 0x38 + 0x18, 0,
				     NULL);
	err = err ?: pvrsgx_kobj_new(sdev, &s->rd_sync, PVRSGX_KRANGE_SYNC, 8, 0, NULL);
	err = err ?: pvrsgx_kobj_new(sdev, &s->hwsync_access, PVRSGX_KRANGE_SYNC, 8, 0, NULL);
	err = err ?: pvrsgx_kobj_new(sdev, &s->done, PVRSGX_KRANGE_SYNC, SYNC_DATA_BYTES, 0, NULL);
	/* What the TA writes and the 3D reads, under the VM's directory too. */
	err = err ?: pvrsgx_kobj_new(sdev, &s->tailptr, PVRSGX_KRANGE_TADATA,
				     s->n.tailptr_bytes, 0, vm);
	err = err ?: pvrsgx_kobj_new(sdev, &s->ta40, PVRSGX_KRANGE_TADATA, 0x40, 0, vm);
	err = err ?: pvrsgx_kobj_new(sdev, &s->ta50, PVRSGX_KRANGE_TADATA, 0x50, 0, vm);
	err = err ?: pvrsgx_kobj_new(sdev, &s->bgblock, PVRSGX_KRANGE_3DPARAMS, 0x200, r->pb->base,
				     vm);
	for (int i = 0; i < 2; i++) {
		err = err ?: pvrsgx_kobj_new(sdev, &s->rgn[i], PVRSGX_KRANGE_TADATA,
					     s->n.region_bytes, 0, vm);
		err = err ?: pvrsgx_kobj_new(sdev, &s->ta88[i], PVRSGX_KRANGE_TADATA, 0x88, 0, vm);
		err = err ?: pvrsgx_kobj_new(sdev, &s->entword[i], PVRSGX_KRANGE_KDATA, 4, 0, NULL);
	}
	if (err)
		goto fail;

	rtds_init(s);
	return s;

fail:
	pvrsgx_rtds_put(s);
	return ERR_PTR(err);
}

static struct pvrsgx_rt_dataset *rtds_lookup(struct pvrsgx_file *sfile, u32 handle)
{
	struct pvrsgx_rt_dataset *s;

	xa_lock(&sfile->rtds);
	s = xa_load(&sfile->rtds, handle);
	if (s)
		kref_get(&s->ref);
	xa_unlock(&sfile->rtds);
	return s;
}

void pvrsgx_rtds_destroy_all(struct pvrsgx_file *sfile)
{
	struct pvrsgx_rt_dataset *s;
	unsigned long handle;

	xa_for_each(&sfile->rtds, handle, s) {
		xa_erase(&sfile->rtds, handle);
		pvrsgx_rtds_put(s);
	}
}

int pvrsgx_ioctl_create_rt_dataset(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_powervr_sgx_ioctl_create_rt_dataset_args *args = data;
	struct pvrsgx_file *sfile = to_pvrsgx_file(file);
	struct pvrsgx_rt_dataset *s;
	struct pvrsgx_context *ctx;
	int err;

	if (args->flags || args->samples != 1 || !args->width || !args->height ||
	    args->width > RT_MAX_DIM || args->height > RT_MAX_DIM)
		return -EINVAL;
	ctx = pvrsgx_context_lookup(sfile, args->context_handle);
	if (!ctx)
		return -ENOENT;
	if (!ctx->render) {
		pvrsgx_context_put(ctx);
		return -EINVAL;
	}
	s = rtds_create(ctx, args->width, args->height);
	pvrsgx_context_put(ctx);
	if (IS_ERR(s))
		return PTR_ERR(s);
	err = xa_alloc(&sfile->rtds, &args->handle, s, xa_limit_32b, GFP_KERNEL);
	if (err)
		pvrsgx_rtds_put(s);
	return err;
}

int pvrsgx_ioctl_destroy_rt_dataset(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_powervr_sgx_ioctl_destroy_rt_dataset_args *args = data;
	struct pvrsgx_rt_dataset *s;

	if (args->_padding_4)
		return -EINVAL;
	s = xa_erase(&to_pvrsgx_file(file)->rtds, args->handle);
	if (!s)
		return -ENOENT;
	pvrsgx_rtds_put(s);
	return 0;
}

/* ---------------------------------------------------------------- streams */

bool pvrsgx_stream_mapped(const void *priv, u64 addr, u64 size)
{
	struct pvrsgx_vm *vm = (struct pvrsgx_vm *)priv;
	u64 cur = addr, end = addr + size;
	struct drm_gpuva *va;

	if (end < addr)
		return false;
	drm_gpuvm_for_each_va_range(va, &vm->gpuvm, addr, end) {
		if (va->va.addr > cur)
			return false;
		cur = va->va.addr + va->va.range;
		if (cur >= end)
			return true;
	}
	return cur >= end;
}

bool pvrsgx_stream_no_quirk(const void *priv, u32 quirk)
{
	return false;
}

/* Copy one stream in, decode and check it field by field, then against the VM. */
int pvrsgx_stream_take(const struct powervr_sgx_stream_cmd_defs *defs,
		       struct powervr_sgx_stream_ctx *sctx, struct pvrsgx_vm *vm,
		       u64 uptr, u32 len, void *dest)
{
	void *buf;
	int err;

	if (!len || len > POWERVR_SGX_STREAM_MAX_SIZE || (uptr & 7))
		return -EINVAL;
	buf = memdup_user(u64_to_user_ptr(uptr), len);
	if (IS_ERR(buf))
		return PTR_ERR(buf);
	err = powervr_sgx_stream_process(defs, sctx, buf, len, dest);
	kfree(buf);
	if (err)
		return err;
	mutex_lock(&vm->lock);
	err = powervr_sgx_stream_check_mapped(defs, sctx, dest);
	mutex_unlock(&vm->lock);
	return err;
}

int pvrsgx_render_job_init(struct drm_file *file, struct pvrsgx_job *job,
			   const struct drm_powervr_sgx_job *in)
{
	struct pvrsgx_render_job *rj = &job->render;
	struct powervr_sgx_stream_ctx sctx = {};
	struct drm_powervr_sgx_ta_kick *kicks;
	struct pvrsgx_vm *vm = job->ctx->vm;
	int err;

	if (in->flags & ~DRM_POWERVR_SGX_SUBMIT_JOB_RENDER_FLAGS_MASK)
		return -EINVAL;
	if (!job->ctx->render)
		return -EINVAL;
	rj->rtds = rtds_lookup(to_pvrsgx_file(file), in->rt_dataset_handle);
	if (!rj->rtds)
		return -ENOENT;
	if (rj->rtds->ctx != job->ctx)
		return -EINVAL;
	if (!in->ta_kicks.count)
		return -EINVAL;
	/* Scenes of more than two kicks need the middle-kick flags (G12). */
	if (in->ta_kicks.count > PVRSGX_MAX_TA_KICKS)
		return -EOPNOTSUPP;

	sctx.heaps[POWERVR_SGX_STREAM_HEAP_PDS_PIXEL].base = PVRSGX_HEAP_PDS_PIXEL;
	sctx.heaps[POWERVR_SGX_STREAM_HEAP_PDS_PIXEL].size =
		PVRSGX_HEAP_KERNEL_CODE - PVRSGX_HEAP_PDS_PIXEL;
	sctx.limits[POWERVR_SGX_STREAM_LIMIT_TILES_X] = rj->rtds->n.xt - 1;
	sctx.limits[POWERVR_SGX_STREAM_LIMIT_TILES_Y] = rj->rtds->n.yt - 1;
	sctx.has_quirk = pvrsgx_stream_no_quirk;
	sctx.rt_pad_width = rj->rtds->n.xt * 32;
	sctx.rt_pad_height = rj->rtds->n.yt * 16;
	sctx.mapped = pvrsgx_stream_mapped;
	sctx.priv = vm;

	err = pvrsgx_stream_take(&powervr_sgx_stream_render_defs, &sctx, vm, in->cmd_stream,
				 in->cmd_stream_len, &rj->render);
	if (err)
		return err;
	err = powervr_sgx_check_render(&rj->render);
	if (err)
		return err;

	kicks = pvrsgx_obj_array_get(&in->ta_kicks, sizeof(*kicks), PVRSGX_MAX_TA_KICKS);
	if (IS_ERR(kicks))
		return PTR_ERR(kicks);
	rj->nkicks = in->ta_kicks.count;
	for (u32 k = 0; k < rj->nkicks && !err; k++)
		err = kicks[k].flags ? -EINVAL :
		      pvrsgx_stream_take(&powervr_sgx_stream_ta_kick_defs, &sctx, vm,
					 kicks[k].cmd_stream, kicks[k].cmd_stream_len,
					 &rj->kick[k]);
	kvfree(kicks);
	return err;
}

void pvrsgx_render_job_fini(struct pvrsgx_job *job)
{
	pvrsgx_rtds_put(job->render.rtds);
	job->render.rtds = NULL;
}

static u32 pds_word(u64 addr, u32 rows)
{
	return rows << 26 | (u32)(addr - PVRSGX_HEAP_PDS_PIXEL) >> 4;
}

/*
 * Write the TA command for kick k of the job's scene (and, at the first, the
 * render details), and say where the microkernel will report the scene done.
 * Runs in run_job: one job on the GPU at a time, so the context's CCB and the
 * dataset are ours.
 *
 * One kick both starts the scene and renders it (TA flags 0x63, the form Mesa
 * uses). Two kicks are the captured form of a single-draw frame (STATE.md, "A
 * frame, end to end"): 0x71 runs the stream, 0x62 runs the terminate words and
 * starts the 3D.
 */
/*
 * A restart (a reset, or a runtime resume) starts a fresh microkernel, but a
 * VM's PB lives through it. After a hang the pages the TA took are still off
 * its free list: the next render on any of the VM's contexts hangs, and the
 * next restart fails. The PB's first job after a restart sets it up as new.
 * Nothing else needs it (log 2026-11-03-0600): the HW contexts and RT datasets
 * carry on as they are.
 */
static void pb_rearm(struct pvrsgx_device *sdev, struct pvrsgx_pb *pb)
{
	if (pb->fw_gen != sdev->fw_gen) {
		pb_init(pb);
		pb->fw_gen = sdev->fw_gen;
	}
}

int pvrsgx_render_job_build(struct pvrsgx_job *job, u32 k, u32 data[6])
{
	struct pvrsgx_render_job *rj = &job->render;
	struct pvrsgx_render_ctx *r = job->ctx->render;
	struct pvrsgx_rt_dataset *s = rj->rtds;
	const struct powervr_sgx_stream_render *in = &rj->render;
	bool first = k == 0, last = k == rj->nkicks - 1;
	u32 f, odd, rgn_this, rgn_next, entry, *cmd, *sh, *t, *rd, *sl, *s3d, *done, *b;
	u32 woff = r->ccb_woff, zls_base = 0;

	if (first)
		pb_rearm(job->ctx->sdev, r->pb);
	f = s->frames;
	odd = f & 1;
	rgn_this = kobj_va(s->rgn[odd ? 0 : 1]);
	rgn_next = kobj_va(s->rgn[odd ? 1 : 0]);
	entry = kobj_va(s->rtdata) + 0x18 + 0x38 * (odd ? 0 : 1);
	rd = kobj_cpu(s->rd);
	if (first) {
		/* Render details (STATE.md 6; UAPI.md 4.2). */
		if (in->depth_addr || in->stencil_addr) {
			u64 lo = in->depth_addr ?: in->stencil_addr;

			if (in->depth_addr && in->stencil_addr)
				lo = min(in->depth_addr, in->stencil_addr);

			zls_base = (u32)lo & 0xfff00000;
		}
		rd[0x04 / 4] = zls_base;
		rd[0x14 / 4] = in->depth_bias_units;
		rd[0x18 / 4] = in->depth_bias_factor;
		rd[0x20 / 4] = rgn_this;
		rd[0x28 / 4] = rgn_next;
		rd[0x2c / 4] = in->zls_control ? in->zls_control | (s->n.xt - 1) << 3 : 0;
		rd[0x30 / 4] = in->depth_addr ? (u32)in->depth_addr - zls_base : 0;
		rd[0x34 / 4] = rd[0x30 / 4];
		rd[0x38 / 4] = BIT(31) | (in->stencil_addr ? (u32)in->stencil_addr - zls_base : 0);
		rd[0x3c / 4] = rd[0x38 / 4];
		rd[0x40 / 4] = in->bg_depth;
		rd[0x5c / 4] = (u32)in->pixel_event_pds_addr;
		rd[0x60 / 4] = in->pixel_event_pds_rows;
		rd[0xdc / 4] = entry;
		kobj_cpu(s->rd_w4)[0] = pds_word(in->bg_pds_primary_addr, in->bg_pds_primary_rows);

		/*
		 * The background object's PDS words, as the DDK microkernel copies them
		 * at the 3D start (secondary, middle, primary): the open microkernel
		 * leaves them to the host, so it never runs on the job's directory.
		 */
		b = kobj_cpu(s->bgblock);
		b[0] = pds_word(in->bg_pds_secondary_addr, in->bg_pds_secondary_rows);
		b[1] = BG_PDS_MIDDLE;
		b[2] = kobj_cpu(s->rd_w4)[0];

		/* What the microkernel clears and the client sets before a scene. */
		kobj_cpu(s->rtword)[0] = f + 1;
		kobj_cpu(s->rd_sync)[0] = 1;
		kobj_cpu(s->hwsync_access)[0] = 1;
	}

	/* The TA command, in the context's ring. */
	cmd = (u32 *)((u8 *)kobj_cpu(r->ccb) + woff);
	memset(cmd, 0, TA_CMD_BYTES);
	cmd[0x00 / 4] = TA_CMD_BYTES;
	if (rj->nkicks == 1)
		cmd[0x04 / 4] = 0x63 | (f ? 0 : 0x10);	/* first and last kick; first render */
	else
		/* 0x10 on the dataset's first render only, as for one kick */
		cmd[0x04 / 4] = first ? 0x61 | (f ? 0 : 0x10) : last ? 0x62 : 0x60;
	cmd[0x08 / 4] = pds_word(in->bg_pds_secondary_addr, in->bg_pds_secondary_rows);
	cmd[0x0c / 4] = BG_PDS_MIDDLE;
	cmd[0x10 / 4] = pds_word(in->bg_pds_primary_addr, in->bg_pds_primary_rows);
	cmd[0x14 / 4] = 0x50000000;
	cmd[0x18 / 4] = 0x50000000;
	cmd[0x1c / 4] = last ? in->flags_3d | BIT(10) | f << 14 : 0;
	cmd[0x20 / 4] = f;
	cmd[0x24 / 4] = kobj_va(r->pb->desc);
	cmd[0x38 / 4] = kobj_va(s->rd);
	cmd[0x3c / 4] = kobj_va(s->hwsync);
	cmd[0x40 / 4] = kobj_va(s->rtdata);
	cmd[0x44 / 4] = entry;

	/* CMDTA_SHARED: ready; the 3D sync on the last kick; the completion word on the first. */
	sh = cmd + TA_CMD_SHARED / 4;
	sh[0x00 / 4] = 1;				/* SGXMKIF_CMDTA_CTRLFLAGS_READY */
	if (last) {
		s3d = kobj_cpu(r->sync3d);
		sh[0x1c / 4] = s3d[SYNC_WRITE_PENDING];
		sh[0x20 / 4] = kobj_va(r->sync3d) + SYNC_WRITE_COMPLETE * 4;
		sh[0x24 / 4] = s3d[SYNC_READ_PENDING]++;
		sh[0x28 / 4] = kobj_va(r->sync3d) + SYNC_READ_COMPLETE * 4;
	}
	if (first) {
		done = kobj_cpu(s->done);
		sl = kobj_cpu(s->hwsync);
		sl[1] = 1;				/* one destination */
		sl[2] = done[SYNC_READ_PENDING];
		sl[3] = kobj_va(s->done) + SYNC_READ_COMPLETE * 4;
		sl[4] = done[SYNC_WRITE_PENDING]++;
		sl[5] = kobj_va(s->done) + SYNC_WRITE_COMPLETE * 4;
		/* The microkernel sets write-complete to the pending value plus one. */
		job->done_cpu = &done[SYNC_WRITE_COMPLETE];
		job->done_val = sl[4] + 1;
	}

	t = cmd + TA_CMD_TAIL / 4;
	t[0x00 / 4] = 0x20000000;			/* TE AA */
	t[0x04 / 4] = s->n.mtile1;
	t[0x08 / 4] = s->n.mtile2;
	t[0x0c / 4] = s->n.te_screen;
	t[0x10 / 4] = s->n.mt_tiles;
	t[0x14 / 4] = rj->kick[k].ta_flags2 | (last ? BIT(22) : 0);	/* b22: this kick renders */
	t[0x18 / 4] = (u32)rj->kick[k].vdm_base;
	t[0x1c / 4] = 0xa2;
	t[0x20 / 4] = rgn_this;
	t[0x24 / 4] = kobj_va(s->tailptr);
	t[0x28 / 4] = 0x358637bd;			/* 1e-6 */
	t[0x2c / 4] = 0x358637bd;
	t[0x30 / 4] = (s->h - 1) << 16 | (s->w - 1);
	t[0x3c / 4] = r->pb->base;
	t[0x6c / 4] = 0x88;

	r->ccb_woff = (woff + TA_CMD_BYTES) & (CCB_RING - 1);
	/* The command is in memory before the write offset that hands it over. */
	wmb();
	kobj_cpu(r->ccbctl)[0] = r->ccb_woff;
	if (last)
		rj->cmd_off = woff;
	if (last)
		s->frames++;

	memset(data, 0, 6 * sizeof(u32));
	data[1] = kobj_va(r->hwctx);
	return 0;
}

/* ---------------------------------------------------------------- transfers */

/* The structures of a render, word by word. */
static void render_dump(struct seq_buf *sb, struct pvrsgx_device *sdev, struct pvrsgx_rt_dataset *s,
			u32 cmd_off)
{
	struct pvrsgx_render_ctx *r = s->ctx->render;

	seq_buf_printf(sb, "PB 0x%08x pages 0x%x base 0x%08x; free list 0x%08x; scratch 0x%08x\n",
		       kobj_va(r->pb->buf), r->pb->pages, r->pb->base, kobj_va(r->pb->freelist),
		       kobj_va(r->scratch));
	pvrsgx_kobj_dump(sb, "hwctx", r->hwctx, 0, 0x44);
	pvrsgx_kobj_dump(sb, "ccbctl", r->ccbctl, 0, 8);
	pvrsgx_kobj_dump(sb, "pbdesc", r->pb->desc, 0, 0x3c);
	pvrsgx_kobj_dump(sb, "pbblocks", r->pb->blocks, 0, 0x10);
	pvrsgx_kobj_dump(sb, "freelist[first..]", r->pb->freelist,
			 ((kobj_va(r->pb->buf) - PVRSGX_HEAP_3D_PARAMS) >> PAGE_SHIFT) * 4, 0x20);
	pvrsgx_kobj_dump(sb, "scratch", r->scratch, 0, 0x40);
	pvrsgx_kobj_dump(sb, "sync3d", r->sync3d, 0, SYNC_DATA_BYTES);
	pvrsgx_kobj_dump(sb, "ta_cmd", r->ccb, cmd_off, TA_CMD_BYTES);
	pvrsgx_kobj_dump(sb, "render_details", s->rd, 0, 0x1ec & ~3u);
	pvrsgx_kobj_dump(sb, "rd_w4", s->rd_w4, 0, 4);
	pvrsgx_kobj_dump(sb, "rd_w8", s->rd_w8, 0, 8);
	pvrsgx_kobj_dump(sb, "rd_sync", s->rd_sync, 0, 4);
	pvrsgx_kobj_dump(sb, "rtdata", s->rtdata, 0, 2 * 0x38 + 0x18);
	pvrsgx_kobj_dump(sb, "rtword", s->rtword, 0, 4);
	pvrsgx_kobj_dump(sb, "entword0", s->entword[0], 0, 4);
	pvrsgx_kobj_dump(sb, "entword1", s->entword[1], 0, 4);
	pvrsgx_kobj_dump(sb, "hwsync", s->hwsync, 0, 0x28);
	pvrsgx_kobj_dump(sb, "hwsync_access", s->hwsync_access, 0, 4);
	pvrsgx_kobj_dump(sb, "done", s->done, 0, SYNC_DATA_BYTES);
	pvrsgx_kobj_dump(sb, "bgblock", s->bgblock, 0, 0x60);
	/* what the TA wrote for the 3D */
	pvrsgx_kobj_dump(sb, "rgn0", s->rgn[0], 0, 0x60);
	pvrsgx_kobj_dump(sb, "rgn1", s->rgn[1], 0, 0x60);
	pvrsgx_kobj_dump(sb, "tailptr", s->tailptr, 0, 0x40);
	pvrsgx_kobj_dump(sb, "ta40", s->ta40, 0, 0x40);
	pvrsgx_kobj_dump(sb, "ta50", s->ta50, 0, 0x50);
	pvrsgx_kobj_dump(sb, "ta88[0]", s->ta88[0], 0, 0x88);
	pvrsgx_kobj_dump(sb, "ta88[1]", s->ta88[1], 0, 0x88);
	seq_buf_printf(sb, "rgn 0x%08x 0x%08x tailptr 0x%08x ta40 0x%08x ta50 0x%08x\n",
		       kobj_va(s->rgn[0]), kobj_va(s->rgn[1]), kobj_va(s->tailptr),
		       kobj_va(s->ta40), kobj_va(s->ta50));
	seq_buf_printf(sb, "ta88 0x%08x 0x%08x\n", kobj_va(s->ta88[0]), kobj_va(s->ta88[1]));
	pvrsgx_fw_dump(sb, sdev, PVRSGX_FW_TA3DCTL, "ta3dctl");
}

/* A job's command and the structures it names, for a devcoredump. */
void pvrsgx_job_dump(struct seq_buf *sb, struct pvrsgx_job *job)
{
	struct pvrsgx_device *sdev = job->ctx->sdev;

	if (job->type == DRM_POWERVR_SGX_JOB_TYPE_RENDER && job->render.rtds)
		render_dump(sb, sdev, job->render.rtds, job->render.cmd_off);
	else if (job->type == DRM_POWERVR_SGX_JOB_TYPE_TRANSFER)
		pvrsgx_transfer_dump(sb, job);
}
