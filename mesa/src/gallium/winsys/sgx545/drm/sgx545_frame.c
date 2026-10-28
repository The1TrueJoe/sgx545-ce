/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

/*
 * sgxws: SGX545 frames from GL-level parameters.  See sgx545_frame.h.
 *
 * Tags in the comments follow re/state/STATE.md: [C] proven by a replay,
 * [D] from a diff or the DDK code that writes it, [G] guess (the value the
 * DDK uses, meaning unknown).
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sgx545_frame.h"
#include "sgx545_usse_progs.h"

/* Placement: every PDS program the DDK makes starts on 32 bytes, every USSE
 * program GL allocates on 128, every DMA source on 32 [G: what the hardware
 * needs is untested] */
#define PDS_ALIGN 0x40
#define USSE_ALIGN 0x80
#define DMA_ALIGN 0x20

/* the CCB is a 64 KB ring with room for one command past its end [G] */
#define CCB_RING 0x10000
#define CCB_SIZE 0x10420
#define TA_CMD_SIZE 0x2c0

#define H_GENERAL SGX545_HEAP_GENERAL
#define H_TADATA SGX545_HEAP_TADATA
#define H_KDATA SGX545_HEAP_KDATA
#define H_PIXSHADER SGX545_HEAP_PIXSHADER
#define H_VTXSHADER SGX545_HEAP_VTXSHADER
#define H_PDSPIXEL SGX545_HEAP_PDSPIXEL
#define H_PDSVERTEX SGX545_HEAP_PDSVERTEX
#define H_SYNC SGX545_HEAP_SYNC
#define H_3DPARAMS SGX545_HEAP_3DPARAMS

#define ws_err(...) fprintf(stderr, "sgx545: " __VA_ARGS__)

static uint32_t
f32(float x)
{
   uint32_t u;

   memcpy(&u, &x, 4);
   return u;
}

/* float -> IEEE half, round to nearest even (finite inputs) */
uint32_t
sgxws_f16(float x)
{
   uint32_t u = f32(x), s = (u >> 16) & 0x8000, m = u & 0x7fffff;
   int e = (int)((u >> 23) & 0xff) - 127 + 15;

   if (e >= 31)
      return s | 0x7c00;
   if (e <= 0) {
      if (e < -10)
         return s;
      m |= 0x800000;
      uint32_t sh = (uint32_t)(14 - e), r = m >> sh, rem = m & ((1u << sh) - 1),
               half = 1u << (sh - 1);

      if (rem > half || (rem == half && (r & 1)))
         r++;
      return s | r;
   }
   uint32_t r = (uint32_t)e << 10 | m >> 13, rem = m & 0x1fff;

   if (rem > 0x1000 || (rem == 0x1000 && (r & 1)))
      r++;
   return s | r;
}

static uint32_t
u8(float x)
{
   return x <= 0 ? 0 : x >= 1 ? 255 : (uint32_t)lrintf(x * 255.0f);
}

/* ------------------------------------------------------------------ memory */

int
sgxws_bo_alloc(struct sgxws_dev *d, struct sgxws_bo *bo, enum sgx545_heap heap, uint32_t attribs,
               uint32_t size, uint32_t align)
{
   int e = pvr_alloc(&d->pvr, &bo->b, heap, attribs, size, align);

   if (e) {
      ws_err("ALLOC_DEVICEMEM heap %u size %#x failed: %d\n", heap, size, e);
      return e;
   }
   if ((e = pvr_map(&d->pvr, &bo->b))) {
      ws_err("map of %#x failed: %d\n", bo->b.devva, e);
      pvr_free(&d->pvr, &bo->b);
      return e;
   }
   bo->va = bo->b.devva;
   bo->map = bo->b.cpu;
   memset(bo->map, 0, size);
   if (d->verbose)
      ws_err("heap %2u %#010x +%#x\n", heap, bo->va, size);
   return 0;
}

void
sgxws_bo_free(struct sgxws_dev *d, struct sgxws_bo *bo)
{
   pvr_free(&d->pvr, &bo->b);
   bo->va = 0;
   bo->map = NULL;
}

/* For the structures whose placement matters (the kernel-data and sync heap
 * order, see sgxws_ctx_create), a failed allocation leaves nothing to fall
 * back to. */
#define BO(d, bo, heap, attribs, size, align)                                   \
   do {                                                                         \
      if (sgxws_bo_alloc(d, bo, heap, attribs, size, align))                    \
         goto fail;                                                             \
   } while (0)

static int
ring_init(struct sgxws_dev *d, struct sgxws_ring *r, enum sgx545_heap heap, uint32_t size)
{
   int e = sgxws_bo_alloc(d, &r->bo, heap, 0x1, size, 0x1000);

   /* nothing at a heap's first byte: a PDS state word or DOUTU word of 0 would name it */
   r->used = r->mark = 0x100;
   return e;
}

static void
ring_mark(struct sgxws_ring *r)
{
   r->mark = r->used;
}

static void
ring_reset(struct sgxws_ring *r)
{
   r->used = r->mark;
}

static uint32_t
ring_room(const struct sgxws_ring *r)
{
   return r->bo.b.size - r->used;
}

/* where something landed: device address and CPU pointer */
struct sgxws_ptr {
   uint32_t va;
   uint32_t *p;
};

static struct sgxws_ptr
ring_alloc(struct sgxws_ring *r, uint32_t size, uint32_t align)
{
   struct sgxws_ptr p;
   uint32_t o = (r->used + align - 1) & ~(align - 1);

   if (o + size > r->bo.b.size) {
      /* sgxws_draw keeps the frame rings from filling; this is a bug */
      ws_err("ring at %#x full (%#x + %#x)\n", r->bo.va, o, size);
      abort();
   }
   r->used = o + size;
   p.va = r->bo.va + o;
   p.p = r->bo.map + o / 4;
   return p;
}

/* ------------------------------------------------------------------ USSE */

uint32_t
sgxws_usse_upload(struct sgxws_ctx *c, bool vertex, const uint32_t *w, unsigned n,
                  const uint8_t *reloc, unsigned nreloc)
{
   struct sgxws_ring *r = vertex ? &c->usse_vtx : &c->usse_pix;
   struct sgxws_ptr p = ring_alloc(r, n * 8, USSE_ALIGN);
   uint32_t pc0 = (p.va - c->dev->heap_base[vertex ? H_VTXSHADER : H_PIXSHADER]) / 8;

   memcpy(p.p, w, n * 8);
   /* phas.i targets are absolute, in instructions (ISA.md) */
   for (unsigned i = 0; i < nreloc; i++)
      p.p[2 * reloc[i]] += pc0;
   return p.va;
}

static uint32_t
usse_load(struct sgxws_ctx *c, const struct usse_prog *pr, bool vertex)
{
   return sgxws_usse_upload(c, vertex, pr->w, pr->n, pr->reloc, pr->nreloc);
}

/* DOUTU word 0 (PDS.md): code base register (pixel 0-9 at the pixel heap +
 * n*512K, vertex 10+), offset/8 in that window at b4, temporaries in fours at b24 */
static uint32_t
doutu_w0(struct sgxws_ctx *c, uint32_t va, bool vertex, uint32_t temps)
{
   uint32_t off = va - c->dev->heap_base[vertex ? H_VTXSHADER : H_PIXSHADER];

   return ((temps + 3) >> 2) << 24 | ((off >> 19) + (vertex ? 10 : 0)) | ((off & 0x7ffff) >> 3) << 4;
}

/* LIMM rN, #v (STATE.md, from the DDK's end-of-tile generator) */
static void
usse_limm(uint32_t *w, uint32_t v, uint32_t reg)
{
   w[0] = (v & 0x1fffff) | reg << 21;
   w[1] = 0xfca00000 | ((v >> 21) & 0x1f) << 4 | (v >> 26) << 12;
}

/* ------------------------------------------------------------------ PDS */

struct pds {
   uint32_t ds0[64], ds1[64];
   unsigned n0, n1;                 /* words used in each data store */
   uint32_t code[64];
   unsigned nc;
};

/* MOVS (DOUT): src1 = DS0 pair row, src2 = DS1 pair row; selects 0 s1.lo, 1 s1.hi, 2 s2.lo, 3 s2.hi */
#define MOVS(top, r1, r2, a, b, c_, d_, dest)                                                     \
   ((uint32_t)(top) << 24 | (uint32_t)(r1) << 18 | (uint32_t)(r2) << 13 | (a) << 11 | (b) << 9 | \
    (c_) << 7 | (d_) << 5 | (dest))
#define DOUTI 2
#define DOUTD 3
#define DOUTT 4
#define DOUTU 5
#define DOUTA 6
#define MV 0x07                     /* always */
#define MV_X 0x2f                   /* .x2.s27: DOUTD adds word 2 to its address */
#define MV_CC3 0x03                 /* the vertex-fetch DOUTU form */
/* ALU: op, sub-op, src1 (S1: 16-bit units, DS0 then inputs), src2 (S2: DS1, 16-bit units), dst (U) */
#define ALU(op, s, s1, s2, dst) \
   (0x07000000u | (uint32_t)(op) << 28 | (uint32_t)(s) << 27 | (s1) << 16 | (s2) << 9 | (dst))
#define S1_DS0(w) (2 * (w))
#define S1_IN(n) (2 * (0x40 + (n)))
#define S2_DS1(w) (2 * (w))
#define U_DS0(w) (w)
#define U_DS1(w) (0x40 + (w))
#define U_IN(n) (0x80 + (n))
#define OP_ADD 4
#define OP_MUL16 6
#define OP_ANDOR 0xc                /* s 1 AND, s 0 OR */
#define OP_XOR 0xd
#define SHL(src, n, dst) (0xf7000000u | (src) << 16 | (n) << 8 | (dst))
#define TST1(s1) (0x87000000u | (s1) << 16)          /* p0 = src1 == 0 */
#define TST2(s2) (0x87000000u | (s2) << 9 | 0x100)   /* p0 = src2 == 0 */
#define BRA_P0(t) (0x90000000u | (t))
#define HALT 0xaf000000u

static void
pds_d0(struct pds *p, unsigned i, uint32_t v)
{
   p->ds0[i] = v;
   if (i + 1 > p->n0)
      p->n0 = i + 1;
}

static void
pds_d1(struct pds *p, unsigned i, uint32_t v)
{
   p->ds1[i] = v;
   if (i + 1 > p->n1)
      p->n1 = i + 1;
}

/* Lay a program out as 16-byte rows (DS0 DS0 DS1 DS1), code after. */
static uint32_t
pds_emit(struct sgxws_ring *r, const struct pds *p, uint32_t *rows_out)
{
   uint32_t rows = ((p->n0 > p->n1 ? p->n0 : p->n1) + 1) / 2;
   struct sgxws_ptr m = ring_alloc(r, rows * 16 + p->nc * 4, PDS_ALIGN);

   for (uint32_t i = 0; i < rows; i++) {
      m.p[4 * i + 0] = p->ds0[2 * i];
      m.p[4 * i + 1] = p->ds0[2 * i + 1];
      m.p[4 * i + 2] = p->ds1[2 * i];
      m.p[4 * i + 3] = p->ds1[2 * i + 1];
   }
   memcpy(m.p + 4 * rows, p->code, p->nc * 4);
   if (rows_out)
      *rows_out = rows;
   return m.va;
}

#define CODE(p, w) ((p)->code[(p)->nc++] = (w))

/* TA/3D state word for a pixel PDS program: rows<<26 | offset in the PDS pixel heap >> 4 */
static uint32_t
pds_word(struct sgxws_ctx *c, uint32_t va, uint32_t rows)
{
   return rows << 26 | (va - c->dev->heap_base[H_PDSPIXEL]) >> 4;
}

/*
 * pixel program (the DDK's 0x1e970 layout): DOUTU, then per iterated input a
 * DOUTI, and after a sampling DOUTI the DOUTT with the texture's four words.
 * A DOUTI takes its word from the high half of a DS1 pair (the first shares
 * DS1 pair 0 with the DOUTU) and a zero DS0 pair; a DOUTT takes T0, T1 from a
 * DS1 pair and T2, T3 from a DS0 pair, as the background object's does.
 */
static uint32_t
pds_pixel(struct sgxws_ctx *c, uint32_t task_w0, uint32_t task_w1, const uint32_t *douti,
          const uint8_t *douti_tex, unsigned n_douti, const uint32_t (*tex)[4], uint32_t *rows)
{
   struct pds p = { 0 };
   unsigned d0 = 1, d1 = 1;

   pds_d0(&p, 0, task_w0);
   pds_d0(&p, 1, task_w1);
   pds_d1(&p, 0, 0);
   pds_d1(&p, 1, n_douti ? douti[0] : 0);
   CODE(&p, MOVS(MV, 0, 0, 0, 1, 2, 3, DOUTU));
   for (unsigned i = 0; i < n_douti; i++) {
      unsigned r1 = d0++, r2 = 0;

      if (i) {
         r2 = d1++;
         pds_d1(&p, 2 * r2, 0);
         pds_d1(&p, 2 * r2 + 1, douti[i]);
      }
      pds_d0(&p, 2 * r1, 0);
      pds_d0(&p, 2 * r1 + 1, 0);
      CODE(&p, MOVS(MV, r1, r2, 3, 0, 1, 1, DOUTI));
      if (douti_tex && douti_tex[i] && tex) {
         const uint32_t *t = tex[douti_tex[i] - 1];
         unsigned a = d1++, b = d0++;

         pds_d1(&p, 2 * a, t[0]);
         pds_d1(&p, 2 * a + 1, t[1]);
         pds_d0(&p, 2 * b, t[2]);
         pds_d0(&p, 2 * b + 1, t[3]);
         CODE(&p, MOVS(MV, b, a, 2, 3, 0, 1, DOUTT));
      }
   }
   CODE(&p, HALT);
   return pds_emit(&c->pds_pix, &p, rows);
}

/* secondary/state program (0x1ed20): an optional DOUTD of n dwords, then a task */
static uint32_t
pds_secondary(struct sgxws_ring *r, uint32_t src, uint32_t dma_ctrl, uint32_t task_w0,
              uint32_t task_w1, uint32_t *rows)
{
   struct pds p = { 0 };

   if (src) {
      pds_d0(&p, 0, src);
      pds_d0(&p, 1, dma_ctrl);
      pds_d1(&p, 0, 0);
      pds_d0(&p, 2, task_w0);
      pds_d0(&p, 3, task_w1);
      CODE(&p, MOVS(MV, 0, 0x18, 0, 1, 0, 1, DOUTD));
      CODE(&p, MOVS(MV, 1, 0, 0, 1, 2, 2, DOUTU));
   } else {
      pds_d0(&p, 0, task_w0);
      pds_d0(&p, 1, task_w1);
      CODE(&p, MOVS(MV, 0, 0, 0, 1, 2, 2, DOUTU));
   }
   CODE(&p, HALT);
   return pds_emit(r, &p, rows);
}

/* DOUTD control: dwords-1, attribute register at b8, b22-24 [G], b25 = secondary attributes [G] */
static uint32_t
dma_ctrl(uint32_t dwords, uint32_t reg, uint32_t kind, int secondary)
{
   return (dwords - 1) | reg << 8 | kind << 22 | (secondary ? 0x2000000 : 0);
}

/*
 * A secondary/state program for any amount of constants: one DOUTD per 16
 * dwords into consecutive secondary attributes, then the task.  The DDK
 * never DMAs more than 25 dwords at once, and every b22-23 it writes is
 * (dwords-1) & 3 (10/11/12/25 dwords: 1/2/3/0; 3-frame and uniform-array
 * captures), so the chunks carry that too [D for the values, G for the
 * meaning].
 */
#define DMA_CHUNK 16

static uint32_t
pds_dma_task(struct sgxws_ring *r, uint32_t src, uint32_t dwords, uint32_t task_w0, uint32_t task_w1,
             uint32_t *rows)
{
   struct pds p = { 0 };
   unsigned row = 0;

   for (uint32_t off = 0; off < dwords; off += DMA_CHUNK, row++) {
      uint32_t n = dwords - off < DMA_CHUNK ? dwords - off : DMA_CHUNK;

      pds_d0(&p, 2 * row, src + off * 4);
      pds_d0(&p, 2 * row + 1, dma_ctrl(n, off, (n - 1) & 3, 1));
      CODE(&p, MOVS(MV, row, 0x18, 0, 1, 0, 1, DOUTD));
   }
   pds_d1(&p, 0, 0);
   pds_d0(&p, 2 * row, task_w0);
   pds_d0(&p, 2 * row + 1, task_w1);
   CODE(&p, MOVS(MV, row, 0, 0, 1, 2, 2, DOUTU));
   CODE(&p, HALT);
   return pds_emit(r, &p, rows);
}

/* ------------------------------------------------------------------ device */

int
sgxws_dev_init(struct sgxws_dev *d, int fd)
{
   const char *trace = getenv("SGX545_TRACE");
   int e;

   memset(d, 0, sizeof(*d));
   d->verbose = getenv("SGX545_VERBOSE") ? atoi(getenv("SGX545_VERBOSE")) : 0;
   d->frame_gap_us = getenv("SGX545_FRAME_GAP_US") ? atoi(getenv("SGX545_FRAME_GAP_US")) : 0;
   d->dry_run = getenv("SGX545_DRY_RUN") ? atoi(getenv("SGX545_DRY_RUN")) : -1;
   pvr_open_fd(&d->pvr, fd);
   /* SGX545_TRACE=path: a pvrtrace log of this client, as tools/decode reads a DDK
    * capture; SGX545_TRACE_KICKS=n: memory copies for the first n kicks only */
   if (trace && (e = pvr_trace_open(&d->pvr, trace)))
      return e;
   if (getenv("SGX545_TRACE_KICKS"))
      d->pvr.trace_kicks = atoi(getenv("SGX545_TRACE_KICKS"));
   if ((e = pvr_init(&d->pvr)))
      return e;
   for (unsigned i = 0; i < d->pvr.nheaps; i++)
      d->heap_base[d->pvr.heaps[i].ui32HeapID & 0xf] = d->pvr.heaps[i].sDevVAddrBase.uiAddr;
   return 0;
}

void
sgxws_dev_fini(struct sgxws_dev *d)
{
   pvr_disconnect(&d->pvr);
   pvr_trace_close(&d->pvr);
}

/* ------------------------------------------------------------------ context */

/* The parameter buffer (FUN_0001e980; STATE.md section 6). */
static int
ctx_pb(struct sgxws_ctx *c, uint32_t bytes)
{
   struct sgxws_dev *d = c->dev;
   uint32_t pages = (bytes + 0x22000) >> 12, first, last, *fl, *pd;

   BO(d, &c->pbdesc, H_KDATA, 0xf, 0x3c, 0x20);
   BO(d, &c->pbblocks, H_KDATA, 0xf, 0x10, 4);
   BO(d, &c->pb, H_3DPARAMS, 0xb, pages << 12, 0x1000);
   first = (c->pb.va - d->heap_base[H_3DPARAMS]) >> 12;
   last = first + pages - 1;
   /* the free list is indexed by page number from the heap base: prev<<16 | next;
    * the DDK always allocates 0x40000, one u32 per page of the whole 256 MB heap */
   BO(d, &c->freelist, H_3DPARAMS, 0xb, (last + 1) * 4 > 0x40000 ? (last + 1) * 4 : 0x40000, 0x10);
   fl = c->freelist.map;
   for (uint32_t i = first; i <= last; i++)
      fl[i] = (i == first ? 0xffffu : i - 1) << 16 | (i == last ? 0xffffu : i + 1);
   /* block list: {pages, first | last<<16, PB descriptor, 0} */
   c->pbblocks.map[0] = pages;
   c->pbblocks.map[1] = first | last << 16;
   c->pbblocks.map[2] = c->pbdesc.va;
   pd = c->pbdesc.map;
   uint32_t zls = (pages - 0x22) & 0xfffff, ta = pages + 0x10, pds = zls < ta ? zls : ta;

   pd[1] = pages;
   pd[2] = last << 16 | first;      /* free list head */
   pd[3] = (last - 1) << 16;
   pd[4] = c->freelist.va;
   pd[6] = pd[2];
   pd[7] = pd[3];
   pd[10] = ta;                     /* TA threshold */
   pd[11] = zls;                    /* ZLS threshold */
   pd[12] = zls;                    /* global threshold */
   pd[13] = pds > 0x100 ? pds - 0x100 : 0;  /* PDS threshold */
   pd[14] = c->pbblocks.va;
   c->pb_pages = pages;
   return 0;
fail:
   return -ENOMEM;
}

#define IDX_COUNT 0x10000

struct sgxws_ctx *
sgxws_ctx_create(struct sgxws_dev *d)
{
   struct sgxws_ctx *c = calloc(1, sizeof(*c));
   struct sgxws_bo dummy;
   uint32_t hw[17] = { 0 }, rows;
   struct sgxws_ptr p;
   struct pds t = { 0 };
   int e;

   if (!c)
      return NULL;
   c->dev = d;
   if (ctx_pb(c, 0x400000))
      goto fail;
   BO(d, &c->ccb, H_KDATA, 0x4f, CCB_SIZE, 0x1000);
   BO(d, &c->ccbctl, H_KDATA, 0x4f, 8, 0x20);
   BO(d, &dummy, H_SYNC, 0x3, 4, 0x10);           /* SGXCreateRenderContext's own sync word */
   BO(d, &c->scratch, H_3DPARAMS, 0xb, 0x40, 0x40);
   BO(d, &dummy, H_SYNC, 0x4b, 0x80, 0x20);       /* and its 0x80-byte buffer for GL */
   /*
    * The kernel-data and sync allocations of the DDK's two transfer contexts,
    * which we don't use: they keep every later structure at the address the
    * DDK gives it.  The microkernel keeps pointers to the previous client's
    * render details and RT data and reads them when the next client's TA
    * starts; after a DDK client, a frame with any other layout hangs the TA
    * (re/gen/GEN.md, "Layout").
    */
   for (int i = 0; i < 2; i++) {
      BO(d, &dummy, H_KDATA, 0x4f, 0x104c8, 0x1000);
      BO(d, &dummy, H_KDATA, 0x4f, 8, 0x20);
      BO(d, &dummy, H_SYNC, 0x3, 4, 0x10);
      BO(d, i ? &dummy : &c->sync3d, H_SYNC, 0x3, 4, 0x10);    /* the DDK's 3D sync */
      BO(d, &dummy, H_KDATA, 0x4f, 4, 4);
   }

   /* HW render context (SGXCreateRenderContext): the kernel fills the PD at word 2 */
   hw[0] = 0x21;                    /* b0, b5 = per-context PB [D] */
   hw[3] = c->ccb.va;
   hw[4] = c->ccbctl.va;
   hw[6] = c->pbdesc.va;
   hw[7] = c->scratch.va;
   hw[16] = (uint32_t)getpid();
   if ((e = pvr_register_render_ctx(&d->pvr, hw, sizeof(hw), 8, &c->hwctx, &c->hwctx_va))) {
      ws_err("SGX_REGISTER_HW_RENDER_CONTEXT failed: %d\n", e);
      goto fail;
   }

   if (ring_init(d, &c->usse_pix, H_PIXSHADER, 0x40000) ||
       ring_init(d, &c->usse_vtx, H_VTXSHADER, 0x40000) ||
       ring_init(d, &c->pds_pix, H_PDSPIXEL, 0x40000) ||
       ring_init(d, &c->pds_vtx, H_PDSVERTEX, 0x40000) ||
       ring_init(d, &c->gen, H_GENERAL, 0x400000) ||
       ring_init(d, &c->vdm, H_GENERAL, 0x10000) ||
       ring_init(d, &c->usse_frame, H_PIXSHADER, 0x4000))
      goto fail;

   /* fixed USSE programs */
   c->empty_v = usse_load(c, &usse_empty, true);
   c->empty_p = usse_load(c, &usse_empty, false);
   c->copy_fs = usse_load(c, &usse_copy_fs, false);
   c->clear_vs = usse_load(c, &usse_clear_vs, true);
   c->pe_ptoff = usse_load(c, &usse_pe_ptoff, false);
   c->pe_emit = usse_load(c, &usse_pe_emit, false);
   {
      /* phas, the secondary programs' prologue up to its wdf, nop [end] */
      uint32_t w[12];

      w[0] = 0;
      w[1] = 0xfa440700;
      memcpy(w + 2, usse_fs_uniform_sec.w + 2, 4 * 8);
      w[10] = 0x00000000;
      w[11] = 0xf8040140;
      c->sec_wait = sgxws_usse_upload(c, false, w, 6, NULL, 0);
   }
   /* every state-emit length up front: the code rings are never reset */
   for (int n = 1; n <= 32; n++)
      c->state_emit[n] = usse_load(c, usse_state_emit[n], true);

   /* VDM state programs that every frame ends with */
   c->empty_state = pds_secondary(&c->pds_vtx, 0, 0, doutu_w0(c, c->empty_v, true, 0), 0, NULL);
   /* TA terminate (VDM type 6, FUN_0001fd60): DOUTA (0x2000, 0), 3 x (0x16000e, 0x100), then
    * a state-emit task for the two attributes [D; the values' meaning G] */
   pds_d0(&t, 0, 0x2000);
   pds_d0(&t, 1, 0);
   pds_d0(&t, 2, 0x0016000e);
   pds_d0(&t, 3, 0x100);
   pds_d0(&t, 4, doutu_w0(c, c->state_emit[2], true, 0));
   pds_d0(&t, 5, 0);
   CODE(&t, MOVS(MV, 0, 0x18, 0, 1, 2, 2, DOUTA));
   for (int i = 0; i < 3; i++)
      CODE(&t, MOVS(MV, 1, 0x18, 0, 1, 2, 2, DOUTA));
   CODE(&t, MOVS(MV, 2, 0, 0, 1, 2, 2, DOUTU));
   CODE(&t, HALT);
   c->terminate = pds_emit(&c->pds_vtx, &t, &rows);

   /* pixel programs shared by every object without inputs, and the clear's */
   c->pds_empty_p = pds_secondary(&c->pds_pix, 0, 0, doutu_w0(c, c->empty_p, false, 0), 0, NULL);
   /* DOUTI: iterate the colour (source 0x20 at b11), 4 components, plain iteration, last [D] */
   {
      uint32_t douti = 0x20u << 11 | 3u << 22 | 1u << 24 | 1u << 29;

      c->clear_pds = pds_pixel(c, doutu_w0(c, c->copy_fs, false, 0), 0x01000004, &douti, NULL, 1,
                               NULL, NULL);
   }

   /* identity index buffer: glDrawArrays draws through it (STATE.md section 2) */
   p = ring_alloc(&c->gen, IDX_COUNT * 2, 4);
   for (int i = 0; i < IDX_COUNT; i += 2)
      p.p[i / 2] = (uint32_t)i | (uint32_t)(i + 1) << 16;
   c->idx = p.va;
   c->idx_count = IDX_COUNT;

   ring_mark(&c->pds_pix);
   ring_mark(&c->pds_vtx);
   ring_mark(&c->gen);
   return c;

fail:
   ws_err("context creation failed\n");
   free(c);
   return NULL;
}

void
sgxws_ctx_destroy(struct sgxws_ctx *c)
{
   int e;

   sgxws_flush(c, 2000);
   if ((e = pvr_unregister_render_ctx(&c->dev->pvr, c->hwctx)))
      ws_err("SGX_UNREGISTER_HW_RENDER_CONTEXT failed: %d\n", e);
   /* the rest of the device memory goes with the services connection (resman) */
   free(c);
}

/* ------------------------------------------------------------------ render target */

/* SGXAddRenderTarget's numbers (STATE.md section 6; re/state/build.py rt_numbers) */
static void
rt_numbers(struct sgxws_rt_numbers *r, uint32_t w, uint32_t h)
{
   uint32_t n, big, m;

   r->xt = (w + 31) >> 5;
   r->yt = (h + 15) >> 4;
   if (w * h < 0x4b000) {
      r->mode = 0;
      r->mx = (((r->xt + 1) >> 1) + 3) & ~3u;
      r->my = (((r->yt + 1) >> 1) + 3) & ~3u;
      r->mtile1 = r->mx << 22 | r->mx << 12 | r->mx;
      r->mtile2 = r->my << 22 | r->my << 12 | r->my;
      n = 2;
   } else {
      r->mode = 1;
      r->mx = (((r->xt + 3) >> 2) + 3) & ~3u;
      r->my = (((r->yt + 3) >> 2) + 3) & ~3u;
      r->mtile1 = 1u << 31 | r->mx << 22 | 2 * r->mx << 12 | 3 * r->mx;
      r->mtile2 = r->my << 22 | 2 * r->my << 12 | 3 * r->my;
      n = 4;
   }
   r->padx = r->mx * n;
   r->pady = r->my * n;
   r->te_screen = (r->yt - 1) << 12 | (r->xt - 1);
   r->mt_tiles = r->mx * r->my;
   r->region_bytes = r->padx * r->pady * 12;
   m = (r->padx > r->pady ? r->padx : r->pady) - 1;
   for (big = 1; m; m >>= 1)
      big <<= 1;
   r->tailptr_bytes = big * big * 4;
}

struct sgxws_rt *
sgxws_rt_create(struct sgxws_ctx *c, uint32_t w, uint32_t h)
{
   struct sgxws_dev *d = c->dev;
   struct sgxws_rt *s = calloc(1, sizeof(*s));
   struct sgxws_rt_numbers *r;
   uint32_t *rd, *rt;

   if (!s)
      return NULL;
   r = &s->n;
   s->w = w;
   s->h = h;
   rt_numbers(r, w, h);

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
   s->rtword.map[0] = 1;            /* the frame counter: entry (1 & 1) renders first */
   rt = s->rtdata.map;
   rt[0] = s->rtword.va;
   rt[2] = r->padx;
   rt[5] = 2;
   for (int i = 0; i < 2; i++) {
      uint32_t *e = rt + 6 + 14 * i;

      e[0] = c->hwctx_va;
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

   /* the background object's vertices in the PB (FUN_0001f990): fixed point, offset 0x1000 */
   {
      uint32_t *b = s->bgblock.map;

      b[5] = f32(1.0f);
      b[8] = f32(1.0f);
      b[10] = 0x202;
      b[15] = 0x100000;
      b[16] = 0x1000;
      b[17] = 0x3f80;
      b[18] = ((w + 0x1000) * 0x100) << 8 | 0x10;
      b[19] = f32(1.0f);
      b[20] = (h + 0x1000) * 0x100;
      b[21] = 0x1000;
      b[22] = 0x3f80;
   }

   /* render details (STATE.md section 6); +0x20/+0x28 and the pixel event
    * program are set per frame */
   rd = s->rd.map;
   rd[0x00 / 4] = c->pb.va & 0xfff00000;  /* 3D request base */
   rd[0x08 / 4] = 0x80;                   /* [G] */
   rd[0x0c / 4] = 0x000f0000;             /* [G] */
   rd[0x10 / 4] = 0x80000000;
   rd[0x38 / 4] = 0x80000000;             /* no depth load */
   rd[0x3c / 4] = 0x80000000;             /* no stencil load */
   rd[0x40 / 4] = f32(1.0f);              /* ISP background depth [C] */
   rd[0x44 / 4] = 0x300;                  /* [G] */
   rd[0x48 / 4] = 0x2000000 | (s->bgblock.va - (c->pb.va & 0xfff00000)) >> 4;
   rd[0x50 / 4] = 0x88;
   rd[0x58 / 4] = 0x30;
   rd[0x64 / 4] = 1;                      /* [G] */
   rd[0x68 / 4] = 1;
   rd[0xcc / 4] = 0x100;
   rd[0xd0 / 4] = s->rd_sync.va;
   rd[0xd8 / 4] = s->rtdata.va;
   rd[0x104 / 4] = 1;                     /* [G] */
   rd[0x110 / 4] = s->rd_w8.va;
   rd[0x114 / 4] = s->rd_w4.va;
   rd[0x13c / 4] = 0x07c00000;            /* [G] */
   s->rd_sync.map[0] = 1;
   s->hwsync.map[0] = s->hwsync_access.va; /* SGXMKIF_HWDEVICE_SYNC_LIST.sAccessDevAddr */
   s->hwsync_access.map[0] = 1;
   return s;

fail:
   free(s);
   return NULL;
}

void
sgxws_rt_destroy(struct sgxws_ctx *c, struct sgxws_rt *s)
{
   /* SGX_FLUSH_HW_RENDER_TARGET is what the DDK sends here.  It times out on a
    * srvinit-started microkernel and the GPU locks up after it (docs/GEN.md),
    * so only on request. */
   if (getenv("SGX545_FLUSH_RT")) {
      int e = pvr_flush_rt(&c->dev->pvr, s->rtdata.va);

      if (e)
         ws_err("SGX_FLUSH_HW_RENDER_TARGET %#x failed: %d\n", s->rtdata.va, e);
   }
   if (c->rt == s)
      sgxws_flush(c, 2000);
   /* The kernel-data structures stay allocated: the microkernel may still
    * hold pointers into them (re/gen/GEN.md, "Layout"). */
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

/* ------------------------------------------------------------------ per-frame programs */

/* The end-of-tile program (FUN_000301d0): LIMM r0-r3 = the four PBE words,
 * EMITPIX them, EMITPIX the sideband word (STATE.md section 4) */
static uint32_t
eot_program(struct sgxws_ctx *c)
{
   struct sgxws_ptr p = ring_alloc(&c->usse_frame, 7 * 8, USSE_ALIGN);
   const struct sgxws_rt *s = c->rt;
   uint32_t pbe[4], side;

   pbe[0] = 0;                                             /* clip min y<<16 | x [D] */
   pbe[1] = (s->h - 1) << 16 | (s->w - 1);                 /* clip max [C] */
   pbe[2] = ((c->target.stride - 1) >> 1) << 16 | 0x8000;  /* stride/2-1 [C]; b15 [D] */
   pbe[3] = c->target.va & ~3u;                            /* address [D] */
   side = 0x200 | 0 /* BGRA8888 */ | 0x400 /* dither */;
   p.p[0] = 0;                                             /* phas.i #0, #0 [wait=7] */
   p.p[1] = 0xfa440700;
   for (uint32_t r = 0; r < 4; r++)
      usse_limm(p.p + 2 + 2 * r, pbe[r], r);
   p.p[10] = 0x00200080;                                   /* emit the four PBE words */
   p.p[11] = 0xfb200100;
   p.p[12] = (side & 0x3f) << 22 | 0x00208180;             /* emit the sideband word, end */
   p.p[13] = 0xfb240000 | (side & 0x3000) << 10 | ((side >> 3) & 0x1f8);
   return p.va;
}

/* The pixel event program (libsrv_um's static template, PDS.md "pixel-event program decoded"):
 *   in1 & 8 -> task ds0[0];  in1 & 2 -> (in1 ^ ds1[0]) & 0x100 ? DOUTA + end-of-tile task : task ds0[2];
 *   in1 & 4 -> task ds0[2]. */
static uint32_t
pixel_event(struct sgxws_ctx *c, uint32_t eot, uint32_t *rows)
{
   struct pds p = { 0 };

   pds_d0(&p, 0, doutu_w0(c, c->pe_ptoff, false, 0));
   pds_d0(&p, 1, 0);
   pds_d1(&p, 0, 0);
   pds_d1(&p, 1, doutu_w0(c, eot, false, 4));
   pds_d0(&p, 2, doutu_w0(c, c->pe_emit, false, 4));
   pds_d0(&p, 3, 0x00c00000);
   pds_d1(&p, 2, 8);
   pds_d1(&p, 3, 2);
   pds_d0(&p, 4, 0x100);
   pds_d0(&p, 5, 0xff8);
   pds_d1(&p, 4, 4);
   pds_d1(&p, 5, 0);
   pds_d0(&p, 6, 0x00800000);
   pds_d0(&p, 7, 0x003ff000);
   pds_d1(&p, 6, 0);
   pds_d1(&p, 7, 0);
   pds_d0(&p, 8, 0x00400000);
   pds_d0(&p, 9, 0);
   pds_d1(&p, 8, 0);
   pds_d1(&p, 9, 0);
   /*  0 */ CODE(&p, ALU(OP_ANDOR, 1, S1_IN(1), S2_DS1(2), U_DS1(0x30)));
   /*  1 */ CODE(&p, TST2(S2_DS1(0x30)));
   /*  2 */ CODE(&p, BRA_P0(5));
   /*  3 */ CODE(&p, MOVS(MV, 0, 0, 0, 1, 0, 0, DOUTU));
   /*  4 */ CODE(&p, HALT);
   /*  5 */ CODE(&p, ALU(OP_ANDOR, 1, S1_IN(1), S2_DS1(3), U_DS1(0x30)));
   /*  6 */ CODE(&p, TST2(S2_DS1(0x30)));
   /*  7 */ CODE(&p, BRA_P0(24));
   /*  8 */ CODE(&p, ALU(OP_XOR, 1, S1_IN(1), S2_DS1(0), U_DS1(0x30)));
   /*  9 */ CODE(&p, ALU(OP_ANDOR, 1, S1_DS0(4), S2_DS1(0x30), U_DS1(0x30)));
   /* 10 */ CODE(&p, TST2(S2_DS1(0x30)));
   /* 11 */ CODE(&p, BRA_P0(27));
   /* 12 */ CODE(&p, MOVS(MV, 0x21, 0, 0, 2, 0, 0, DOUTA));  /* (in2, ds1[0]) */
   /* 13 */ CODE(&p, SHL(U_IN(0), 3, U_DS1(0x30)));
   /* 14 */ CODE(&p, ALU(OP_ANDOR, 1, S1_DS0(5), S2_DS1(0x30), U_DS1(0x30)));
   /* 15 */ CODE(&p, ALU(OP_ANDOR, 0, S1_DS0(6), S2_DS1(0x30), U_DS0(0x30)));
   /* 16 */ CODE(&p, SHL(U_IN(0), 3, U_DS1(0x30)));
   /* 17 */ CODE(&p, ALU(OP_ANDOR, 1, S1_DS0(7), S2_DS1(0x30), U_DS1(0x30)));
   /* 18 */ CODE(&p, ALU(OP_ANDOR, 0, S1_DS0(0x30), S2_DS1(0x30), U_DS0(0x30)));
   /* 19 */ CODE(&p, SHL(U_IN(1), 20, U_DS1(0x30)));
   /* 20 */ CODE(&p, ALU(OP_ANDOR, 1, S1_DS0(8), S2_DS1(0x30), U_DS1(0x30)));
   /* 21 */ CODE(&p, ALU(OP_ANDOR, 0, S1_DS0(0x30), S2_DS1(0x30), U_DS0(0x30)));
   /* 22 */ CODE(&p, MOVS(MV, 0x18, 0, 3, 0, 0, 0, DOUTU));  /* (ds1[1], ds0[0x30], ...) */
   /* 23 */ CODE(&p, HALT);
   /* 24 */ CODE(&p, ALU(OP_ANDOR, 1, S1_IN(1), S2_DS1(4), U_DS1(0x30)));
   /* 25 */ CODE(&p, TST2(S2_DS1(0x30)));
   /* 26 */ CODE(&p, BRA_P0(28));
   /* 27 */ CODE(&p, MOVS(MV, 1, 0, 0, 1, 0, 0, DOUTU));
   /* 28 */ CODE(&p, HALT);
   return pds_emit(&c->pds_pix, &p, rows);
}

/* The background object: every tile starts by loading the target's old
 * contents (a sample of the surface as a strided texture) [D]. */
static void
background(struct sgxws_ctx *c)
{
   const struct sgxws_rt *s = c->rt;
   struct pds p = { 0 };
   uint32_t rows, va;

   pds_d0(&p, 0, doutu_w0(c, c->copy_fs, false, 0));
   pds_d0(&p, 1, 0x02000004);       /* DOUTU w1: samples (b25), b2 */
   pds_d1(&p, 0, 0);
   /* DOUTI: sample texcoord set 0 (b0-4 = set+1), pure sample (b13, b16), last sample (b30) */
   pds_d1(&p, 1, 1u | 0x12000u | 1u << 30);
   pds_d0(&p, 2, 0);
   pds_d0(&p, 3, 0);
   /* DOUTT: T0 = the pitch (pitch-1 split over b9-11 and b17-26, re/tex/TEX.md;
    * 0x20e00 at 736) | 0x90 (clamp S/T b7/b4); T1 = layout 6 (strided) | format
    * 0x0c (BGRA8888) | w-1 | h-1; T2 = address; T3 [G] */
   {
      uint32_t pm1 = c->target.stride - 1;

      pds_d1(&p, 2, (pm1 & 7) << 9 | (pm1 >> 3) << 17 | 0x90);
   }
   pds_d1(&p, 3, 6u << 29 | 0x0cu << 24 | (s->w - 1) << 12 | (s->h - 1));
   pds_d0(&p, 4, c->target.va);
   pds_d0(&p, 5, 0x00800000);
   CODE(&p, MOVS(MV, 0, 0, 0, 1, 2, 3, DOUTU));
   CODE(&p, MOVS(MV, 1, 0, 3, 0, 1, 1, DOUTI));
   CODE(&p, MOVS(MV, 2, 1, 2, 3, 0, 1, DOUTT));
   CODE(&p, HALT);
   va = pds_emit(&c->pds_pix, &p, &rows);
   c->bg_pds[0] = pds_word(c, c->pds_empty_p, 1);
   c->bg_pds[1] = 0x00030001;       /* [G] */
   c->bg_pds[2] = pds_word(c, va, rows);
}

/* ------------------------------------------------------------------ recording */

bool
sgxws_recording(struct sgxws_ctx *c)
{
   return c->rt != NULL;
}

int
sgxws_begin(struct sgxws_ctx *c, struct sgxws_rt *rt, const struct sgxws_target *t)
{
   uint32_t pixev, rows;
   uint32_t *rd;

   if (c->rt == rt && c->target.va == t->va)
      return 0;
   if (c->rt) {
      int e = sgxws_flush(c, 2000);

      if (e)
         return e;
   }
   c->rt = rt;
   c->target = *t;
   c->vdm_start = c->vdm.bo.va + c->vdm.used;
   c->draws = 0;

   pixev = pixel_event(c, eot_program(c), &rows);
   background(c);
   rd = rt->rd.map;
   rd[0x5c / 4] = pixev;
   rd[0x60 / 4] = rows;
   rt->rd_w4.map[0] = c->bg_pds[2];
   return 0;
}

static void
vdm(struct sgxws_ctx *c, uint32_t w)
{
   *ring_alloc(&c->vdm, 4, 4).p = w;
}

/* A TA state block goes to the MTE through a VDM state program: DMA the block
 * into USSE attributes, run a task that emits them (STATE.md section 2) */
static void
state_block(struct sgxws_ctx *c, const uint32_t *blk, uint32_t n, uint32_t dma_dwords, uint32_t kind)
{
   struct sgxws_ptr b = ring_alloc(&c->pds_vtx, dma_dwords * 4, DMA_ALIGN);
   struct pds p = { 0 };
   uint32_t rows, va;

   memset(b.p, 0, dma_dwords * 4);
   memcpy(b.p, blk, n * 4);
   /* the DDK's static state program: tst; bra; doutd; tst; bra; doutd; doutu */
   pds_d0(&p, 0, b.va);
   pds_d0(&p, 1, dma_ctrl(dma_dwords, 0, kind, 1));
   pds_d0(&p, 2, 0);
   pds_d0(&p, 3, 0);
   pds_d0(&p, 4, doutu_w0(c, c->state_emit[n], true, 0));
   pds_d0(&p, 5, 0);
   CODE(&p, TST1(S1_DS0(0)));
   CODE(&p, BRA_P0(6));
   CODE(&p, MOVS(MV, 0, 0, 0, 1, 0, 0, DOUTD));
   CODE(&p, TST1(S1_DS0(2)));
   CODE(&p, BRA_P0(6));
   CODE(&p, MOVS(MV, 1, 0, 0, 1, 0, 0, DOUTD));
   CODE(&p, MOVS(MV, 2, 0, 0, 1, 2, 0, DOUTU));
   CODE(&p, HALT);
   va = pds_emit(&c->pds_vtx, &p, &rows);
   vdm(c, 0x40000000 | va >> 4);
   /* info (libGLESv2 0x397f0): rows, the rotating buffer index + 0x2000, 0x2020400, dwords */
   vdm(c, rows << 26 | ((c->vdm_counter + 0x2000) | 0x2020400 | ((n + 3) >> 2)));
   c->vdm_counter = (c->vdm_counter + 1) & 3;
}

/* Vertices per USSE task and index batch (libGLESv2 FUN_00017c20 limits them
 * by the USSE attribute space; the two values here fit every draw seen [G]). */
static uint32_t
vertices_per_task(uint32_t out_dwords)
{
   uint32_t o = (out_dwords + 3) & ~3u, v = 64 / (o ? o : 4);

   return v > 16 ? 16 : v;
}

/* a draw: index list + vertex fetch program (STATE.md section 2; PDS.md 0x1f3d0) */
static void
vdm_draw(struct sgxws_ctx *c, uint32_t prim, uint32_t count, uint32_t idx, uint32_t vb,
         uint32_t stride, uint32_t attr_dwords, uint32_t vs, uint32_t in_dwords, uint32_t out_dwords,
         uint32_t temps)
{
   struct pds p = { 0 };
   uint32_t rows, va, vpt = vertices_per_task(out_dwords), batch = 16;

   pds_d0(&p, 0, vb);
   pds_d0(&p, 1, attr_dwords - 1);  /* DOUTD: dwords-1, attribute register 0 */
   pds_d1(&p, 0, stride);
   pds_d1(&p, 1, 0);
   pds_d0(&p, 2, doutu_w0(c, vs, true, temps));
   pds_d0(&p, 3, 0);
   CODE(&p, ALU(OP_MUL16, 0, S1_IN(0), S2_DS1(0), U_DS1(0x32)));  /* index * stride */
   CODE(&p, MOVS(MV_X, 0, 0x19, 0, 1, 2, 2, DOUTD));              /* + ds1[0x32] */
   CODE(&p, MOVS(MV_CC3, 1, 0, 0, 1, 3, 3, DOUTU));
   CODE(&p, HALT);
   va = pds_emit(&c->pds_vtx, &p, &rows);
   vdm(c, 0x80000000 | 0x00c00000 | prim << 26 | count);
   vdm(c, idx & ~1u);                /* 16-bit indices */
   vdm(c, (batch - 1) << 28);
   vdm(c, 0x003fffff);
   vdm(c, (vpt - 1) << 28 | va >> 4);
   vdm(c, (out_dwords + 3) >> 2 << 24 | ((in_dwords * 4 + 0xf) >> 4) << 7 |
          (vpt - 1 < 3 ? vpt - 1 : 3) << 22 | rows);
}

/* headroom one object needs in the frame rings, beyond its vertices */
#define OBJ_PDS 0x400
#define OBJ_VDM 0x100

static bool
frame_has_room(struct sgxws_ctx *c, uint32_t vertex_bytes)
{
   return ring_room(&c->gen) > vertex_bytes + 0x100 && ring_room(&c->pds_pix) > OBJ_PDS &&
          ring_room(&c->pds_vtx) > OBJ_PDS && ring_room(&c->vdm) > OBJ_VDM + 0x40;
}

/* make room for one more object, submitting the frame so far if need be (the
 * next render starts from its pixels, as every render does) */
static int
make_room(struct sgxws_ctx *c, uint32_t vertex_bytes)
{
   struct sgxws_rt *rt = c->rt;
   struct sgxws_target t = c->target;
   int e;

   if (frame_has_room(c, vertex_bytes))
      return 0;
   if ((e = sgxws_flush(c, 2000)))
      return e;
   if ((e = sgxws_begin(c, rt, &t)))
      return e;
   return frame_has_room(c, vertex_bytes) ? 0 : -E2BIG;
}

int
sgxws_clear(struct sgxws_ctx *c, const float rgba[4], float depth, bool write_depth)
{
   const struct sgxws_rt *s = c->rt;
   struct sgxws_ptr v;
   uint32_t argb = u8(rgba[3]) << 24 | u8(rgba[0]) << 16 | u8(rgba[1]) << 8 | u8(rgba[2]);
   float xy[3][2] = { { 0, 0 }, { 2.0f * s->w, 0 }, { 0, 2.0f * s->h } };
   uint32_t blk[12];
   int e;

   if ((e = make_room(c, 3 * 16)))
      return e;
   v = ring_alloc(&c->gen, 3 * 16, DMA_ALIGN);
   for (int i = 0; i < 3; i++) {
      v.p[4 * i + 0] = f32(xy[i][0]);
      v.p[4 * i + 1] = f32(xy[i][1]);
      v.p[4 * i + 2] = f32(depth);
      v.p[4 * i + 3] = argb;
   }
   /* TA state block: ISPA ISPB PDS region-clip output-select MTE TSP (header 0xd4c3) */
   blk[0] = 0x1 | 0x2 | 0x40 | 0x80 | 0x400 | 0x1000 | 0x4000 | 0x8000;
   /* always; depth write only for a depth clear to something other than the
    * background depth (1.0) */
   blk[1] = write_depth && depth != 1.0f ? 0x01c00200 : 0x01d00200;
   blk[2] = 0xf << 4;                                    /* ISPB: colour mask */
   blk[3] = pds_word(c, c->pds_empty_p, 1);
   blk[4] = 0x00030001;                                  /* [G] */
   blk[5] = pds_word(c, c->clear_pds, 2);
   blk[6] = 0x80000000 | 0 << 16 | (s->n.xt - 1);        /* region clip: whole surface, in tiles */
   blk[7] = 0 << 16 | (s->n.yt - 1);
   blk[8] = 8 << 16 | 0x1800;                            /* 8 output dwords [D], 0x1800 [G] */
   blk[9] = 0x40;                                        /* MTE ctrl of the clear [G] */
   blk[10] = 5;                                          /* one 4-component set */
   blk[11] = 0;
   state_block(c, blk, 12, 12, 3);
   vdm_draw(c, SGXWS_TRIANGLES, 3, c->idx, v.va, 16, 4, c->clear_vs, 4, 8, 0);
   c->draws++;
   return 0;
}

static uint32_t
ispa(const struct sgxws_state *st)
{
   uint32_t v = 0x200;              /* b9: ISPB follows */

   if (st->translucent)
      v |= 1u << 25;                /* pass type 1 */

   if (!st->depth_func)
      v |= 7u << 22 | 0x100000;     /* always, no write */
   else
      v |= (st->depth_func - 0x200) << 22 | (st->depth_write ? 0 : 0x100000);
   return v;
}

/* MTE cull (STATE.md section 3): with gallium's front_ccw (st flips it for a
 * y-down window), 1 culls back faces of a CCW-front draw, 2 front faces;
 * checked against the DDK on the EA3 for cull back, front, and back with a
 * CW front */
static uint32_t
mte_ctrl(const struct sgxws_state *st)
{
   uint32_t k = 0;

   if (st->cull == SGXWS_BACK)
      k = st->front_ccw ? 1 : 2;
   else if (st->cull == SGXWS_FRONT)
      k = st->front_ccw ? 2 : 1;
   return 0x1555620 | k;
}

int
sgxws_draw(struct sgxws_ctx *c, const struct sgxws_draw *d)
{
   const struct sgxws_state *st = d->state;
   const struct sgxws_prog *vs = d->vs, *fs = d->fs;
   uint32_t blk[18], n = 0, rows, sec, prim, vrows;
   uint32_t vw = vs->attr_dwords * 4, vsec_dwords = 10 + vs->const_dwords;
   struct sgxws_ptr v, u, vsec;
   int e;

   if (d->count > c->idx_count)
      return -E2BIG;
   if ((e = make_room(c, d->count * vw + vsec_dwords * 4)))
      return e;
   v = ring_alloc(&c->gen, d->count * vw, DMA_ALIGN);
   memcpy(v.p, d->verts, d->count * vw);

   /* fragment: the constants and the secondary program that DMAs them, then
    * the primary program that runs the shader */
   if (fs->sec_dwords) {
      u = ring_alloc(&c->pds_pix, fs->sec_dwords * 4, DMA_ALIGN);
      memcpy(u.p, d->fs_consts, fs->sec_dwords * 4);
      /* a task that waits for the DMA (wdf) before it ends: the empty task
       * here locks the GPU up (EA3) */
      sec = pds_dma_task(&c->pds_pix, u.va, fs->sec_dwords,
                         doutu_w0(c, fs->sec ? fs->sec : c->sec_wait, false, fs->sec_temps), 4, &rows);
   } else {
      sec = c->pds_empty_p;
      rows = 1;
   }
   blk[n++] = 0;
   blk[n++] = ispa(st);
   blk[n++] = 0x10 << 8 | (st->color_mask & 0xf) << 4;  /* ISPB: line width 1.0, colour mask */
   blk[n++] = pds_word(c, sec, rows);
   /* PDS middle word: b18-24 = (secondary dwords+3)>>2 [D]; the rest from the
    * DDK's shader object (b0, b16-17 constant; b25-28 = 0xb for its uniform
    * shader) [G] */
   blk[n++] = fs->pds_middle ? fs->pds_middle
                             : 0x00030001 | ((fs->sec_dwords + 3) >> 2) << 18 | 0xbu << 25;
   /* DOUTU word 1: b24 = the program has plain iterations, b25 samples (PDS.md) */
   {
      uint32_t w1 = 4;

      /* from each DOUTI's own halves: b24 = plain iteration, a sample half */
      for (unsigned i = 0; i < fs->n_douti; i++) {
         if (fs->douti[i] & (1u << 24))
            w1 |= 0x01000000;
         if (fs->douti_tex[i])
            w1 |= 0x02000000;
      }
      prim = pds_pixel(c, doutu_w0(c, fs->code, false, fs->temps), w1, fs->douti, fs->douti_tex,
                       fs->n_douti, d->tex, &rows);
   }
   blk[n++] = pds_word(c, prim, rows);
   /* viewport (6 floats): offset/scale for x, y, z, as gallium's transform [C x offset, D rest] */
   blk[n++] = f32(st->vp_translate[0]);
   blk[n++] = f32(st->vp_scale[0]);
   blk[n++] = f32(st->vp_translate[1]);
   blk[n++] = f32(st->vp_scale[1]);
   blk[n++] = f32(st->vp_translate[2]);
   blk[n++] = f32(st->vp_scale[2]);
   blk[n++] = 0;                                          /* header 0x200 */
   blk[n++] = vs->out_dwords << 16 | 0x3000;              /* output select */
   blk[n++] = 0x3727c5ac;                                 /* W clamp 1e-5 */
   blk[n++] = mte_ctrl(st);
   blk[n++] = vs->tsp_sizes;                              /* TSP: texcoord sizes */
   blk[n++] = vs->tsp_prec;
   blk[0] = 0x1 | 0x2 | 0x40 | 0x100 | 0x200 | 0x400 | 0x800 | 0x1000 | 0x4000 | 0x8000;
   /* the DDK DMAs 25 dwords for a draw's block (kind 0); the header says where it ends */
   state_block(c, blk, n, 25, 0);

   /* vertex secondary attributes: the block's own address, then the driver
    * constants 2/255, 1/255, 2/65535, 1/65535 [D], then the shader's constants
    * from sa10 (the compiler's convention) */
   vsec = ring_alloc(&c->pds_vtx, vsec_dwords * 4, DMA_ALIGN);
   memset(vsec.p, 0, 40);
   vsec.p[0] = vsec.va;
   vsec.p[2] = f32(2.0f / 255);
   vsec.p[3] = f32(1.0f / 255);
   vsec.p[4] = f32(2.0f / 65535);
   vsec.p[5] = f32(1.0f / 65535);
   if (vs->const_dwords)
      memcpy(vsec.p + 10, d->vs_consts, vs->const_dwords * 4);
   sec = pds_dma_task(&c->pds_vtx, vsec.va, vsec_dwords, doutu_w0(c, c->empty_v, true, 0), 0, &vrows);
   vdm(c, 0x40000000 | sec >> 4);
   vdm(c, vrows << 26 | 0xe100 | (vsec_dwords + 3) >> 2);

   vdm_draw(c, d->prim, d->count, c->idx, v.va, vw, vs->attr_dwords, vs->code, vs->in_dwords,
            vs->out_dwords, vs->temps);
   c->draws++;
   return 0;
}

/* ------------------------------------------------------------------ submission */

/* the end of the stream: an empty secondary update, the TA terminate, VDM terminate */
static void
vdm_end(struct sgxws_ctx *c)
{
   vdm(c, 0x40000000 | c->empty_state >> 4);
   vdm(c, 1u << 26 | 0xe100);
   vdm(c, 0x60000000 | c->terminate >> 4);
   vdm(c, 3u << 26 | 0x2022401);
   vdm(c, 0xc0000000);
}

/*
 * SGXMKIF_CMDTA, client part (STATE.md section 1), for frame f of the render
 * target as the DDK's swaps do it (3- and 6-frame captures of probe-st with
 * PROBE_FRAMES): one kick, first and last in scene; TA flags 0x63, plus 0x10
 * on the target's first frame; the frame number at +0x20 and at b14 of the 3D
 * flags; the RT data entry (in the command and in render details +0xdc) and
 * the region headers alternate, frame 0 taking entry 1 and the second
 * region-header array.
 */
static void
ta_command(struct sgxws_ctx *c, uint32_t *cmd)
{
   struct sgxws_rt *s = c->rt;
   uint32_t f = s->frames, odd = f & 1, *t = cmd + 0x238 / 4, *rd = s->rd.map;
   uint32_t rgn_this = s->rgn[odd ? 0 : 1].va, rgn_next = s->rgn[odd ? 1 : 0].va;
   uint32_t entry = s->rtdata.va + 0x18 + 0x38 * (odd ? 0 : 1);

   memset(cmd, 0, TA_CMD_SIZE);
   cmd[0x00 / 4] = TA_CMD_SIZE;                     /* size, rounded to 64 */
   cmd[0x04 / 4] = 0x63 | (f ? 0 : 0x10);           /* TA flags [D] */
   cmd[0x08 / 4] = c->bg_pds[0];
   cmd[0x0c / 4] = c->bg_pds[1];
   cmd[0x10 / 4] = c->bg_pds[2];
   cmd[0x14 / 4] = 0x50000000;
   cmd[0x18 / 4] = 0x50000000;
   cmd[0x1c / 4] = 0x400 | f << 14;                 /* 3D flags: render, frame [D] */
   cmd[0x20 / 4] = f;                               /* the target's frame number [D] */
   cmd[0x24 / 4] = c->pbdesc.va;
   cmd[0x38 / 4] = s->rd.va;
   cmd[0x3c / 4] = s->hwsync.va;
   cmd[0x40 / 4] = s->rtdata.va;
   cmd[0x44 / 4] = entry;
   t[0x00 / 4] = 0x20000000;                        /* TE AA */
   t[0x04 / 4] = s->n.mtile1;
   t[0x08 / 4] = s->n.mtile2;
   t[0x0c / 4] = s->n.te_screen;
   t[0x10 / 4] = s->n.mt_tiles;
   t[0x14 / 4] = 0x400000;                          /* TA flags 2: this kick renders */
   t[0x18 / 4] = c->vdm_start;
   t[0x1c / 4] = 0xa2;                              /* [G] */
   t[0x20 / 4] = rgn_this;
   t[0x24 / 4] = s->tailptr.va;
   t[0x28 / 4] = f32(1e-6f);
   t[0x2c / 4] = f32(1e-6f);
   t[0x30 / 4] = (s->h - 1) << 16 | (s->w - 1);
   t[0x3c / 4] = c->pb.va & 0xfff00000;
   t[0x6c / 4] = 0x88;

   rd[0x20 / 4] = rgn_this;                         /* region headers of this render */
   rd[0x28 / 4] = rgn_next;                         /* and of the next */
   /* The RT data entry this render retires.  It must follow cmd+0x44: left
    * at entry 1 (as one frame needs), the microkernel retires the wrong entry
    * on odd frames, their PB pages are never freed, and the 3D locks up on
    * the fourth frame (EA3, 2026-10-28). */
   rd[0xdc / 4] = entry;

   /* What the DDK's client writes before every kick (a word-by-word diff of
    * its 6-frame capture against ours): the frame counter in the RT data
    * header's word, which picks the entry that renders (counter & 1), and the
    * render-details and HW sync-list access words, which the microkernel
    * clears.  Left alone, the counter stays at 1, odd frames retire the
    * wrong entry, their PB pages are never freed, and the 3D locks up on the
    * fourth frame (EA3, 2026-10-28). */
   s->rtword.map[0] = f + 1;
   s->rd_sync.map[0] = 1;
   s->hwsync_access.map[0] = 1;
}

static int
wait_render(struct sgxws_ctx *c, unsigned ms)
{
   volatile PVRSRV_SYNC_DATA *sd = c->target.sync_data;
   unsigned t = 0;

   if (!sd)
      return 0;
   while (sd->ui32WriteOpsComplete < sd->ui32WriteOpsPending && t < ms * 10) {
      struct timespec ts = { 0, 100000 };

      nanosleep(&ts, NULL);
      t++;
   }
   if (c->dev->verbose)
      ws_err("render target write ops %u/%u after %u.%u ms\n", sd->ui32WriteOpsComplete,
             sd->ui32WriteOpsPending, t / 10, t % 10);
   return sd->ui32WriteOpsComplete < sd->ui32WriteOpsPending ? -ETIMEDOUT : 0;
}

int
sgxws_flush(struct sgxws_ctx *c, unsigned timeout_ms)
{
   struct sgxws_rt *s = c->rt;
   SGX_CCB_KICK k;
   IMG_HANDLE dst[1];
   uint32_t woff = c->ccb_woff;
   int e;

   if (!s)
      return 0;

   vdm_end(c);
   ta_command(c, c->ccb.map + woff / 4);

   memset(&k, 0, sizeof(k));
   dst[0] = c->target.sync;
   k.hDevMemContext = c->dev->pvr.memctx;
   k.hCCBKernelMemInfo = c->ccb.b.mem;
   k.ui32CCBOffset = woff + 0x58;
   k.sCommand.ui32Data[1] = c->hwctx_va;
   k.hKernelHWSyncListMemInfo = s->hwsync.b.mem;
   k.ui32NumDstSyncObjects = 1;
   k.pahDstSyncHandles = dst;
   k.bFirstKickOrResume = IMG_TRUE;
   k.bLastInScene = IMG_TRUE;
   k.h3DSyncInfo = c->sync3d.b.sync;
   c->ccb_woff = (woff + TA_CMD_SIZE) & (CCB_RING - 1);
   c->ccbctl.map[0] = c->ccb_woff;                  /* CCB write offset */
   __sync_synchronize();
   if (c->dev->dry_run >= 0 && c->frames >= (unsigned)c->dev->dry_run) {
      e = pvr_kick_traced(&c->dev->pvr, &k, 0);     /* traced, not sent */
   } else {
      e = pvr_kick(&c->dev->pvr, &k);
      if (e)
         ws_err("SGX_DOKICK failed: %d\n", e);
      else
         e = wait_render(c, timeout_ms);
   }
   if (!e && c->dev->frame_gap_us) {
      struct timespec ts = { 0, (long)c->dev->frame_gap_us * 1000 };

      nanosleep(&ts, NULL);
   }
   if (c->dev->verbose || e)
      ws_err("frame %u (target %u): %u objects, PB pages in use %#x, RT frames %u, rc %d\n",
             c->frames, s->frames, c->draws, c->pbdesc.map[5], s->rtdata.map[1], e);

   s->frames++;
   c->frames++;
   c->rt = NULL;
   /* the render has finished (or the GPU is gone): the frame's memory is free */
   ring_reset(&c->pds_pix);
   ring_reset(&c->pds_vtx);
   ring_reset(&c->gen);
   ring_reset(&c->vdm);
   ring_reset(&c->usse_frame);
   return e;
}

/* ------------------------------------------------------------------ built-in shaders */

void
sgxws_prog_builtin(struct sgxws_ctx *c, enum sgxws_builtin which, struct sgxws_prog *s)
{
   memset(s, 0, sizeof(*s));
   switch (which) {
   case SGXWS_VS_XY:
   case SGXWS_VS_XYZ:
      s->vertex = true;
      s->code = usse_load(c, which == SGXWS_VS_XY ? &usse_vs_xy : &usse_vs_xyz, true);
      s->in_dwords = 4;
      s->out_dwords = 4;
      s->attr_dwords = which == SGXWS_VS_XY ? 2 : 3;
      break;
   case SGXWS_FS_UNIFORM:
      s->sec = usse_load(c, &usse_fs_uniform_sec, false);
      s->code = usse_load(c, &usse_fs_uniform, false);
      s->sec_dwords = SGXWS_FS_UNIFORM_DWORDS;
      break;
   }
}

/* ------------------------------------------------------------------ compiled programs */

#define PHAS_HI 0xfa440000u            /* phas.i #pc, #temps/4 [wait] (docs/ISA.md) */
#define NOP_END_LO 0x00000000u
#define NOP_END_HI 0xf8040140u         /* nop [end] */

static void
put(uint32_t *w, unsigned *n, uint32_t lo, uint32_t hi)
{
   w[2 * *n] = lo;
   w[2 * *n + 1] = hi;
   (*n)++;
}

static void
put_code(uint32_t *w, unsigned *n, const uint64_t *code, unsigned count)
{
   for (unsigned i = 0; i < count; i++)
      put(w, n, (uint32_t)code[i], (uint32_t)(code[i] >> 32));
}

uint32_t
sgxws_upload_vs(struct sgxws_ctx *c, const uint64_t *code, unsigned count, unsigned temps)
{
   /* usse_vs_xy: [0] phas.i @p1, [1-4] the prologue, [7] phas.i @p2 [wait=2],
    * [9] emit.t3 r0, #0, #0 [end f12 f10], [10-11] the epilogue phase */
   const uint32_t *t = usse_vs_xy.w;
   uint32_t *w = malloc((count + 12) * 8), va;
   uint8_t reloc[2];
   unsigned n = 0, p1, p2;

   if (!w)
      return 0;
   p1 = 6;
   p2 = p1 + 1 + count + 1;
   put(w, &n, p1, PHAS_HI | ((temps + 3) / 4));    /* phase 0 -> phase 1 with its temporaries */
   for (unsigned i = 1; i <= 4; i++)
      put(w, &n, t[2 * i], t[2 * i + 1]);
   put(w, &n, NOP_END_LO, NOP_END_HI);
   put(w, &n, p2, t[2 * 7 + 1]);                    /* phase 1 -> the epilogue */
   put_code(w, &n, code, count);
   put(w, &n, t[2 * 9], t[2 * 9 + 1]);
   put(w, &n, t[2 * 10], t[2 * 10 + 1]);
   put(w, &n, t[2 * 11], t[2 * 11 + 1]);
   reloc[0] = 0;
   reloc[1] = p1;
   va = sgxws_usse_upload(c, true, w, n, reloc, 2);
   free(w);
   return va;
}

/* sop2's fields (hw10, tools/isa/hw_int.py): c1 h6-8 (complement h24), c2
 * h3-5 (h15), a1 h20-21 (h11), a2 h9-10 (h2), cop l18-19, aop l16-17, end
 * h18, dst l21-27 (bank h0-1: o 1, pa 2); the base is
 * `sop2 r0, pa0, o0 [skipinv]` with every factor zero, add.  The alpha
 * selectors number zero, s1a, s2a. */
static uint32_t
alpha_sel(unsigned s)
{
   return (s & 7) == SGXWS_BS_S1A ? 1 : (s & 7) == SGXWS_BS_S2A ? 2 : 0;
}

/* end: the blend is the last instruction and writes o0; otherwise it
 * writes pa<c> back for the colour mask */
void
sgxws_blend_words(const struct sgxws_blend *b, unsigned color_pa, bool end, uint32_t w[2])
{
   uint32_t hi = 0x80800000u;

   hi |= end ? 1u << 18 | 1 : 2;
   hi |= (b->c1 & 7u) << 6 | (b->c1 & SGXWS_BS_INV ? 1u << 24 : 0);
   hi |= (b->c2 & 7u) << 3 | (b->c2 & SGXWS_BS_INV ? 1u << 15 : 0);
   hi |= alpha_sel(b->a1) << 20 | (b->a1 & SGXWS_BS_INV ? 1u << 11 : 0);
   hi |= alpha_sel(b->a2) << 9 | (b->a2 & SGXWS_BS_INV ? 1u << 2 : 0);
   /* src1 l7-13 (bank l30-31), src2 l0-6 (bank l28-29): pa = 2, o = 1 */
   w[0] = (b->swap ? 0x60000000u | color_pa : 0x90000000u | color_pa << 7) |
          (end ? 0 : color_pa << 21) | (b->cop & 3u) << 18 | (b->aop & 3u) << 16;
   w[1] = hi;
}

/* sopwm o0, pa<c>, pa<c> [skipinv end mask=m c1=zero c2=zero cop=max aop=max]
 * (hw12): the mask's x y z w are h12 h13 h14 h11 */
static void
wmask_words(unsigned m, unsigned color_pa, uint32_t w[2])
{
   w[0] = 0xa0000000u | color_pa << 7 | color_pa;
   w[1] = 0x90b40601u | (m & 1 ? 1u << 12 : 0) | (m & 2 ? 1u << 13 : 0) |
          (m & 4 ? 1u << 14 : 0) | (m & 8 ? 1u << 11 : 0);
}

uint32_t
sgxws_upload_fs(struct sgxws_ctx *c, const uint64_t *code, unsigned count, unsigned color_pa,
                const struct sgxws_blend *blend)
{
   uint32_t *w = malloc((count + 3) * 8), va, b[2];
   bool masked = blend && blend->wmask != 0xf;
   unsigned n = 0;

   if (!w)
      return 0;
   put(w, &n, 0, PHAS_HI | 0x700);                  /* phas.i #0, #0 [wait=7] */
   put_code(w, &n, code, count);
   if (blend && blend->blend) {
      sgxws_blend_words(blend, color_pa, !masked, b);
      put(w, &n, b[0], b[1]);
   }
   if (masked) {
      wmask_words(blend->wmask, color_pa, b);
      put(w, &n, b[0], b[1]);
   } else if (!blend || !blend->blend) {
      /* mov o0, pa<c> [skipinv end mask=1], as the DDK writes it (its uniform
       * shader's mov o0, sa9); without the mask this mov locks the GPU up (EA3) */
      put(w, &n, 0xa0000000u | color_pa << 7, 0x28851001u);
   }
   va = sgxws_usse_upload(c, false, w, n, NULL, 0);
   free(w);
   return va;
}

uint32_t
sgxws_upload_fs_secondary(struct sgxws_ctx *c, const uint64_t *code, unsigned count)
{
   uint32_t *w = malloc((count + 2) * 8), va;
   unsigned n = 0;

   if (!w)
      return 0;
   put(w, &n, 0, PHAS_HI | 0x700);
   put_code(w, &n, code, count);
   put(w, &n, NOP_END_LO, NOP_END_HI);
   va = sgxws_usse_upload(c, false, w, n, NULL, 0);
   free(w);
   return va;
}
