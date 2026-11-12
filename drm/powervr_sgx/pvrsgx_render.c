// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * Render contexts, RT datasets and render jobs: everything the microkernel
 * needs for a TA kick and the 3D after it, built by the kernel from the
 * client's CMD_RENDER and CMD_TA_KICK streams.
 *
 * A render job is one scene of one TA kick, which starts the scene and the 3D
 * (TA flags 0x63). Scenes of several kicks need the middle-kick flags, which
 * are not known yet.
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

#include "pvrsgx_cmd.h"
#include "pvrsgx_cmd_checks.h"
#include "pvrsgx_ccb.h"
#include "pvrsgx_device.h"
#include "pvrsgx_drv.h"
#include "pvrsgx_fw.h"
#include "pvrsgx_fwif.h"
#include "pvrsgx_job.h"
#include "pvrsgx_kobj.h"
#include "pvrsgx_mmu.h"
#include "pvrsgx_pm.h"
#include "pvrsgx_render.h"
#include "pvrsgx_trace.h"
#include "pvrsgx_transfer.h"
#include "pvrsgx_vm.h"

#define PB_EXTRA_PAGES		0x22
#define FREELIST_BYTES		0x40000	/* one word per page of the 256 MiB range */
#define CCB_BYTES		0x10420
/* The background object's middle PDS word, a constant. */
#define BG_PDS_MIDDLE		0x00030001
/* Constant TA command words. */
#define TA_CTL			0x50000000
#define TE_AA			0x20000000
#define TE_AA_4X		(TE_AA | BIT(31) | BIT(30))	/* 2x in X and in Y */
#define VDM_CTL			0xa2
#define F32_1E_6		0x358637bd
#define RT_FLAGS		0x88
#define RT_FLAGS_4X		0xeaa26e26	/* the RT flags of a 4x render */
#define RT_MAX_DIM		2048

static u32 f32_one(void)
{
	return 0x3f800000;
}

/* ---------------------------------------------------------------- render context */

/* The PB's free list and descriptor as new: at creation, and after a reset (pb_rearm). */
static void pb_init(struct pvrsgx_pb *pb)
{
	u32 pages = pb->pages, first, last, zls, ta, pds, *fl;
	struct pvrsgx_fw_pb_desc *pd;

	/* Free list: prev<<16 | next per page, indexed from the range's base. */
	first = (kobj_va(pb->buf) - pvrsgx_heaps(pb->sdev)->params_3d.base) >> PAGE_SHIFT;
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
	pd = (struct pvrsgx_fw_pb_desc *)kobj_cpu(pb->desc);
	memset(pd, 0, sizeof(*pd));
	pd->pages = pages;
	pd->free_head = last << 16 | first;
	pd->free_tail = (last - 1) << 16;
	pd->free_list = kobj_va(pb->freelist);
	pd->free_head_copy = pd->free_head;
	pd->free_tail_copy = pd->free_tail;
	pd->ta_threshold = ta;
	pd->zls_threshold = zls;
	pd->global_threshold = zls;
	pd->pds_threshold = pds > 0x100 ? pds - 0x100 : 0;
	pd->blocks = kobj_va(pb->blocks);
}

/*
 * The device's parameter buffer: PB, free list, PB descriptor and block list,
 * shared by every render context of every client. The DPM's free-list state is
 * one set of registers that the microkernel doesn't switch between PBs, so two
 * PBs would run each other's lists off their ends. The DPM numbers PB pages from
 * the 3D-params heap base, so the PB starts there, at the same address in every
 * VM (pvrsgx_pb_map). The first context sets the size; later requests are
 * ignored.
 */
static struct pvrsgx_pb *pb_create(struct pvrsgx_device *sdev, u32 req_pages)
{
	struct pvrsgx_pb *pb = kzalloc_obj(*pb);
	u32 pages = (req_pages ?: PVRSGX_PB_DEFAULT_PAGES) + PB_EXTRA_PAGES;
	int err;

	if (!pb)
		return ERR_PTR(-ENOMEM);
	kref_init(&pb->ref);
	pb->sdev = sdev;

	/* The PB, and what is addressed from its 1 MiB-aligned base, in the VM. */
	err = pvrsgx_kobj_new(sdev, &pb->buf, PVRSGX_KRANGE_3DPARAMS, pages << PAGE_SHIFT, 0, NULL);
	if (err)
		goto fail;
	pb->base = kobj_va(pb->buf) & 0xfff00000;
	err = pvrsgx_kobj_new(sdev, &pb->freelist, PVRSGX_KRANGE_3DPARAMS, FREELIST_BYTES, pb->base,
			      NULL);
	err = err ?: pvrsgx_kobj_new(sdev, &pb->desc, PVRSGX_KRANGE_KDATA,
				     sizeof(struct pvrsgx_fw_pb_desc), 0, NULL);
	if (pb->desc)
		pb->desc->until_restart = true;
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

/* The device's PB, created by the first render context; the device keeps one reference. */
static struct pvrsgx_pb *dev_pb_get(struct pvrsgx_device *sdev, u32 req_pages)
{
	struct pvrsgx_pb *pb;

	mutex_lock(&sdev->pb_lock);
	if (!sdev->pb) {
		pb = pb_create(sdev, req_pages);
		if (IS_ERR(pb)) {
			mutex_unlock(&sdev->pb_lock);
			return pb;
		}
		sdev->pb = pb;
		drm_info(&sdev->base, "parameter buffer: %#x pages\n", pb->pages);
	}
	pb = sdev->pb;
	kref_get(&pb->ref);
	mutex_unlock(&sdev->pb_lock);
	return pb;
}

static struct pvrsgx_kobj *pb_mapped(struct pvrsgx_pb *pb, int i)
{
	return i ? pb->freelist : pb->buf;
}

/* The PB and its free list into the VM, at their device addresses; once per VM. */
static int pvrsgx_pb_map(struct pvrsgx_vm *vm, struct pvrsgx_pb *pb)
{
	struct pvrsgx_device *sdev = vm->sdev;
	int err = 0, i;

	mutex_lock(&vm->lock);
	if (vm->pb)
		goto out;
	for (i = 0; i < ARRAY_SIZE(vm->pb_nodes); i++) {
		struct pvrsgx_kobj *o = pb_mapped(pb, i);

		vm->pb_nodes[i].start = kobj_va(o);
		vm->pb_nodes[i].size = o->km.size;
		mutex_lock(&sdev->kobj_lock);
		err = drm_mm_reserve_node(&vm->params, &vm->pb_nodes[i]);
		mutex_unlock(&sdev->kobj_lock);
		if (err)
			goto unwind;
		err = pvrsgx_mmu_map_pages(vm->mmu, kobj_va(o), o->km.pages, o->km.npages, 0);
		if (err) {
			i++;
			goto unwind;
		}
	}
	kref_get(&pb->ref);
	vm->pb = pb;
	mutex_lock(&sdev->ccb_lock);
	sdev->fw.cache_control |= PVRSGX_CC_INVAL_BIF_PT | PVRSGX_CC_INVAL_BIF_PD;
	mutex_unlock(&sdev->ccb_lock);
out:
	mutex_unlock(&vm->lock);
	return err;

unwind:
	while (i--) {
		pvrsgx_mmu_unmap(vm->mmu, vm->pb_nodes[i].start, vm->pb_nodes[i].size);
		mutex_lock(&sdev->kobj_lock);
		drm_mm_remove_node(&vm->pb_nodes[i]);
		mutex_unlock(&sdev->kobj_lock);
	}
	mutex_unlock(&vm->lock);
	return err;
}

/* At the VM's release: nothing on the GPU uses it any more. */
void pvrsgx_pb_unmap(struct pvrsgx_vm *vm)
{
	struct pvrsgx_device *sdev = vm->sdev;

	for (int i = 0; i < ARRAY_SIZE(vm->pb_nodes); i++) {
		pvrsgx_mmu_unmap(vm->mmu, vm->pb_nodes[i].start, vm->pb_nodes[i].size);
		mutex_lock(&sdev->kobj_lock);
		drm_mm_remove_node(&vm->pb_nodes[i]);
		mutex_unlock(&sdev->kobj_lock);
	}
	pvrsgx_pb_put(vm->pb);
	vm->pb = NULL;
}

int pvrsgx_render_ctx_init(struct pvrsgx_device *sdev, struct pvrsgx_context *ctx, u32 req_pages)
{
	struct pvrsgx_render_ctx *r = kzalloc_obj(*r);
	struct pvrsgx_vm *vm = ctx->vm;
	struct pvrsgx_fw_hw_render_ctx *hw;
	int err = -ENOMEM;

	if (!r)
		return -ENOMEM;
	ctx->render = r;

	r->pb = dev_pb_get(sdev, req_pages);
	if (IS_ERR(r->pb)) {
		err = PTR_ERR(r->pb);
		r->pb = NULL;
		goto fail;
	}
	err = pvrsgx_pb_map(vm, r->pb);
	if (err)
		goto fail;

	err = pvrsgx_kobj_new(sdev, &r->scratch, PVRSGX_KRANGE_3DPARAMS, 0x40, r->pb->base, vm);
	err = err ?: pvrsgx_kobj_new(sdev, &r->ccb, PVRSGX_KRANGE_KDATA, CCB_BYTES, 0, NULL);
	err = err ?: pvrsgx_kobj_new(sdev, &r->ccbctl, PVRSGX_KRANGE_KDATA, 8, 0, NULL);
	err = err ?: pvrsgx_kobj_new(sdev, &r->sync3d, PVRSGX_KRANGE_SYNC, SYNC_DATA_BYTES, 0,
				     NULL);
	err = err ?: pvrsgx_kobj_new(sdev, &r->hwctx, PVRSGX_KRANGE_KCODE,
				     sizeof(struct pvrsgx_fw_hw_render_ctx), 0, NULL);
	if (err)
		goto fail;

	/* HW render context: the microkernel points list 0 at word 2's directory. */
	hw = (void *)kobj_cpu(r->hwctx);
	hw->flags = PVRSGX_HW_RENDER_CTX_VALID | PVRSGX_HW_RENDER_CTX_OWN_PB;
	hw->page_dir = pvrsgx_vm_pd_word(vm);
	hw->ccb = kobj_va(r->ccb);
	hw->ccb_ctl = kobj_va(r->ccbctl);
	hw->pb_desc = kobj_va(r->pb->desc);
	hw->scratch = kobj_va(r->scratch);
	hw->pid = task_tgid_nr(current);
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

/*
 * At 4x (2x2 samples) the TE tiles samples: twice the tiles in X and in Y,
 * and the region and tail pointer arrays follow. Its macrotile registers
 * stay in pixel tiles, as does the screen.
 */
static void rt_numbers(struct pvrsgx_rt_numbers *r, u32 w, u32 h, u32 samples)
{
	u32 aa = samples == 4 ? 2 : 1, xt, yt, mx, my, n, big, m;

	r->xt = (w + 31) >> 5;
	r->yt = (h + 15) >> 4;
	xt = r->xt * aa;
	yt = r->yt * aa;
	if (w * h * samples < 0x4b000) {
		mx = (((xt + 1) >> 1) + 3) & ~3u;
		my = (((yt + 1) >> 1) + 3) & ~3u;
		n = 2;
	} else {
		mx = (((xt + 3) >> 2) + 3) & ~3u;
		my = (((yt + 3) >> 2) + 3) & ~3u;
		n = 4;
	}
	r->mx = mx / aa;
	r->my = my / aa;
	if (n == 2) {
		r->mtile1 = r->mx << 22 | r->mx << 12 | r->mx;
		r->mtile2 = r->my << 22 | r->my << 12 | r->my;
	} else {
		r->mtile1 = BIT(31) | r->mx << 22 | 2 * r->mx << 12 | 3 * r->mx;
		r->mtile2 = r->my << 22 | 2 * r->my << 12 | 3 * r->my;
	}
	r->zls_xt = xt;
	r->padx = mx * n;
	r->pady = my * n;
	r->te_screen = (r->yt - 1) << 12 | (r->xt - 1);
	r->mt_tiles = r->mx * r->my;
	r->rt_mt_tiles = mx * my;
	r->region_bytes = r->padx * r->pady * 12;
	m = max(r->padx, r->pady) - 1;
	for (big = 1; m; m >>= 1)
		big <<= 1;
	r->tailptr_bytes = big * big * 4;
	r->te_aa = aa == 2 ? TE_AA_4X : TE_AA;
	r->rt_flags = aa == 2 ? RT_FLAGS_4X : RT_FLAGS;
}

static void rtds_release(struct kref *ref)
{
	struct pvrsgx_rt_dataset *s = container_of(ref, struct pvrsgx_rt_dataset, ref);
	struct pvrsgx_device *sdev = s->ctx->sdev;

	pvrsgx_kobj_drop(sdev, &s->rtword);
	pvrsgx_kobj_drop(sdev, &s->rtdata);
	pvrsgx_kobj_drop(sdev, &s->done);
	pvrsgx_kobj_drop(sdev, &s->tailptr);
	pvrsgx_kobj_drop(sdev, &s->ta40);
	pvrsgx_kobj_drop(sdev, &s->ta50);
	for (int i = 0; i < 2; i++) {
		pvrsgx_kobj_drop(sdev, &s->bgblock[i]);
		pvrsgx_kobj_drop(sdev, &s->rd[i]);
		pvrsgx_kobj_drop(sdev, &s->rd_w8[i]);
		pvrsgx_kobj_drop(sdev, &s->rd_w4[i]);
		pvrsgx_kobj_drop(sdev, &s->hwsync[i]);
		pvrsgx_kobj_drop(sdev, &s->rd_sync[i]);
		pvrsgx_kobj_drop(sdev, &s->hwsync_access[i]);
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
/* One RT-data entry's render details and dependency block, as new. */
static void rtds_init_entry(struct pvrsgx_rt_dataset *s, int i)
{
	struct pvrsgx_render_ctx *r = s->ctx->render;
	struct pvrsgx_fw_render_details *rd = (void *)kobj_cpu(s->rd[i]);

	memset(rd, 0, sizeof(*rd));
	/* Render details; the per-render words are written at each job. */
	rd->req_base_3d = r->pb->base;
	rd->unk_08 = 0x80;
	rd->unk_0c = 0x000f0000;
	rd->rt_ctl = BIT(31) | (s->samples == 4 ? BIT(1) : 0);
	rd->stencil[0] = BIT(31);
	rd->stencil[1] = BIT(31);
	rd->bg_depth = f32_one();
	rd->unk_44 = 0x300;
	rd->bg_object = 0x2000000 | (kobj_va(s->bgblock[i]) - r->pb->base) >> 4;
	rd->rt_flags = s->n.rt_flags;
	rd->unk_58 = 0x30;
	rd->unk_64 = 1;
	rd->unk_68 = 1;
	rd->unk_cc = 0x100;
	rd->sync = kobj_va(s->rd_sync[i]);
	rd->rt_data = kobj_va(s->rtdata);
	rd->unk_104 = 1;
	rd->word8 = kobj_va(s->rd_w8[i]);
	rd->bg_pds_word = kobj_va(s->rd_w4[i]);
	rd->unk_13c = 0x07c00000;
	kobj_cpu(s->rd_sync[i])[0] = 1;
	kobj_cpu(s->hwsync[i])[0] = kobj_va(s->hwsync_access[i]);
	kobj_cpu(s->hwsync_access[i])[0] = 1;
}

static void rtds_init(struct pvrsgx_rt_dataset *s)
{
	struct pvrsgx_render_ctx *r = s->ctx->render;
	struct pvrsgx_fw_rt_data *rt = (void *)kobj_cpu(s->rtdata);
	u32 w = s->w, h = s->h, *b;

	memset(rt, 0, sizeof(*rt));
	for (int i = 0; i < 2; i++) {
		memset(kobj_cpu(s->bgblock[i]), 0, 0x60);
		kobj_cpu(s->entword[i])[0] = 0;
	}
	s->frames = 0;

	kobj_cpu(s->rtword)[0] = 1;
	rt->rt_word = kobj_va(s->rtword);
	rt->padded_tiles_x = s->n.padx;
	rt->num_entries = ARRAY_SIZE(rt->entry);
	for (int i = 0; i < ARRAY_SIZE(rt->entry); i++) {
		struct pvrsgx_fw_rt_entry *e = &rt->entry[i];

		e->hw_render_ctx = kobj_va(r->hwctx);
		e->mt_units = s->n.rt_mt_tiles >> 4;
		e->mt_bytes = s->n.rt_mt_tiles * 0xc;
		e->bg_block = kobj_va(s->bgblock[i]);
		e->ta_data[0] = kobj_va(s->ta88[i]);
		e->ta_data[1] = kobj_va(s->ta40);
		e->ta_data[2] = kobj_va(s->ta50);
		e->tail_pointers = kobj_va(s->tailptr);
		e->tail_pointers_size = s->n.tailptr_bytes;
		e->num_rts = 1;
		e->entry_word = kobj_va(s->entword[i]);
		e->unk_34 = 0x10;
	}

	for (int i = 0; i < 2; i++) {
		/* Background object: screen-space vertices, fixed point offset by 0x1000. */
		b = kobj_cpu(s->bgblock[i]);
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
		rtds_init_entry(s, i);
	}
}

struct pvrsgx_rt_dataset *pvrsgx_rtds_create(struct pvrsgx_context *ctx, u32 w, u32 h, u32 samples)
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
	s->samples = samples;
	rt_numbers(&s->n, w, h, samples);

	err = pvrsgx_kobj_new(sdev, &s->rtword, PVRSGX_KRANGE_KDATA, 4, 0, NULL);
	err = err ?: pvrsgx_kobj_new(sdev, &s->rtdata, PVRSGX_KRANGE_KDATA,
				     sizeof(struct pvrsgx_fw_rt_data), 0, NULL);
	err = err ?: pvrsgx_kobj_new(sdev, &s->done, PVRSGX_KRANGE_SYNC, SYNC_DATA_BYTES, 0, NULL);
	/* What the TA writes and the 3D reads, under the VM's directory too. */
	err = err ?: pvrsgx_kobj_new(sdev, &s->tailptr, PVRSGX_KRANGE_TADATA,
				     s->n.tailptr_bytes, 0, vm);
	err = err ?: pvrsgx_kobj_new(sdev, &s->ta40, PVRSGX_KRANGE_TADATA, 0x40, 0, vm);
	err = err ?: pvrsgx_kobj_new(sdev, &s->ta50, PVRSGX_KRANGE_TADATA, 0x50, 0, vm);
	for (int i = 0; i < 2; i++) {
		err = err ?: pvrsgx_kobj_new(sdev, &s->bgblock[i], PVRSGX_KRANGE_3DPARAMS, 0x200,
					     r->pb->base, vm);
		err = err ?: pvrsgx_kobj_new(sdev, &s->rgn[i], PVRSGX_KRANGE_TADATA,
					     s->n.region_bytes, 0, vm);
		err = err ?: pvrsgx_kobj_new(sdev, &s->ta88[i], PVRSGX_KRANGE_TADATA, 0x88, 0, vm);
		err = err ?: pvrsgx_kobj_new(sdev, &s->entword[i], PVRSGX_KRANGE_KDATA, 4, 0, NULL);
		err = err ?: pvrsgx_kobj_new(sdev, &s->rd[i], PVRSGX_KRANGE_KDATA,
					     sizeof(struct pvrsgx_fw_render_details), 0, NULL);
		err = err ?: pvrsgx_kobj_new(sdev, &s->rd_w8[i], PVRSGX_KRANGE_KDATA, 8, 0, NULL);
		err = err ?: pvrsgx_kobj_new(sdev, &s->rd_w4[i], PVRSGX_KRANGE_KDATA, 4, 0, NULL);
		err = err ?: pvrsgx_kobj_new(sdev, &s->hwsync[i], PVRSGX_KRANGE_KDATA,
					     sizeof(struct pvrsgx_fw_rt_deps), 0, NULL);
		err = err ?: pvrsgx_kobj_new(sdev, &s->rd_sync[i], PVRSGX_KRANGE_SYNC, 8, 0, NULL);
		err = err ?: pvrsgx_kobj_new(sdev, &s->hwsync_access[i], PVRSGX_KRANGE_SYNC, 8, 0,
					     NULL);
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

	if (args->flags || (args->samples > 1 && args->samples != 4) || !args->width ||
	    !args->height ||
	    args->width > RT_MAX_DIM || args->height > RT_MAX_DIM)
		return -EINVAL;
	if (args->samples > to_pvrsgx_device(drm)->family->cmds->max_samples)
		return -EOPNOTSUPP;
	ctx = pvrsgx_context_lookup(sfile, args->context_handle);
	if (!ctx)
		return -ENOENT;
	if (!ctx->render) {
		pvrsgx_context_put(ctx);
		return -EINVAL;
	}
	s = pvrsgx_rtds_create(ctx, args->width, args->height, args->samples ?: 1);
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

bool pvrsgx_cmd_mapped(const void *priv, u64 addr, u64 size)
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

/* The core's command of @type: what a client's command is checked against. */
const struct pvrsgx_cmd_def *pvrsgx_cmd_def(struct pvrsgx_device *sdev, enum pvrsgx_cmd_type type)
{
	return sdev->family->cmds->cmds[type];
}

/* A copied command, field by field, then against the VM (under its lock). */
int pvrsgx_cmd_take(struct pvrsgx_device *sdev, const struct pvrsgx_cmd_def *def,
		    const struct pvrsgx_cmd_ctx *ctx, struct pvrsgx_vm *vm, const void *all,
		    u64 present)
{
	const struct pvrsgx_cmd_core *core = sdev->family->cmds;
	int err = pvrsgx_cmd_check(core, def, ctx, all, present);

	if (err)
		return err;
	mutex_lock(&vm->lock);
	err = pvrsgx_cmd_check_mapped(core, def, ctx, all, present);
	mutex_unlock(&vm->lock);
	return err;
}

int pvrsgx_render_job_init(struct drm_file *file, struct pvrsgx_job *job,
			   const struct drm_powervr_sgx_job *in)
{
	struct pvrsgx_device *sdev = job->ctx->sdev;
	const struct pvrsgx_cmd_def *rdef = pvrsgx_cmd_def(sdev, PVRSGX_CMD_RENDER);
	const struct pvrsgx_cmd_def *kdef = pvrsgx_cmd_def(sdev, PVRSGX_CMD_TA_KICK);
	struct pvrsgx_render_job *rj = &job->render;
	struct drm_powervr_sgx_cmd_ta_kick *kicks;
	struct pvrsgx_vm *vm = job->ctx->vm;
	struct pvrsgx_cmd_ctx cctx = {};
	u64 present;
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
	/* Scenes of more than two kicks need the middle-kick flags, not known yet. */
	if (in->ta_kicks.count > PVRSGX_MAX_TA_KICKS)
		return -EINVAL;

	if (!rdef || !kdef)
		return -EOPNOTSUPP;

	cctx.heaps[PVRSGX_CMD_HEAP_PDS_PIXEL].base = pvrsgx_heaps(sdev)->pds_pixel.base;
	cctx.heaps[PVRSGX_CMD_HEAP_PDS_PIXEL].size =
		pvrsgx_heaps(sdev)->pds_pixel.end - pvrsgx_heaps(sdev)->pds_pixel.base;
	cctx.limits[PVRSGX_CMD_LIMIT_TILES_X] = rj->rtds->n.xt - 1;
	cctx.limits[PVRSGX_CMD_LIMIT_TILES_Y] = rj->rtds->n.yt - 1;
	cctx.rt_pad_width = rj->rtds->n.xt * 32;
	cctx.rt_pad_height = rj->rtds->n.yt * 16;
	cctx.rt_samples = rj->rtds->samples;
	cctx.mapped = pvrsgx_cmd_mapped;
	cctx.priv = vm;

	err = pvrsgx_cmd_copy(rdef, in->cmd, &rj->render, &present);
	err = err ?: pvrsgx_cmd_take(sdev, rdef, &cctx, vm, &rj->render, present);
	err = err ?: pvrsgx_check_render(sdev->family->cmds, &rj->render, rj->rtds->samples);
	if (err)
		return err;

	kicks = pvrsgx_obj_array_get(&in->ta_kicks, sizeof(*kicks), PVRSGX_MAX_TA_KICKS);
	if (IS_ERR(kicks))
		return PTR_ERR(kicks);
	rj->nkicks = in->ta_kicks.count;
	for (u32 k = 0; k < rj->nkicks && !err; k++) {
		err = pvrsgx_cmd_take_elem(kdef, &kicks[k], in->ta_kicks.stride, &rj->kick[k],
					   &present);
		err = err ?: pvrsgx_cmd_take(sdev, kdef, &cctx, vm, &rj->kick[k], present);
	}
	kvfree(kicks);
	return err;
}

void pvrsgx_render_job_fini(struct pvrsgx_job *job)
{
	pvrsgx_rtds_put(job->render.rtds);
	job->render.rtds = NULL;
}

static u32 pds_word(u64 pds, u64 addr, u32 rows)
{
	return rows << 26 | (u32)(addr - pds) >> 4;
}

/*
 * Write the TA command for kick k of the job's scene (and, at the first, the
 * render details), and say where the microkernel will report the scene done.
 * Runs in run_job: one job on the GPU at a time, so the context's CCB and the
 * dataset are ours.
 *
 * One kick both starts the scene and renders it (TA flags 0x63, the form Mesa
 * uses). A single-draw frame can also take two kicks: 0x71 runs the stream,
 * 0x62 runs the terminate words and starts the 3D.
 */
/*
 * A restart (a reset, or a runtime resume) starts a fresh microkernel, but a
 * VM's PB lives through it. After a hang the pages the TA took are still off
 * its free list: the next render on any of the VM's contexts hangs, and the
 * next restart fails. The PB's first job after a restart sets it up as new.
 * Nothing else needs it: the HW contexts and RT datasets
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
	const struct drm_powervr_sgx_cmd_render *in = &rj->render.base;
	const struct drm_powervr_sgx_ext_sgx545_render *x545 = &rj->render.sgx545_render;
	bool first = k == 0, last = k == rj->nkicks - 1;
	struct pvrsgx_fw_ta_cmd *cmd;
	struct pvrsgx_fw_rt_deps *deps;
	struct pvrsgx_fw_render_details *rd;
	u32 f, odd, p, rgn_this, rgn_next, entry, *s3d, *done, *b;
	u32 woff = r->ccb_woff, zls_base = 0;
	u64 pds = pvrsgx_heaps(job->ctx->sdev)->pds_pixel.base;

	if (first)
		pb_rearm(job->ctx->sdev, r->pb);
	f = s->frames;
	odd = f & 1;
	/* This frame's RT-data entry and everything that goes with it. */
	p = odd ? 0 : 1;
	rgn_this = kobj_va(s->rgn[p]);
	rgn_next = kobj_va(s->rgn[!p]);
	entry = kobj_va(s->rtdata) + 0x18 + 0x38 * p;
	rd = (struct pvrsgx_fw_render_details *)kobj_cpu(s->rd[p]);
	if (first) {
		/* Render details. */
		if (in->depth_addr || in->stencil_addr) {
			u64 lo = in->depth_addr ?: in->stencil_addr;

			if (in->depth_addr && in->stencil_addr)
				lo = min(in->depth_addr, in->stencil_addr);

			zls_base = (u32)lo & 0xfff00000;
		}
		rd->zls_base = zls_base;
		rd->depth_bias_units = in->depth_bias_units;
		rd->depth_bias_factor = in->depth_bias_factor_f32;
		rd->region_headers = rgn_this;
		rd->region_headers_next = rgn_next;
		rd->zls_ctl = x545->zls_control ? x545->zls_control | (s->n.zls_xt - 1) << 3 : 0;
		rd->depth[0] = in->depth_addr ? (u32)in->depth_addr - zls_base : 0;
		rd->depth[1] = rd->depth[0];
		rd->stencil[0] = BIT(31) |
				 (in->stencil_addr ? (u32)in->stencil_addr - zls_base : 0);
		rd->stencil[1] = rd->stencil[0];
		rd->bg_depth = in->bg_depth_f32;
		rd->pixel_event_pds = (u32)in->pixel_event_pds_addr;
		rd->pixel_event_rows = in->pixel_event_pds_rows;
		rd->rt_data_entry = entry;
		kobj_cpu(s->rd_w4[p])[0] = pds_word(pds, in->bg_pds_primary_addr,
						    in->bg_pds_primary_rows);

		/*
		 * The background object's PDS words (secondary, middle, primary),
		 * written by the host so the microkernel never runs on the job's
		 * directory.
		 */
		b = kobj_cpu(s->bgblock[p]);
		b[0] = pds_word(pds, in->bg_pds_secondary_addr, in->bg_pds_secondary_rows);
		b[1] = BG_PDS_MIDDLE;
		b[2] = kobj_cpu(s->rd_w4[p])[0];

		/* What the microkernel clears and the client sets before a scene. */
		kobj_cpu(s->rtword)[0] = f + 1;
		kobj_cpu(s->rd_sync[p])[0] = 1;
		kobj_cpu(s->hwsync_access[p])[0] = 1;
	}

	/* The TA command, in the context's ring. */
	cmd = (struct pvrsgx_fw_ta_cmd *)((u8 *)kobj_cpu(r->ccb) + woff);
	memset(cmd, 0, sizeof(*cmd));
	cmd->size = sizeof(*cmd);
	cmd->ta_flags = PVRSGX_TA_FLAGS_BASE | (first ? PVRSGX_TA_FIRST_KICK : 0) |
			(last ? PVRSGX_TA_LAST_KICK : 0) |
			(first && !f ? PVRSGX_TA_FIRST_RENDER : 0);
	cmd->bg_pds[0] = pds_word(pds, in->bg_pds_secondary_addr, in->bg_pds_secondary_rows);
	cmd->bg_pds[1] = BG_PDS_MIDDLE;
	cmd->bg_pds[2] = pds_word(pds, in->bg_pds_primary_addr, in->bg_pds_primary_rows);
	cmd->ta_ctl[0] = TA_CTL;
	cmd->ta_ctl[1] = TA_CTL;
	if (last)
		cmd->flags_3d = x545->flags_3d | PVRSGX_3D_RENDER | f << PVRSGX_3D_FRAME_SHIFT;
	cmd->frame = f;
	cmd->pb_desc = kobj_va(r->pb->desc);
	cmd->render_details = kobj_va(s->rd[p]);
	cmd->rt_deps = kobj_va(s->hwsync[p]);
	cmd->rt_data = kobj_va(s->rtdata);
	cmd->rt_data_entry = entry;

	/* Ready; the 3D sync on the last kick; the completion word on the first. */
	cmd->ready = 1;
	if (last) {
		s3d = kobj_cpu(r->sync3d);
		cmd->sync3d_write_pending = s3d[SYNC_WRITE_PENDING];
		cmd->sync3d_write_complete = kobj_va(r->sync3d) + SYNC_WRITE_COMPLETE * 4;
		cmd->sync3d_read_pending = s3d[SYNC_READ_PENDING]++;
		cmd->sync3d_read_complete = kobj_va(r->sync3d) + SYNC_READ_COMPLETE * 4;
	}
	if (first) {
		done = kobj_cpu(s->done);
		deps = (struct pvrsgx_fw_rt_deps *)kobj_cpu(s->hwsync[p]);
		deps->num_dst = 1;
		deps->read_pending = done[SYNC_READ_PENDING];
		deps->read_complete = kobj_va(s->done) + SYNC_READ_COMPLETE * 4;
		deps->write_pending = done[SYNC_WRITE_PENDING]++;
		deps->write_complete = kobj_va(s->done) + SYNC_WRITE_COMPLETE * 4;
		/* The microkernel sets write-complete to the pending value plus one. */
		job->done_cpu = &done[SYNC_WRITE_COMPLETE];
		job->done_val = deps->write_pending + 1;
	}

	cmd->te_aa = s->n.te_aa;
	cmd->te_mtile[0] = s->n.mtile1;
	cmd->te_mtile[1] = s->n.mtile2;
	cmd->te_screen = s->n.te_screen;
	cmd->mt_tiles = s->n.mt_tiles;
	cmd->ta_flags2 = rj->kick[k].sgx545_ta_kick.ta_flags2 | (last ? PVRSGX_TA2_RENDER : 0);
	cmd->vdm_base = (u32)rj->kick[k].base.vdm_base;
	cmd->vdm_ctl = VDM_CTL;
	cmd->region_headers = rgn_this;
	cmd->tail_pointers = kobj_va(s->tailptr);
	cmd->eps[0] = F32_1E_6;
	cmd->eps[1] = F32_1E_6;
	cmd->screen_size = (s->h - 1) << 16 | (s->w - 1);
	cmd->params_base = r->pb->base;
	cmd->rt_flags = s->n.rt_flags;

	r->ccb_woff = (woff + sizeof(*cmd)) & (CCB_RING - 1);
	/* The command is in memory before the write offset that hands it over. */
	wmb();
	kobj_cpu(r->ccbctl)[0] = r->ccb_woff;
	if (last) {
		rj->cmd_off = woff;
		rj->entry = p;
		trace_pvrsgx_render_kick(cmd, s->w, s->h, rgn_next, woff);
		s->frames++;
	}

	memset(data, 0, 6 * sizeof(u32));
	data[1] = kobj_va(r->hwctx);
	return 0;
}

/* ---------------------------------------------------------------- transfers */

/* The structures of a render, word by word. */
static void render_dump(struct seq_buf *sb, struct pvrsgx_device *sdev, struct pvrsgx_rt_dataset *s,
			u32 cmd_off, u32 p)
{
	struct pvrsgx_render_ctx *r = s->ctx->render;

	seq_buf_printf(sb, "PB 0x%08x pages 0x%x base 0x%08x; free list 0x%08x; scratch 0x%08x\n",
		       kobj_va(r->pb->buf), r->pb->pages, r->pb->base, kobj_va(r->pb->freelist),
		       kobj_va(r->scratch));
	pvrsgx_kobj_dump(sb, "hwctx", r->hwctx, 0, sizeof(struct pvrsgx_fw_hw_render_ctx));
	pvrsgx_kobj_dump(sb, "ccbctl", r->ccbctl, 0, 8);
	pvrsgx_kobj_dump(sb, "pbdesc", r->pb->desc, 0, sizeof(struct pvrsgx_fw_pb_desc));
	pvrsgx_kobj_dump(sb, "pbblocks", r->pb->blocks, 0, 0x10);
	pvrsgx_kobj_dump(sb, "freelist[first..]", r->pb->freelist,
			 ((kobj_va(r->pb->buf) - pvrsgx_heaps(sdev)->params_3d.base) >>
			  PAGE_SHIFT) * 4, 0x20);
	pvrsgx_kobj_dump(sb, "scratch", r->scratch, 0, 0x40);
	pvrsgx_kobj_dump(sb, "sync3d", r->sync3d, 0, SYNC_DATA_BYTES);
	pvrsgx_kobj_dump(sb, "ta_cmd", r->ccb, cmd_off, sizeof(struct pvrsgx_fw_ta_cmd));
	seq_buf_printf(sb, "rt-data entry %u\n", p);
	pvrsgx_kobj_dump(sb, "render_details", s->rd[p], 0,
			 sizeof(struct pvrsgx_fw_render_details));
	pvrsgx_kobj_dump(sb, "rd_w4", s->rd_w4[p], 0, 4);
	pvrsgx_kobj_dump(sb, "rd_w8", s->rd_w8[p], 0, 8);
	pvrsgx_kobj_dump(sb, "rd_sync", s->rd_sync[p], 0, 4);
	pvrsgx_kobj_dump(sb, "rtdata", s->rtdata, 0, sizeof(struct pvrsgx_fw_rt_data));
	pvrsgx_kobj_dump(sb, "rtword", s->rtword, 0, 4);
	pvrsgx_kobj_dump(sb, "entword0", s->entword[0], 0, 4);
	pvrsgx_kobj_dump(sb, "entword1", s->entword[1], 0, 4);
	pvrsgx_kobj_dump(sb, "hwsync", s->hwsync[p], 0, sizeof(struct pvrsgx_fw_rt_deps));
	pvrsgx_kobj_dump(sb, "hwsync_access", s->hwsync_access[p], 0, 4);
	pvrsgx_kobj_dump(sb, "done", s->done, 0, SYNC_DATA_BYTES);
	pvrsgx_kobj_dump(sb, "bgblock", s->bgblock[p], 0, 0x60);
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
		render_dump(sb, sdev, job->render.rtds, job->render.cmd_off, job->render.entry);
	else if (job->type == DRM_POWERVR_SGX_JOB_TYPE_TRANSFER)
		pvrsgx_transfer_dump(sb, job);
}

#if IS_ENABLED(CONFIG_DRM_POWERVR_SGX_KUNIT_TEST)
#include "tests/pvrsgx_kobj_test.c"
#include "tests/pvrsgx_rt_test.c"
#endif
