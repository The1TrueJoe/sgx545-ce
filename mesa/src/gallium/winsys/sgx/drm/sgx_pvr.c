/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * sgxws over the DDK's services driver (pvrsrvkm), through libpvr: the client
 * builds the structures the microkernel reads (PB, HW render context, RT
 * data, render details, the TA command in the CCB) as the DDK's GL driver
 * does.  the DDK's services stack; never upstream (it is
 * built against the kernel's bridge headers).
 *
 * Tags in the comments follow docs/STATE.md: [C] proven by a replay,
 * [D] from a diff of captures, [G] guess.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pvr.h"
#include "sgx_frame.h"

/* the CCB is a 64 KB ring with room for one command past its end [G] */
#define CCB_RING    0x10000
#define CCB_SIZE    0x10420
#define TA_CMD_SIZE 0x2c0

#define H_GENERAL  SGX_HEAP_GENERAL
#define H_TADATA   SGX_HEAP_TADATA
#define H_KDATA    SGX_HEAP_KDATA
#define H_SYNC     SGX_HEAP_SYNC
#define H_3DPARAMS SGX_HEAP_3DPARAMS

struct pvr_dev {
   struct sgxws_dev base;
   struct pvr pvr;
};

struct pvr_bo {
   struct sgxws_bo_impl base;
   struct pvr_buf b;
   struct pvr_buf sync_map; /* the mapping of its sync data, if any */
   volatile PVRSRV_SYNC_DATA
      *sd; /* render targets: the sync data the CPU waits on */
};

/* a submitted render: done when its sync's completed writes reach value */
struct pvr_token {
   volatile PVRSRV_SYNC_DATA *sd;
   uint32_t value;
   /* a render's: when it was kicked, and its target's render time (NULL:
    * unknown) */
   uint64_t kicked_ns, *render_ns;
};

struct pvr_ctx {
   struct sgxws_bo pb, freelist, pbdesc, pbblocks, ccb, ccbctl, scratch, sync3d;
   uint32_t pb_pages;
   IMG_HANDLE hwctx;
   uint32_t hwctx_va;
   uint32_t ccb_woff;
   struct pvr_token token[SGXWS_MAX_SLOTS]; /* by kick number % c->slots */
};

/* The TA/3D structures SGXAddRenderTarget makes for one target. */
struct pvr_rt {
   struct sgxws_bo rtdata, rtword, entword[2], tailptr, ta40, ta50, rgn[2],
      ta88[2], bgblock;
   struct sgxws_bo rd, rd_sync, rd_w8, rd_w4, hwsync, hwsync_access;
   uint64_t render_ns; /* a render into it, kick to done (running average) */
};

static const struct sgxws_kif pvr_kif;

static struct pvr *
dev_pvr(struct sgxws_dev *d)
{
   return &((struct pvr_dev *)d)->pvr;
}

static struct pvr_bo *
pbo(const struct sgxws_bo *bo)
{
   return (struct pvr_bo *)bo->impl;
}

/* ------------------------------------------------------------------ memory */

static int
pvr_bo_alloc(struct sgxws_dev *d, struct sgxws_bo *bo, enum sgx_heap heap,
             uint32_t attribs, uint32_t size, uint32_t align)
{
   struct pvr_bo *impl = calloc(1, sizeof(*impl));
   int e;

   if (!impl)
      return -ENOMEM;
   if ((e = pvr_alloc(dev_pvr(d), &impl->b, heap, attribs, size, align))) {
      mesa_loge("sgx: ALLOC_DEVICEMEM heap %u size %#x failed: %d", heap, size,
                e);
      free(impl);
      return e;
   }
   if ((e = pvr_map(dev_pvr(d), &impl->b))) {
      mesa_loge("sgx: map of %#x failed: %d", impl->b.devva, e);
      pvr_free(dev_pvr(d), &impl->b);
      free(impl);
      return e;
   }
   impl->base.kif = &pvr_kif;
   impl->base.size = impl->b.size;
   bo->impl = &impl->base;
   bo->va = impl->b.devva;
   bo->map = impl->b.cpu;
   memset(bo->map, 0, size);
   if (d->verbose)
      mesa_logi("sgx: heap %2u %#010x +%#x", heap, bo->va, size);
   return 0;
}

/* a render target's sync data, mapped once: the CPU waits on it before reading */
static void
pvr_bo_prepare_target(struct sgxws_dev *d, struct sgxws_bo *bo)
{
   struct pvr_bo *impl = pbo(bo);

   if (!impl->sd)
      impl->sd = pvr_sync_data(dev_pvr(d), impl->b.sync, &impl->sync_map);
}

static int
pvr_bo_wrap(struct sgxws_dev *d, struct sgxws_bo *bo, void *cpu, uint32_t size)
{
   struct pvr_bo *impl = calloc(1, sizeof(*impl));
   int e;

   if (!impl)
      return -ENOMEM;
   if ((e = pvr_wrap(dev_pvr(d), &impl->b, cpu, size))) {
      mesa_loge("sgx: WRAP_EXT_MEMORY of %u bytes failed: %d", size, e);
      free(impl);
      return e;
   }
   impl->base.kif = &pvr_kif;
   impl->base.size = impl->b.size;
   bo->impl = &impl->base;
   bo->va = impl->b.devva;
   bo->map = impl->b.cpu;
   pvr_bo_prepare_target(d, bo);
   return 0;
}

static void
pvr_bo_free(struct sgxws_dev *d, struct sgxws_bo *bo)
{
   struct pvr_bo *impl = pbo(bo);

   if (impl->sd)
      pvr_unmap(dev_pvr(d), &impl->sync_map);
   pvr_free(dev_pvr(d), &impl->b);
   free(impl);
}

static bool
token_done(const struct pvr_token *t)
{
   return !t->sd || (int32_t)(t->sd->ui32WriteOpsComplete - t->value) >= 0;
}

/* Poll a render's completion.  The counter is polled, not waited on, so the
 * poll's steps round the wait up: at 50 us steps doubling to 1 ms a 3 ms
 * render was seen done at 3.55 ms (perfmicro tex 0.71 of the DDK on the EA3,
 * 0.9 at 50 us steps), and 0.1 ms steps all the way were thousands of
 * wakeups.  So a render whose kick time is known is slept on until just
 * before it should be done (from its target's last renders), then polled
 * every 50 us; other waits (a buffer's: a readback) poll every 50 us for
 * 4 ms, then at steps doubling to 1 ms. */
static int
token_wait(struct pvr_token *t, unsigned ms)
{
   uint64_t waited = 0, step = 50000, limit = (uint64_t)ms * 1000000;
   bool known = false;

   if (!token_done(t) && t->render_ns && *t->render_ns) {
      uint64_t now = sgxws_now_ns(), due = t->kicked_ns + *t->render_ns * 3 / 4;

      if (due > now && due - now < limit) {
         struct timespec ts = {(time_t)((due - now) / 1000000000),
                               (long)((due - now) % 1000000000)};

         nanosleep(&ts, NULL);
         waited += due - now;
      }
      known = true;
   }
   while (!token_done(t) && waited < limit) {
      struct timespec ts = {0, (long)step};

      nanosleep(&ts, NULL);
      waited += step;
      if (!known && waited >= 4000000)
         step = step * 2 < 1000000 ? step * 2 : 1000000;
   }
   if (!token_done(t))
      return -ETIMEDOUT;
   if (t->render_ns) {
      uint64_t took = sgxws_now_ns() - t->kicked_ns;

      *t->render_ns = *t->render_ns ? (*t->render_ns * 3 + took) / 4 : took;
      t->render_ns = NULL;
   }
   t->sd = NULL;
   return 0;
}

/* wait for the renders that write a buffer (its sync's pending writes) */
static int
pvr_bo_wait(const struct sgxws_bo *bo, unsigned timeout_ms)
{
   volatile PVRSRV_SYNC_DATA *sd = pbo(bo)->sd;
   struct pvr_token t = {sd, sd ? sd->ui32WriteOpsPending : 0};

   return token_wait(&t, timeout_ms);
}

/* For the structures whose placement matters (the kernel-data and sync heap
 * order, see pvr_ctx_init), a failed allocation leaves nothing to fall back
 * to. */
#define BO(d, bo, heap, attribs, size, align)                                  \
   do {                                                                        \
      if (sgxws_bo_alloc_heap(d, bo, heap, attribs, size, align))              \
         goto fail;                                                            \
   } while (0)

/* ------------------------------------------------------------------ context */

/* The parameter buffer (STATE.md section 6, "PB descriptor"). */
static int
ctx_pb(struct sgxws_ctx *c, struct pvr_ctx *k, uint32_t bytes)
{
   struct sgxws_dev *d = c->dev;
   uint32_t pages = (bytes + 0x22000) >> 12, first, last, *fl, *pd;

   BO(d, &k->pbdesc, H_KDATA, 0xf, 0x3c, 0x20);
   BO(d, &k->pbblocks, H_KDATA, 0xf, 0x10, 4);
   BO(d, &k->pb, H_3DPARAMS, 0xb, pages << 12, 0x1000);
   first = (k->pb.va - d->heap_base[H_3DPARAMS]) >> 12;
   last = first + pages - 1;
   /* the free list is indexed by page number from the heap base: prev<<16 |
    * next; the DDK always allocates 0x40000, one u32 per page of the whole 256
    * MB heap */
   BO(d, &k->freelist, H_3DPARAMS, 0xb,
      (last + 1) * 4 > 0x40000 ? (last + 1) * 4 : 0x40000, 0x10);
   fl = k->freelist.map;
   for (uint32_t i = first; i <= last; i++)
      fl[i] =
         (i == first ? 0xffffu : i - 1) << 16 | (i == last ? 0xffffu : i + 1);
   /* block list: {pages, first | last<<16, PB descriptor, 0} */
   k->pbblocks.map[0] = pages;
   k->pbblocks.map[1] = first | last << 16;
   k->pbblocks.map[2] = k->pbdesc.va;
   pd = k->pbdesc.map;
   uint32_t zls = (pages - 0x22) & 0xfffff, ta = pages + 0x10,
            pds = zls < ta ? zls : ta;

   pd[1] = pages;
   pd[2] = last << 16 | first; /* free list head */
   pd[3] = (last - 1) << 16;
   pd[4] = k->freelist.va;
   pd[6] = pd[2];
   pd[7] = pd[3];
   pd[10] = ta;                            /* TA threshold */
   pd[11] = zls;                           /* ZLS threshold */
   pd[12] = zls;                           /* global threshold */
   pd[13] = pds > 0x100 ? pds - 0x100 : 0; /* PDS threshold */
   pd[14] = k->pbblocks.va;
   k->pb_pages = pages;
   return 0;
fail:
   return -ENOMEM;
}

static int
pvr_ctx_init(struct sgxws_ctx *c)
{
   struct sgxws_dev *d = c->dev;
   struct pvr_ctx *k = calloc(1, sizeof(*k));
   struct sgxws_bo dummy;
   uint32_t hw[17] = {0};
   int e;

   if (!k)
      return -ENOMEM;
   c->k = k;
   if (ctx_pb(c, k, 0x400000))
      goto fail;
   BO(d, &k->ccb, H_KDATA, 0x4f, CCB_SIZE, 0x1000);
   BO(d, &k->ccbctl, H_KDATA, 0x4f, 8, 0x20);
   BO(d, &dummy, H_SYNC, 0x3, 4,
      0x10); /* SGXCreateRenderContext's own sync word */
   BO(d, &k->scratch, H_3DPARAMS, 0xb, 0x40, 0x40);
   BO(d, &dummy, H_SYNC, 0x4b, 0x80,
      0x20); /* and its 0x80-byte buffer for GL */
   /*
    * The kernel-data and sync allocations of the DDK's two transfer contexts,
    * which we don't use: they keep every later structure at the address the
    * DDK gives it.  The microkernel keeps pointers to the previous client's
    * render details and RT data and reads them when the next client's TA
    * starts; after a DDK client, a frame with any other layout hangs the TA
    * (docs/GEN.md, "Layout").
    */
   for (int i = 0; i < 2; i++) {
      BO(d, &dummy, H_KDATA, 0x4f, 0x104c8, 0x1000);
      BO(d, &dummy, H_KDATA, 0x4f, 8, 0x20);
      BO(d, &dummy, H_SYNC, 0x3, 4, 0x10);
      BO(d, i ? &dummy : &k->sync3d, H_SYNC, 0x3, 4,
         0x10); /* the DDK's 3D sync */
      BO(d, &dummy, H_KDATA, 0x4f, 4, 4);
   }

   /* HW render context (SGXCreateRenderContext): the kernel fills the PD at
    * word 2 */
   hw[0] = 0x21; /* b0, b5 = per-context PB [D] */
   hw[3] = k->ccb.va;
   hw[4] = k->ccbctl.va;
   hw[6] = k->pbdesc.va;
   hw[7] = k->scratch.va;
   hw[16] = (uint32_t)getpid();
   if ((e = pvr_register_render_ctx(dev_pvr(d), hw, sizeof(hw), 8, &k->hwctx,
                                    &k->hwctx_va))) {
      mesa_loge("sgx: SGX_REGISTER_HW_RENDER_CONTEXT failed: %d", e);
      goto fail;
   }
   return 0;

fail:
   free(k);
   c->k = NULL;
   return -ENOMEM;
}

static void
pvr_ctx_fini(struct sgxws_ctx *c)
{
   struct pvr_ctx *k = c->k;
   int e;

   if ((e = pvr_unregister_render_ctx(dev_pvr(c->dev), k->hwctx)))
      mesa_loge("sgx: SGX_UNREGISTER_HW_RENDER_CONTEXT failed: %d", e);
   /* the rest of the device memory goes with the services connection (resman) */
   free(k);
}

/* ------------------------------------------------------------------ render
 * target */

static int
pvr_rt_init(struct sgxws_ctx *c, struct sgxws_rt *rt)
{
   struct sgxws_dev *d = c->dev;
   struct pvr_ctx *k = c->k;
   struct pvr_rt *s = calloc(1, sizeof(*s));
   const struct sgxws_rt_numbers *r = &rt->n;
   uint32_t *rd, *rtd;

   if (!s)
      return -ENOMEM;

   /* the services path builds one-sample render targets only */
   if (rt->samples > 1) {
      free(s);
      return -EINVAL;
   }

   /* SGXAddRenderTarget: three render-details sets as the DDK makes them (the
    * third renders: this keeps the DDK's kernel-data addresses), RT data (2
    * entries), tail pointers, region headers (double buffered), TA data
    * blocks, the background object block */
   for (int i = 0; i < 3; i++) {
      BO(d, &s->rd, H_KDATA, 0xf, 0x1ec, 0x20);
      BO(d, &s->rd_sync, H_SYNC, 0xf, 8, 4);
      BO(d, &s->rd_w8, H_KDATA, 0xf, 8, 0x20);
      BO(d, &s->rd_w4, H_KDATA, 0xf, 4, 0x20);
      BO(d, &s->hwsync, H_KDATA, 0xf, 0x28, 0x20);
      BO(d, &s->hwsync_access, H_SYNC, 0xf, 8, 4);
   }
   BO(d, &s->rtword, H_KDATA, 0x4f, 4, 0x20);
   BO(d, &s->rtdata, H_KDATA, 0x4f, 2 * 0x38 + 0x18, 0x20);
   BO(d, &s->tailptr, H_TADATA, 0xb, r->tailptr_bytes, 0x40);
   BO(d, &s->ta40, H_TADATA, 0xb, 0x40, 0x10);
   BO(d, &s->ta50, H_TADATA, 0xb, 0x50, 0x1000);
   BO(d, &s->bgblock, H_3DPARAMS, 0x4b, 0x200, 0x1000);
   for (int i = 0; i < 2; i++) {
      BO(d, &s->rgn[i], H_TADATA, 0xb, r->region_bytes, 0x40);
      BO(d, &s->ta88[i], H_TADATA, 0xb, 0x88, 0x10);
      BO(d, &s->entword[i], H_KDATA, 0x4f, 4, 0x20);
   }

   /* RT data: header, then per entry {HW ctx, render details in flight (the
    * microkernel fills it), tiles/16, tiles*12, bg block, TA data
    * 0x88/0x40/0x50, tail pointers and size, RTs, 0, kdata word, 0x10} */
   s->rtword.map[0] = 1; /* the frame counter: entry (1 & 1) renders first */
   rtd = s->rtdata.map;
   rtd[0] = s->rtword.va;
   rtd[2] = r->padx;
   rtd[5] = 2;
   for (int i = 0; i < 2; i++) {
      uint32_t *e = rtd + 6 + 14 * i;

      e[0] = k->hwctx_va;
      e[2] = r->mt_tiles >> 4;
      e[3] = r->mt_tiles * 0xc;
      e[4] = s->bgblock.va;
      e[5] = s->ta88[i].va;
      e[6] = s->ta40.va;
      e[7] = s->ta50.va;
      e[8] = s->tailptr.va;
      e[9] = r->tailptr_bytes;
      e[10] = 1;
      e[12] = s->entword[i].va;
      e[13] = 0x10;
   }

   /* the background object's vertices in the PB (STATE.md section 6,
    * "Background object block"): fixed point, offset 0x1000 */
   {
      uint32_t *b = s->bgblock.map;

      b[5] = sgxws_f32(1.0f);
      b[8] = sgxws_f32(1.0f);
      b[10] = 0x202;
      b[15] = 0x100000;
      b[16] = 0x1000;
      b[17] = 0x3f80;
      b[18] = ((rt->w + 0x1000) * 0x100) << 8 | 0x10;
      b[19] = sgxws_f32(1.0f);
      b[20] = (rt->h + 0x1000) * 0x100;
      b[21] = 0x1000;
      b[22] = 0x3f80;
   }

   /* render details (STATE.md section 6); +0x20/+0x28 and the pixel event
    * program are set per frame */
   rd = s->rd.map;
   rd[0x00 / 4] = k->pb.va & 0xfff00000; /* 3D request base */
   rd[0x08 / 4] = 0x80;                  /* [G] */
   rd[0x0c / 4] = 0x000f0000;            /* [G] */
   rd[0x10 / 4] = 0x80000000;
   rd[0x38 / 4] = 0x80000000;      /* no depth load */
   rd[0x3c / 4] = 0x80000000;      /* no stencil load */
   rd[0x40 / 4] = sgxws_f32(1.0f); /* ISP background depth [C] */
   rd[0x44 / 4] = 0x300;           /* [G] */
   rd[0x48 / 4] = 0x2000000 | (s->bgblock.va - (k->pb.va & 0xfff00000)) >> 4;
   rd[0x50 / 4] = 0x88;
   rd[0x58 / 4] = 0x30;
   rd[0x64 / 4] = 1; /* [G] */
   rd[0x68 / 4] = 1;
   rd[0xcc / 4] = 0x100;
   rd[0xd0 / 4] = s->rd_sync.va;
   rd[0xd8 / 4] = s->rtdata.va;
   rd[0x104 / 4] = 1; /* [G] */
   rd[0x110 / 4] = s->rd_w8.va;
   rd[0x114 / 4] = s->rd_w4.va;
   rd[0x13c / 4] = 0x07c00000; /* [G] */
   s->rd_sync.map[0] = 1;
   s->hwsync.map[0] =
      s->hwsync_access.va; /* SGXMKIF_HWDEVICE_SYNC_LIST.sAccessDevAddr */
   s->hwsync_access.map[0] = 1;
   rt->k = s;
   return 0;

fail:
   free(s);
   return -ENOMEM;
}

static void
pvr_rt_fini(struct sgxws_ctx *c, struct sgxws_rt *rt)
{
   struct pvr_rt *s = rt->k;

   /* SGX_FLUSH_HW_RENDER_TARGET is what the DDK sends here.  It times out on a
    * srvinit-started microkernel and the GPU locks up after it (docs/GEN.md),
    * so only on request. */
   if (c->dev->flush_rt) {
      int e = pvr_flush_rt(dev_pvr(c->dev), s->rtdata.va);

      if (e)
         mesa_loge("sgx: SGX_FLUSH_HW_RENDER_TARGET %#x failed: %d",
                   s->rtdata.va, e);
   }
   /* The kernel-data structures stay allocated: the microkernel may still
    * hold pointers into them (docs/GEN.md, "Layout"). */
   sgxws_bo_free(c->dev, &s->tailptr);
   sgxws_bo_free(c->dev, &s->ta40);
   sgxws_bo_free(c->dev, &s->ta50);
   sgxws_bo_free(c->dev, &s->bgblock);
   for (int i = 0; i < 2; i++) {
      sgxws_bo_free(c->dev, &s->rgn[i]);
      sgxws_bo_free(c->dev, &s->ta88[i]);
   }
   free(s);
}

/* ------------------------------------------------------------------ submission
 */

/* A scene in one TA kick (0x63), or in several: the first resets the TA
 * (b0), the last (b1) starts the 3D and alone carries the 3D flags and TA
 * flags 2; b5-b6 are on every kick (ta3d-state, the DDK's multi-kick
 * scenes) */
enum ta_kick { TA_FIRST = 1, TA_LAST = 2 };

/*
 * SGXMKIF_CMDTA, client part (STATE.md section 1), for frame f of the render
 * target as the DDK's swaps do it (3- and 6-frame captures of a test client):
 * one kick, first and last in scene; TA flags 0x63, plus 0x10 on the target's
 * first frame; the frame number at +0x20 and at b14 of the 3D flags; the RT
 * data entry (in the command and in render details +0xdc) and the region
 * headers alternate, frame 0 taking entry 1 and the second region-header array.
 */
static void
ta_command(struct sgxws_ctx *c, uint32_t *cmd, unsigned kind)
{
   struct pvr_ctx *k = c->k;
   struct sgxws_rt *rt = c->rt;
   struct pvr_rt *s = rt->k;
   uint32_t f = rt->frames, odd = f & 1, *t = cmd + 0x238 / 4, *rd = s->rd.map;
   uint32_t rgn_this = s->rgn[odd ? 0 : 1].va,
            rgn_next = s->rgn[odd ? 1 : 0].va;
   uint32_t entry = s->rtdata.va + 0x18 + 0x38 * (odd ? 0 : 1);

   memset(cmd, 0, TA_CMD_SIZE);
   cmd[0x00 / 4] = TA_CMD_SIZE; /* size, rounded to 64 */
   cmd[0x04 / 4] = 0x60 | (kind & TA_FIRST ? 1 : 0) | (kind & TA_LAST ? 2 : 0) |
                   (f || !(kind & TA_FIRST) ? 0 : 0x10); /* TA flags [D] */
   cmd[0x08 / 4] = c->bg_pds[0];
   cmd[0x0c / 4] = c->bg_pds[1];
   cmd[0x10 / 4] = c->bg_pds[2];
   cmd[0x14 / 4] = 0x50000000;
   cmd[0x18 / 4] = 0x50000000;
   /* 3D flags: render, frame [D]; b7 and b8 (the DDK's kick b19-20) with a
    * depth/stencil buffer kept across renders, as the DDK's window renders */
   cmd[0x1c / 4] =
      kind & TA_LAST ? 0x400 | f << 14 | (c->target.zls_va ? 0x180 : 0) : 0;
   cmd[0x20 / 4] = f; /* the target's frame number [D] */
   cmd[0x24 / 4] = k->pbdesc.va;
   cmd[0x38 / 4] = s->rd.va;
   cmd[0x3c / 4] = s->hwsync.va;
   cmd[0x40 / 4] = s->rtdata.va;
   cmd[0x44 / 4] = entry;
   t[0x00 / 4] = 0x20000000; /* TE AA */
   t[0x04 / 4] = rt->n.mtile1;
   t[0x08 / 4] = rt->n.mtile2;
   t[0x0c / 4] = rt->n.te_screen;
   t[0x10 / 4] = rt->n.mt_tiles;
   t[0x14 / 4] =
      kind & TA_LAST ? 0x400000 : 0; /* TA flags 2: this kick renders */
   t[0x18 / 4] = c->vdm_start;
   t[0x1c / 4] = 0xa2; /* [G] */
   t[0x20 / 4] = rgn_this;
   t[0x24 / 4] = s->tailptr.va;
   t[0x28 / 4] = sgxws_f32(1e-6f);
   t[0x2c / 4] = sgxws_f32(1e-6f);
   t[0x30 / 4] = (rt->h - 1) << 16 | (rt->w - 1);
   t[0x3c / 4] = k->pb.va & 0xfff00000;
   t[0x6c / 4] = 0x88;

   /* ZLS (sgxws_target): base, control with the tile count, and the depth
    * and stencil addresses from the base, each in both of its words */
   if (c->target.zls_va) {
      uint32_t d = c->target.zls_va, base = d & 0xfff00000,
               st = d + sgxws_zls_pixels(rt->w, rt->h) * 4;

      rd[0x04 / 4] = base;
      rd[0x2c / 4] = SGXWS_ZLS_BASE | SGXWS_ZLS_STORE |
                     (c->target.zls_load ? SGXWS_ZLS_LOAD : 0) |
                     (rt->n.xt - 1) << 3;
      rd[0x30 / 4] = rd[0x34 / 4] = d - base;
      rd[0x38 / 4] = rd[0x3c / 4] = 0x80000000 | (st - base);
   } else {
      rd[0x04 / 4] = rd[0x2c / 4] = rd[0x30 / 4] = rd[0x34 / 4] = 0;
      rd[0x38 / 4] = rd[0x3c / 4] = 0x80000000;
   }
   rd[0x14 / 4] = (uint32_t)c->bias_units; /* depth bias */
   rd[0x18 / 4] = sgxws_f32(c->bias_factor);
   rd[0x20 / 4] = rgn_this; /* region headers of this render */
   rd[0x28 / 4] = rgn_next; /* and of the next */
   /* The RT data entry this render retires.  It must follow cmd+0x44: left
    * at entry 1 (as one frame needs), the microkernel retires the wrong entry
    * on odd frames, their PB pages are never freed, and the 3D locks up on
    * the fourth frame. */
   rd[0xdc / 4] = entry;

   /* What the DDK's client writes before every kick (a word-by-word diff of
    * its 6-frame capture against ours): the frame counter in the RT data
    * header's word, which picks the entry that renders (counter & 1), and the
    * render-details and HW sync-list access words, which the microkernel
    * clears.  Left alone, the counter stays at 1, odd frames retire the
    * wrong entry, their PB pages are never freed, and the 3D locks up on the
    * fourth frame. */
   s->rtword.map[0] = f + 1;
   s->rd_sync.map[0] = 1;
   s->hwsync_access.map[0] = 1;
}

static int
pvr_kick_ta_cmd(struct sgxws_ctx *c, uint32_t seq, unsigned kind)
{
   struct pvr_ctx *k = c->k;
   struct pvr_rt *s = c->rt->k;
   struct pvr_token *done = &k->token[seq % c->slots];
   struct pvr *pvr = dev_pvr(c->dev);
   SGX_CCB_KICK kick;
   IMG_HANDLE dst[1];
   uint32_t woff, *rd;
   int e;

   woff = k->ccb_woff;
   rd = s->rd.map;
   rd[0x5c / 4] = c->pixev;
   rd[0x60 / 4] = c->pixev_rows;
   s->rd_w4.map[0] = c->bg_pds[2];
   {
      uint64_t t0 = sgxws_now_ns();

      ta_command(c, k->ccb.map + woff / 4, kind);
      c->ns_cmd += sgxws_now_ns() - t0;
   }

   memset(&kick, 0, sizeof(kick));
   dst[0] = pbo(c->target.bo)->b.sync;
   kick.hDevMemContext = pvr->memctx;
   kick.hCCBKernelMemInfo = pbo(&k->ccb)->b.mem;
   kick.ui32CCBOffset = woff + 0x58;
   kick.sCommand.ui32Data[1] = k->hwctx_va;
   kick.hKernelHWSyncListMemInfo = pbo(&s->hwsync)->b.mem;
   /* every kick of a scene names the target, whose write the first counts
    * and the 3D completes; the 3D sync goes with the last (the DDK's
    * SGX_CCB_KICKs of a 3-kick scene) */
   kick.ui32NumDstSyncObjects = 1;
   kick.pahDstSyncHandles = dst;
   kick.bFirstKickOrResume = kind & TA_FIRST ? IMG_TRUE : IMG_FALSE;
   kick.bLastInScene = kind & TA_LAST ? IMG_TRUE : IMG_FALSE;
   kick.h3DSyncInfo = kind & TA_LAST ? pbo(&k->sync3d)->b.sync : NULL;
   k->ccb_woff = (woff + TA_CMD_SIZE) & (CCB_RING - 1);
   k->ccbctl.map[0] = k->ccb_woff; /* CCB write offset */
   __sync_synchronize();
   if (kind & TA_FIRST)
      done->sd = NULL;
   done->render_ns = NULL;
   if (c->dev->dry_run >= 0 && c->frames >= (unsigned)c->dev->dry_run) {
      e = pvr_kick_traced(pvr, &kick, 0); /* traced, not sent */
   } else {
      uint64_t t0 = sgxws_now_ns();

      e = pvr_kick(pvr, &kick);
      c->ns_kick += sgxws_now_ns() - t0;
      if (e) {
         mesa_loge("sgx: SGX_DOKICK failed: %d", e);
      } else if ((kind & TA_FIRST) && pbo(c->target.bo)->sd) {
         /* the kick counted one more pending write on the target */
         done->sd = pbo(c->target.bo)->sd;
         done->value = done->sd->ui32WriteOpsPending;
         done->kicked_ns = sgxws_now_ns();
         done->render_ns = &s->render_ns;
      }
   }
   return e;
}

static int
pvr_kick_frame(struct sgxws_ctx *c, uint32_t seq)
{
   return pvr_kick_ta_cmd(c, seq, TA_LAST | (c->ta_kicks ? 0 : TA_FIRST));
}

/* (seq: the frame's kick, still to come; only the last kick's token counts) */
static int
pvr_kick_ta(struct sgxws_ctx *c)
{
   return pvr_kick_ta_cmd(c, c->kicks + 1, c->ta_kicks ? 0 : TA_FIRST);
}

static int
pvr_wait_seq(struct sgxws_ctx *c, uint32_t seq, unsigned timeout_ms)
{
   struct pvr_ctx *k = c->k;

   return token_wait(&k->token[seq % c->slots], timeout_ms);
}

static void
pvr_frame_info(struct sgxws_ctx *c, char *buf, unsigned size)
{
   struct pvr_ctx *k = c->k;

   snprintf(buf, size, "PB pages in use %#x, RT frames %u", k->pbdesc.map[5],
            ((struct pvr_rt *)c->rt->k)->rtdata.map[1]);
}

/* ------------------------------------------------------------------ device */

static void
pvr_dev_destroy(struct sgxws_dev *d)
{
   pvr_disconnect(dev_pvr(d));
   pvr_trace_close(dev_pvr(d));
   free(d);
}

static const struct sgxws_kif pvr_kif = {
   .name = "pvrsrvkm",
   .rt_one_in_flight = true,
   .dev_destroy = pvr_dev_destroy,
   .bo_alloc = pvr_bo_alloc,
   .bo_free = pvr_bo_free,
   .bo_wrap = pvr_bo_wrap,
   .bo_prepare_target = pvr_bo_prepare_target,
   .bo_wait = pvr_bo_wait,
   .ctx_init = pvr_ctx_init,
   .ctx_fini = pvr_ctx_fini,
   .rt_init = pvr_rt_init,
   .rt_fini = pvr_rt_fini,
   .kick = pvr_kick_frame,
   .kick_ta = pvr_kick_ta,
   /* the CCB's read offset isn't kept in its control word here: the kicks in
    * flight are bounded instead */
   .ring_kicks = CCB_RING / TA_CMD_SIZE,
   .wait_seq = pvr_wait_seq,
   .frame_info = pvr_frame_info,
};

int
sgxws_pvr_dev_create(int fd, const struct sgxws_dev_options *opts,
                     struct sgxws_dev **out)
{
   struct pvr_dev *pd = calloc(1, sizeof(*pd));
   struct sgxws_dev *d = &pd->base;
   int e;

   if (!pd)
      return -ENOMEM;
   d->kif = &pvr_kif;
   pvr_open_fd(&pd->pvr, fd);
   if (opts->trace && (e = pvr_trace_open(&pd->pvr, opts->trace)))
      goto fail;
   if (opts->trace_kicks)
      pd->pvr.trace_kicks = opts->trace_kicks;
   if ((e = pvr_init(&pd->pvr))) {
      mesa_loge("sgx: no PVR services on fd %d: %d", fd, e);
      pvr_trace_close(&pd->pvr);
      goto fail;
   }
   for (unsigned i = 0; i < pd->pvr.nheaps; i++)
      d->heap_base[pd->pvr.heaps[i].ui32HeapID & 0xf] =
         pd->pvr.heaps[i].sDevVAddrBase.uiAddr;
   /* code bases 0-9 cover the pixel heap, 10-13 the vertex heap (SRVINIT.md) */
   d->usse_base_index[0] = 0;
   d->usse_base_index[1] = 10;
   /* The services driver exists only for the CE5300's SGX545 (its system
    * layer), so its identity is that core's; the limits are the core's
    * description (sgxws_dev_create) */
   d->info.core_id = 0x01150000;
   d->info.core_revision = 0x0001000e;
   *out = d;
   return 0;

fail:
   free(pd);
   return e;
}
