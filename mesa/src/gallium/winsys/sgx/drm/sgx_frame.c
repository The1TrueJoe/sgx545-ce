/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * sgxws: SGX545 frames from GL-level parameters.  See sgx_frame.h.
 *
 * Tags in the comments follow docs/STATE.md: [C] hardware (a replay or
 * this driver on the board), [D] capture (the command streams of another
 * driver's frames), [G] guess (a value that works, its meaning unknown).
 */

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>

#include "util/log.h"
#include "util/macros.h"
#include "util/u_debug.h"
#include "util/u_math.h"

#include "sgx_csb.h"
#include "sgx_frame.h"
#include "sgx_pds.h"

/* Placement: PDS programs on 64 bytes, USSE programs on 128, DMA sources on
 * 32 [D: captured PDS programs start on 32; G: what the hardware needs is
 * untested] */
#define PDS_ALIGN  0x40
#define USSE_ALIGN 0x80
#define DMA_ALIGN  0x20

#define H_GENERAL   SGX_HEAP_GENERAL
#define H_PIXSHADER SGX_HEAP_PIXSHADER
#define H_VTXSHADER SGX_HEAP_VTXSHADER
#define H_PDSPIXEL  SGX_HEAP_PDSPIXEL
#define H_PDSVERTEX SGX_HEAP_PDSVERTEX

#define f32    sgxws_f32

uint32_t
sgxws_f32(float x)
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
sgxws_bo_alloc_heap(struct sgxws_dev *d, struct sgxws_bo *bo,
                    enum sgx_heap heap, uint32_t attribs, uint32_t size,
                    uint32_t align)
{
   return d->kif->bo_alloc(d, bo, heap, attribs, size, align);
}

void
sgxws_bo_prepare_target(struct sgxws_dev *d, struct sgxws_bo *bo)
{
   if (d->kif->bo_prepare_target)
      d->kif->bo_prepare_target(d, bo);
}

bool
sgxws_target_twiddled(const struct sgxws_target *t)
{
   return t->t1_layout == SGX_TEXSTATE_LAYOUT_TWIDDLED;
}

void
sgxws_bo_cpu_written(struct sgxws_bo *bo)
{
   if (bo->impl)
      bo->impl->cpu_dirty = true;
}

int
sgxws_bo_alloc(struct sgxws_dev *d, struct sgxws_bo *bo, enum sgxws_mem mem,
               uint32_t size, uint32_t align)
{
   int e;

   /* Whatever the texture unit samples lives in the texture heap: render
    * targets are sampled too (FBO textures, the back buffer a blit reads).
    * Sampling from elsewhere hangs some cores (the sample cache erratum,
    * sgx_core.sample_texture_heap_only); the general heap only when the
    * texture heap is full. */
   switch (mem) {
   case SGXWS_MEM_RENDER_TARGET:
      e = d->core->sample_texture_heap_only
             ? sgxws_bo_alloc_heap(d, bo, SGX_HEAP_TEXTURE, SGXWS_ATTRIBS_COLOR,
                                   size, align)
             : -ENOMEM;
      if (e)
         e = sgxws_bo_alloc_heap(d, bo, SGX_HEAP_GENERAL, SGXWS_ATTRIBS_COLOR,
                                 size, align);
      if (!e)
         sgxws_bo_prepare_target(d, bo);
      return e;
   case SGXWS_MEM_TEXTURE:
      e = sgxws_bo_alloc_heap(d, bo, SGX_HEAP_TEXTURE, 0x3, size, align);
      if (e)
         e = sgxws_bo_alloc_heap(d, bo, SGX_HEAP_GENERAL, 0x3, size, align);
      return e;
   case SGXWS_MEM_VERTEX:
      return sgxws_bo_alloc_heap(d, bo, SGX_HEAP_GENERAL, 0x1, size, align);
   case SGXWS_MEM_ZLS: /* the 3D writes it: read and write */
      return sgxws_bo_alloc_heap(d, bo, SGX_HEAP_GENERAL, 0x3, size, align);
   }
   return -EINVAL;
}

int
sgxws_bo_wrap(struct sgxws_dev *d, struct sgxws_bo *bo, void *cpu,
              uint32_t size)
{
   return d->kif->bo_wrap ? d->kif->bo_wrap(d, bo, cpu, size) : -ENOTSUP;
}

bool
sgxws_dev_has_dmabuf(const struct sgxws_dev *d)
{
   return d->kif->bo_import != NULL;
}

int
sgxws_bo_import(struct sgxws_dev *d, struct sgxws_bo *bo, int dmabuf_fd,
                uint32_t size)
{
   return d->kif->bo_import ? d->kif->bo_import(d, bo, dmabuf_fd, size)
                            : -ENOTSUP;
}

int
sgxws_bo_export(struct sgxws_dev *d, const struct sgxws_bo *bo, int *dmabuf_fd)
{
   return d->kif->bo_export ? d->kif->bo_export(d, bo, dmabuf_fd) : -ENOTSUP;
}

int
sgxws_bo_kms_handle(const struct sgxws_bo *bo, uint32_t *handle)
{
   return bo->impl && bo->impl->kif->bo_kms_handle
             ? bo->impl->kif->bo_kms_handle(bo, handle)
             : -ENOTSUP;
}

void
sgxws_bo_free(struct sgxws_dev *d, struct sgxws_bo *bo)
{
   if (!bo->impl)
      return;
   d->kif->bo_free(d, bo);
   bo->impl = NULL;
   bo->va = 0;
   bo->map = NULL;
}

int
sgxws_bo_wait(const struct sgxws_bo *bo, unsigned timeout_ms)
{
   return bo->impl ? bo->impl->kif->bo_wait(bo, timeout_ms) : 0;
}

static int
ring_init(struct sgxws_dev *d, struct sgxws_ring *r, enum sgx_heap heap,
          uint32_t size)
{
   int e = sgxws_bo_alloc_heap(d, &r->bo, heap, 0x1, size, 0x1000);

   r->size = size;
   /* nothing at a heap's first byte: a PDS state word or DOUTU word of 0 would
    * name it */
   r->used = r->mark = 0x100;
   return e;
}

static void
ring_mark(struct sgxws_ring *r)
{
   r->mark = r->used;
   r->part = ((r->size - r->mark) / r->parts) & ~0xffu;
   r->hi = r->mark + r->part;
}

/* record into slot k: what the frame there before wrote is done with */
static void memo_reset(struct sgxws_pds_memo *m);

static void
ring_use_slot(struct sgxws_ring *r, unsigned k)
{
   if (r->memo)
      memo_reset(r->memo);
   r->used = r->mark + k * r->part;
   r->hi = r->used + r->part;
}

static uint32_t
ring_room(const struct sgxws_ring *r)
{
   return (r->hi ? r->hi : r->size) - r->used;
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

   if (o + size > (r->hi ? r->hi : r->size)) {
      /* sgxws_draw keeps the frame rings from filling; this is a bug */
      mesa_loge("sgx: ring at %#x full (%#x + %#x)", r->bo.va, o, size);
      abort();
   }
   r->used = o + size;
   p.va = r->bo.va + o;
   p.p = r->bo.map + o / 4;
   return p;
}

/* ------------------------------------------------------------------ USSE */

/* A USSE heap is cut into 512 KiB code base windows: a task runs from the
 * window its DOUTU names, and its absolute targets (phas.i, ba) count from
 * that window's start, not the heap's (ISA.md) */
#define USSE_WINDOW_SHIFT 19
#define USSE_WINDOW_MASK ((1u << USSE_WINDOW_SHIFT) - 1)

static uint32_t
usse_heap_off(const struct sgxws_ctx *c, uint32_t va, bool vertex)
{
   return va - c->dev->heap_base[vertex ? H_VTXSHADER : H_PIXSHADER];
}

static bool
code_chunk_init(struct sgxws_dev *d, struct sgxws_code_chunk *k,
                enum sgx_heap heap, uint32_t size)
{
   /* aligned to its size: a chunk never straddles a 512K code base window */
   uint32_t al = 1u << util_logbase2_ceil(size);

   if (sgxws_bo_alloc_heap(d, &k->r.bo, heap, 0x1, size, al))
      return false;
   if ((k->r.bo.va - d->heap_base[heap]) >> USSE_WINDOW_SHIFT) {
      static bool once;

      if (!once)
         mesa_logi("sgx: program code past the first 512 KiB window (%s heap)",
                   heap == H_VTXSHADER ? "vertex" : "pixel");
      once = true;
   }
   k->r.size = size;
   /* nothing at a heap's first byte: a DOUTU word of 0 would name it */
   k->r.used = 0x100;
   return true;
}

/* empty a chunk no render runs code from any more */
static void
code_chunk_reset(struct sgxws_ctx *c, struct sgxws_code_chunk *k)
{
   if (c->dev->kif->code_reset && c->dev->kif->code_reset(c->dev, &k->r.bo))
      mesa_loge("sgx: code chunk reset failed");
   memset(k->r.bo.map, 0, k->r.size);
   k->r.used = 0x100;
   k->r.uploaded = 0;
}

/* room for size bytes of code: the current chunk, an emptied one, a new one,
 * or one waited for; NULL when the heap is full of live programs */
static struct sgxws_code_chunk *
code_room(struct sgxws_ctx *c, struct sgxws_code *code, uint32_t size)
{
   uint32_t need = size + USSE_ALIGN;
   struct sgxws_code_chunk *k, **grown;

   if (code->n) {
      k = code->chunk[code->cur];
      if (k->r.size - k->r.used >= need)
         return k;
   }
   sgxws_done_seq(c);
   for (int pass = 0; pass < 2; pass++) {
      for (unsigned i = 0; i < code->n; i++) {
         k = code->chunk[i];
         if (k->live || k->r.size - 0x100 < need)
            continue;
         if (sgxws_seq_after(k->free_seq, c->done_seq)) {
            /* the second pass waits, the heap being full, for a kick that
             * was made (not the frame being recorded) */
            if (!pass || k->free_seq == sgxws_next_seq(c) ||
                sgxws_wait_seq(c, k->free_seq, 2000))
               continue;
         }
         code_chunk_reset(c, k);
         code->cur = i;
         return k;
      }
      if (pass)
         break;
      k = calloc(1, sizeof(*k));
      grown = realloc(code->chunk, (code->n + 1) * sizeof(*grown));
      if (k && grown) {
         code->chunk = grown;
         if (code_chunk_init(c->dev, k, code->heap,
                             MAX2(SGXWS_CODE_CHUNK, align(need, 0x1000)))) {
            code->chunk[code->n] = k;
            code->cur = code->n++;
            return k;
         }
      } else if (grown) {
         code->chunk = grown;
      }
      free(k);
   }
   return NULL;
}

/* a program's code, 0 when there is no room for it */
uint32_t
sgxws_usse_upload(struct sgxws_ctx *c, bool vertex, const uint32_t *w,
                  unsigned n, const uint8_t *reloc, unsigned nreloc)
{
   struct sgxws_code_chunk *k = code_room(c, &c->usse[vertex], n * 8);
   struct sgxws_ptr p;
   uint32_t pc0;

   if (!k) {
      mesa_loge("sgx: %s program code is full", vertex ? "vertex" : "pixel");
      return 0;
   }
   k->live++;
   p = ring_alloc(&k->r, n * 8, USSE_ALIGN);
   /* from the start of the program's code base window (doutu_w0) */
   pc0 = (usse_heap_off(c, p.va, vertex) & USSE_WINDOW_MASK) / 8;
   memcpy(p.p, w, n * 8);
   /* phas.i targets are absolute, in instructions (ISA.md) */
   for (unsigned i = 0; i < nreloc; i++)
      p.p[2 * reloc[i]] += pc0;
   return p.va;
}

void
sgxws_code_free(struct sgxws_ctx *c, uint32_t va)
{
   if (!va)
      return;
   for (unsigned v = 0; v < 2; v++) {
      struct sgxws_code *code = &c->usse[v];

      for (unsigned i = 0; i < code->n; i++) {
         struct sgxws_code_chunk *k = code->chunk[i];

         if (va >= k->r.bo.va && va < k->r.bo.va + k->r.size) {
            assert(k->live);
            k->live--;
            /* the frame being recorded may still draw with it */
            k->free_seq = sgxws_next_seq(c);
            return;
         }
      }
   }
   mesa_loge("sgx: freeing code at %#x that isn't a program", va);
}

static void
code_fini(struct sgxws_ctx *c, struct sgxws_code *code)
{
   for (unsigned i = 0; i < code->n; i++) {
      sgxws_bo_free(c->dev, &code->chunk[i]->r.bo);
      free(code->chunk[i]);
   }
   free(code->chunk);
}

static uint32_t
usse_load(struct sgxws_ctx *c, const struct sgx_fixed_prog *pr, bool vertex)
{
   return sgxws_usse_upload(c, vertex, pr->w, pr->n, pr->reloc, pr->nreloc);
}

/* DOUTU word 0 (PDS.md): the code base register (pixel 0-9 at the pixel heap +
 * n*512K, vertex 10+: from the device's heap layout), the offset in its window,
 * temporaries in fours */
static uint32_t
doutu_w0(struct sgxws_ctx *c, uint32_t va, bool vertex, uint32_t temps)
{
   uint32_t off = usse_heap_off(c, va, vertex);

   return sgx_pds_doutu_w0(&(struct sgx_pds_doutu){
      .base = (off >> USSE_WINDOW_SHIFT) + c->dev->usse_base_index[vertex],
      .offset = off & USSE_WINDOW_MASK,
      .temps4 = (temps + 3) >> 2,
   });
}

/* DOUTU word 1 of a pixel task (PDS.md) */
static uint32_t
doutu_w1_pixel(bool iterations, bool samples)
{
   return sgx_pds_doutu_w1(&(struct sgx_pds_doutu){.pixel = true,
                                                   .iterations = iterations,
                                                   .samples = samples});
}

/* LIMM rN, #v (ISA.md) */
static void
usse_limm(uint32_t *w, uint32_t v, uint32_t reg)
{
   w[0] = (v & 0x1fffff) | reg << 21;
   w[1] = 0xfca00000 | ((v >> 21) & 0x1f) << 4 | (v >> 26) << 12;
}

/* ------------------------------------------------------------------ PDS */

struct pds {
   uint32_t ds0[64], ds1[64];
   unsigned n0, n1; /* words used in each data store */
   uint32_t code[64];
   unsigned nc;
};

#define SGXWS_PDS_MEMO 4
struct sgxws_pds_memo {
   struct {
      struct pds p;
      uint32_t va, rows;
   } e[SGXWS_PDS_MEMO];
   unsigned n, next;
};

static void
memo_reset(struct sgxws_pds_memo *m)
{
   m->n = m->next = 0;
}

/* The instructions, through the PDS encoder (src/sgx/pds). */
#define DOUTI SGX_PDS_DOUTI
#define DOUTD SGX_PDS_DOUTD
#define DOUTT SGX_PDS_DOUTT
#define DOUTU SGX_PDS_DOUTU
#define DOUTA SGX_PDS_DOUTA
enum {
   MV,
   MV_X,
   MV_CC3
}; /* always; .x2.s27 (DOUTD adds word 2); the vertex-fetch DOUTU */
#define DS0 SGX_PDS_DS0
#define DS1 SGX_PDS_DS1
#define IN  SGX_PDS_IN

/* MOVS (DOUT): r1 = DS0 pair row, r2 = DS1 pair row; selects 0 s1.lo, 1 s1.hi,
 * 2 s2.lo, 3 s2.hi */
static uint32_t
pds_movs(unsigned form, unsigned r1, unsigned r2, unsigned a, unsigned b,
         unsigned c, unsigned d, enum sgx_pds_dout dest)
{
   return sgx_pds_encode(&(struct sgx_pds_instr){
      .opc = SGX_PDS_OPC_MOVS,
      .cc = form == MV_CC3 ? SGX_PDS_CC_3 : SGX_PDS_CC_ALWAYS,
      .x2 = form == MV_X,
      .s27 = form == MV_X,
      .dest = dest,
      .r1 = r1,
      .r2 = r2,
      .sel = {a, b, c, d},
   });
}

/* dst = src1 op src2: add, mul16, and, or, xor, op0xd.s1 */
static uint32_t
pds_alu(enum sgx_pds_opc op, struct sgx_pds_reg src1, struct sgx_pds_reg src2,
        struct sgx_pds_reg dst)
{
   return sgx_pds_encode(&(struct sgx_pds_instr){.opc = op,
                                                 .cc = SGX_PDS_CC_ALWAYS,
                                                 .dst = dst,
                                                 .src1 = src1,
                                                 .src2 = src2});
}

static uint32_t
pds_shl(struct sgx_pds_reg src, unsigned n, struct sgx_pds_reg dst)
{
   return sgx_pds_encode(&(struct sgx_pds_instr){.opc = SGX_PDS_OPC_SHL,
                                                 .cc = SGX_PDS_CC_ALWAYS,
                                                 .dst = dst,
                                                 .src1 = src,
                                                 .amount = n});
}

/* p0 = src == 0 */
static uint32_t
pds_tst(struct sgx_pds_reg src)
{
   bool ds1 = src.file == SGX_PDS_FILE_DS1;

   return sgx_pds_encode(&(struct sgx_pds_instr){
      .opc = ds1 ? SGX_PDS_OPC_TST_S2 : SGX_PDS_OPC_TST,
      .cc = SGX_PDS_CC_ALWAYS,
      .src1 = ds1 ? (struct sgx_pds_reg){0} : src,
      .src2 = ds1 ? src : (struct sgx_pds_reg){0},
   });
}

static uint32_t
pds_bra_p0(unsigned target)
{
   return sgx_pds_encode(&(struct sgx_pds_instr){.opc = SGX_PDS_OPC_BRA,
                                                 .cc = SGX_PDS_CC_P0,
                                                 .target = target});
}

static uint32_t
pds_halt(void)
{
   return sgx_pds_encode(&(struct sgx_pds_instr){.opc = SGX_PDS_OPC_HALT,
                                                 .cc = SGX_PDS_CC_ALWAYS});
}

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
   struct sgxws_pds_memo *mm = r->memo;
   struct sgxws_ptr m;

   /* a program this slot already holds: its address again (draws that
    * differ in their vertex data only share one TA state) */
   if (mm) {
      for (unsigned i = 0; i < mm->n; i++) {
         const struct pds *q = &mm->e[i].p;

         if (q->n0 == p->n0 && q->n1 == p->n1 && q->nc == p->nc &&
             !memcmp(q->ds0, p->ds0, p->n0 * 4) &&
             !memcmp(q->ds1, p->ds1, p->n1 * 4) &&
             !memcmp(q->code, p->code, p->nc * 4)) {
            if (rows_out)
               *rows_out = mm->e[i].rows;
            return mm->e[i].va;
         }
      }
   }
   m = ring_alloc(r, rows * 16 + p->nc * 4, PDS_ALIGN);

   for (uint32_t i = 0; i < rows; i++) {
      m.p[4 * i + 0] = p->ds0[2 * i];
      m.p[4 * i + 1] = p->ds0[2 * i + 1];
      m.p[4 * i + 2] = p->ds1[2 * i];
      m.p[4 * i + 3] = p->ds1[2 * i + 1];
   }
   memcpy(m.p + 4 * rows, p->code, p->nc * 4);
   if (rows_out)
      *rows_out = rows;
   if (mm) {
      unsigned i = mm->n < SGXWS_PDS_MEMO ? mm->n++ : mm->next++ % SGXWS_PDS_MEMO;

      mm->e[i].p = *p;
      mm->e[i].va = m.va;
      mm->e[i].rows = rows;
   }
   return m.va;
}

#define CODE(p, w) ((p)->code[(p)->nc++] = (w))

/* the clear's DOUTI: iterate the colour, 4 components, the last plain iteration
 * [D] */
static const struct sgx_pds_douti clear_douti = {
   .src = 0x20,
   .comps = 4,
   .iterate = true,
   .last_iter = true,
};

/* TA/3D state word for a pixel PDS program */
static uint32_t
pds_word(struct sgxws_ctx *c, uint32_t va, uint32_t rows)
{
   uint32_t w;

   sgx_pack(&w, TA_STATE_PDS_PROGRAM, pds)
   {
      pds.data_rows = rows;
      pds.offset = (va - c->dev->heap_base[H_PDSPIXEL]) >> 4;
   }
   return w;
}

/* TA/3D PDS word 1: the primary attributes in units of 4 dwords, the
 * secondary attribute dwords, b25-28 */
static uint32_t
pds_sizeinfo(uint32_t units, uint32_t sec_dwords, uint32_t b25_28)
{
   uint32_t w;

   sgx_pack(&w, TA_STATE_PDS_SIZEINFO, si)
   {
      si.primary_units = units;
      si.secondary_size = (sec_dwords + 3) >> 2;
      si.unknown_25_28 = b25_28;
   }
   return w;
}

/*
 * pixel program (PDS.md, program forms): DOUTU, then per iterated
 * input a DOUTI, and after a sampling DOUTI the DOUTT with the texture's four
 * words. A DOUTI takes its word from the high half of a DS1 pair (the first
 * shares DS1 pair 0 with the DOUTU) and a zero DS0 pair; a DOUTT takes T0, T1
 * from a DS1 pair and T2, T3 from a DS0 pair, as the background object's does.
 */
static uint32_t
pds_pixel(struct sgxws_ctx *c, uint32_t task_w0, uint32_t task_w1,
          const struct sgx_pds_douti *douti, const uint8_t *douti_tex,
          const uint32_t *t3_clr, const uint32_t *t3_set, unsigned n_douti,
          const uint32_t (*tex)[4], uint32_t *rows)
{
   struct pds p = {0};
   unsigned d0 = 1, d1 = 1;

   pds_d0(&p, 0, task_w0);
   pds_d0(&p, 1, task_w1);
   pds_d1(&p, 0, 0);
   pds_d1(&p, 1, n_douti ? sgx_pds_douti_w0(&douti[0]) : 0);
   CODE(&p, pds_movs(MV, 0, 0, 0, 1, 2, 3, DOUTU));
   for (unsigned i = 0; i < n_douti; i++) {
      unsigned r1 = d0++, r2 = 0;

      if (i) {
         r2 = d1++;
         pds_d1(&p, 2 * r2, 0);
         pds_d1(&p, 2 * r2 + 1, sgx_pds_douti_w0(&douti[i]));
      }
      pds_d0(&p, 2 * r1, sgx_pds_douti_w1(&douti[i]));
      pds_d0(&p, 2 * r1 + 1, 0);
      CODE(&p, pds_movs(MV, r1, r2, 3, 0, 1, 1, DOUTI));
      if (douti_tex && douti_tex[i] && tex) {
         const uint32_t *t = tex[douti_tex[i] - 1];
         unsigned a = d1++, b = d0++;

         pds_d1(&p, 2 * a, t[0]);
         pds_d1(&p, 2 * a + 1, t[1]);
         pds_d0(&p, 2 * b, t[2]);
         pds_d0(&p, 2 * b + 1, (t[3] & ~t3_clr[i]) | t3_set[i]);
         CODE(&p, pds_movs(MV, b, a, 2, 3, 0, 1, DOUTT));
      }
   }
   CODE(&p, pds_halt());
   return pds_emit(&c->pds_pix, &p, rows);
}

/* secondary/state program (PDS.md, program forms): an optional DOUTD
 * of n dwords, then a task */
static uint32_t
pds_secondary(struct sgxws_ring *r, uint32_t src, uint32_t dma_ctrl,
              uint32_t task_w0, uint32_t task_w1, uint32_t *rows)
{
   struct pds p = {0};

   if (src) {
      pds_d0(&p, 0, src);
      pds_d0(&p, 1, dma_ctrl);
      pds_d1(&p, 0, 0);
      pds_d0(&p, 2, task_w0);
      pds_d0(&p, 3, task_w1);
      CODE(&p, pds_movs(MV, 0, 0x18, 0, 1, 0, 1, DOUTD));
      CODE(&p, pds_movs(MV, 1, 0, 0, 1, 2, 2, DOUTU));
   } else {
      pds_d0(&p, 0, task_w0);
      pds_d0(&p, 1, task_w1);
      CODE(&p, pds_movs(MV, 0, 0, 0, 1, 2, 2, DOUTU));
   }
   CODE(&p, pds_halt());
   return pds_emit(r, &p, rows);
}

/* DOUTD control: dwords, attribute register, b22-24 [G], b25 = secondary
 * attributes [G] */
static uint32_t
dma_ctrl(uint32_t dwords, uint32_t reg, uint32_t kind, int secondary)
{
   return sgx_pds_doutd_ctrl(&(struct sgx_pds_doutd){.dwords = dwords,
                                                     .reg = reg,
                                                     .kind = kind,
                                                     .secondary = secondary});
}

/*
 * A secondary/state program for any amount of constants: one DOUTD per 16
 * dwords into consecutive secondary attributes, then the task.  b22-23 of
 * the DMA control word is (dwords-1) & 3 (10/11/12/25 dwords: 1/2/3/0), and
 * no captured DMA moves more than 25 dwords [D for the values, G for the
 * meaning].
 */
static uint32_t
pds_dma_task(struct sgxws_ctx *c, struct sgxws_ring *r, uint32_t src,
             uint32_t dwords, uint32_t task_w0, uint32_t task_w1,
             uint32_t *rows)
{
   const uint32_t chunk = c->dev->core->dma_chunk_dwords;
   struct pds p = {0};
   unsigned row = 0;

   for (uint32_t off = 0; off < dwords; off += chunk, row++) {
      uint32_t n = dwords - off < chunk ? dwords - off : chunk;

      pds_d0(&p, 2 * row, src + off * 4);
      pds_d0(&p, 2 * row + 1, dma_ctrl(n, off, (n - 1) & 3, 1));
      CODE(&p, pds_movs(MV, row, 0x18, 0, 1, 0, 1, DOUTD));
   }
   pds_d1(&p, 0, 0);
   pds_d0(&p, 2 * row, task_w0);
   pds_d0(&p, 2 * row + 1, task_w1);
   CODE(&p, pds_movs(MV, row, 0, 0, 1, 2, 2, DOUTU));
   CODE(&p, pds_halt());
   return pds_emit(r, &p, rows);
}

/* ------------------------------------------------------------------ device */

struct sgxws_dev *
sgxws_dev_create(int fd, const struct sgxws_dev_options *opts)
{
   const struct sgxws_dev_options none = {.dry_run = -1};
   struct sgxws_dev *d = NULL;
   drmVersionPtr v = drmGetVersion(fd);
   int e = -ENODEV;

   if (!opts)
      opts = &none;
   if (v && !strcmp(v->name, "powervr_sgx"))
      e = sgxws_powervr_sgx_dev_create(fd, opts, &d);
#ifdef SGX_PVRSRVKM
   else if (v && !strcmp(v->name, "pvrsrvkm"))
      e = sgxws_pvr_dev_create(fd, opts, &d);
#endif
   if (e)
      mesa_loge("sgx: no SGX kernel driver on fd %d (%s): %d", fd,
                v ? v->name : "?", e);
   drmFreeVersion(v);
   if (e)
      return NULL;
   /* a core this winsys has a description of (sgx_core.h), or none */
   d->core = sgx_core_lookup(d->info.core_id, d->info.core_revision);
   if (!d->core) {
      mesa_loge(
         "sgx: SGX core id %#x revision %#x has no description (sgx_core.c)",
         d->info.core_id, d->info.core_revision);
      d->kif->dev_destroy(d);
      return NULL;
   }
   /* render-target limits the kernel didn't report: the core's */
   if (!d->info.rt_max_width) {
      d->info.rt_max_width = d->core->rt_max_width;
      d->info.rt_max_height = d->core->rt_max_height;
      d->info.rt_max_samples = d->core->rt_max_samples;
   }
   d->verbose = opts->verbose;
   d->flush_rt = opts->flush_rt;
   d->frame_gap_us = opts->frame_gap_us;
   d->state_always = opts->state_always;
   /* four: the next frame records while this one renders (a game's frame is
    * three renders); docs/PERF.md */
   d->slots = opts->slots ? CLAMP(opts->slots, 2, SGXWS_MAX_SLOTS) : 4;
   d->ta_split = d->kif->kick_ta ? opts->ta_split : 0;
   d->dry_run = opts->dry_run;

   drmDevicePtr dev = NULL;
   if (!drmGetDevice2(fd, 0, &dev)) {
      if (dev->bustype == DRM_BUS_PCI) {
         d->info.pci_vendor = dev->deviceinfo.pci->vendor_id;
         d->info.pci_device = dev->deviceinfo.pci->device_id;
      }
      drmFreeDevice(&dev);
   }
   return d;
}

void
sgxws_dev_destroy(struct sgxws_dev *d)
{
   d->kif->dev_destroy(d);
}

const struct sgxws_device_info *
sgxws_dev_info(const struct sgxws_dev *d)
{
   return &d->info;
}

/* ------------------------------------------------------------------ context */

#define IDX_COUNT 0x10000

static uint32_t clear_pds_masked(struct sgxws_ctx *c, unsigned wmask);

static void
ctx_free(struct sgxws_ctx *c)
{
   struct sgxws_ring *rings[] = {&c->pds_pix, &c->pds_vtx,     &c->gen,
                                 &c->vdm,     &c->eot_pool[0], &c->eot_pool[1]};

   for (unsigned i = 0; i < sizeof(rings) / sizeof(rings[0]); i++)
      sgxws_bo_free(c->dev, &rings[i]->bo);
   code_fini(c, &c->usse[0]);
   code_fini(c, &c->usse[1]);
   for (unsigned v = 0; v < 2; v++)
      sgxws_bo_free(c->dev, &c->code_hold[v]);
   free(c->pds_pix.memo);
   free(c->last_u);
   c->dev->kif->ctx_fini(c);
   free(c);
}

struct sgxws_ctx *
sgxws_ctx_create(struct sgxws_dev *d, int priority)
{
   struct sgxws_ctx *c = calloc(1, sizeof(*c));
   uint32_t rows;
   struct sgxws_ptr p;
   struct pds t = {0};

   if (!c)
      return NULL;
   c->dev = d;
   c->priority = priority;
   c->slots = d->slots;
   /* the kernel side first: under services, the kernel-data and sync
    * allocations come before the rings */
   if (d->kif->ctx_init(c)) {
      free(c);
      return NULL;
   }

   c->usse[0].heap = H_PIXSHADER;
   c->usse[1].heap = H_VTXSHADER;
   if (/* the frame rings hold a whole frame of a game (~1500 draws, ~100K
        * vertices): a ring that fills submits the frame so far and waits for
        * it (heaps: PDS pixel 32 MB, PDS vertex 28 MB, general 2.8 GB) */
       ring_init(d, &c->pds_pix, H_PDSPIXEL, 0x40000 + d->slots * 0x200000) ||
       ring_init(d, &c->pds_vtx, H_PDSVERTEX, 0x40000 + d->slots * 0x200000) ||
       ring_init(d, &c->gen, H_GENERAL, 0x80000 + d->slots * 0x1000000) ||
       ring_init(d, &c->vdm, H_GENERAL, d->slots * 0x80000 + 0x1000) ||
       ring_init(d, &c->eot_pool[0], H_PIXSHADER,
                 SGXWS_EOT_CACHE * 0x80 + 0x100) ||
       ring_init(d, &c->eot_pool[1], H_PIXSHADER,
                 SGXWS_EOT_CACHE * 0x80 + 0x100))
      goto fail;
   /* (without it, every draw's pixel programs anew) */
   if (!d->state_always)
      c->pds_pix.memo = calloc(1, sizeof(*c->pds_pix.memo));

   /* SGX_CODE_WINDOW=n: program code starts in window n of each USSE heap,
    * to test code past the first window (the space below it is held) */
   unsigned skip = debug_get_num_option("SGX_CODE_WINDOW", 0);
   for (unsigned v = 0; skip && v < 2; v++) {
      enum sgx_heap heap = v ? H_VTXSHADER : H_PIXSHADER;

      if (sgxws_bo_alloc_heap(d, &c->code_hold[v], heap, 0x1,
                              (skip << USSE_WINDOW_SHIFT) - SGXWS_CODE_CHUNK,
                              SGXWS_CODE_CHUNK))
         mesa_logw("sgx: SGX_CODE_WINDOW=%u: no room to hold", skip);
   }

   /* fixed USSE programs */
   char err[160];
   if (!sgx_fixed_progs_build(&c->fixed, err, sizeof(err))) {
      mesa_loge("sgx: %s", err);
      goto fail;
   }
   c->empty_v = usse_load(c, &c->fixed.empty, true);
   c->empty_p = usse_load(c, &c->fixed.empty, false);
   c->copy_fs = usse_load(c, &c->fixed.copy_fs, false);
   c->clear_vs = usse_load(c, &c->fixed.clear_vs, true);
   c->vpgen_vs = usse_load(c, &c->fixed.vpgen_vs, true);
   c->pe_ptoff = usse_load(c, &c->fixed.pe_ptoff, false);
   c->pe_emit = usse_load(c, &c->fixed.pe_emit, false);
   {
      /* phas, the secondary programs' prologue up to its wdf, nop [end] */
      uint32_t w[12];

      w[0] = 0;
      w[1] = 0xfa440700;
      memcpy(w + 2, c->fixed.fs_uniform_sec.w + 2, 4 * 8);
      w[10] = 0x00000000;
      w[11] = 0xf8040140;
      c->sec_wait = sgxws_usse_upload(c, false, w, 6, NULL, 0);
   }
   /* every state-emit length up front: the code rings are never reset */
   for (int n = 1; n <= 32; n++)
      c->state_emit[n] = usse_load(c, &c->fixed.state_emit[n], true);

   /* VDM state programs that every frame ends with */
   c->empty_state = pds_secondary(&c->pds_vtx, 0, 0,
                                  doutu_w0(c, c->empty_v, true, 0), 0, NULL);
   /* TA terminate (VDM type 6, PDS.md "DOUTA"): DOUTA (0x2000, 0), 3 x
    * (0x16000e, 0x100), then a state-emit task for the two attributes [D; the
    * values' meaning G] */
   pds_d0(&t, 0, 0x2000);
   pds_d0(&t, 1, sgx_pds_douta_ctrl(&(struct sgx_pds_douta){.reg = 0}));
   pds_d0(&t, 2, 0x0016000e);
   pds_d0(&t, 3, sgx_pds_douta_ctrl(&(struct sgx_pds_douta){.reg = 1}));
   pds_d0(&t, 4, doutu_w0(c, c->state_emit[2], true, 0));
   pds_d0(&t, 5, 0);
   CODE(&t, pds_movs(MV, 0, 0x18, 0, 1, 2, 2, DOUTA));
   for (int i = 0; i < 3; i++)
      CODE(&t, pds_movs(MV, 1, 0x18, 0, 1, 2, 2, DOUTA));
   CODE(&t, pds_movs(MV, 2, 0, 0, 1, 2, 2, DOUTU));
   CODE(&t, pds_halt());
   c->terminate = pds_emit(&c->pds_vtx, &t, &rows);

   /* pixel programs shared by every object without inputs, and the clear's */
   c->pds_empty_p = pds_secondary(&c->pds_pix, 0, 0,
                                  doutu_w0(c, c->empty_p, false, 0), 0, NULL);
   /* DOUTI: iterate the colour (source 0x20), 4 components, plain iteration,
    * last [D] */
   c->clear_pds =
      pds_pixel(c, doutu_w0(c, c->copy_fs, false, 0),
                doutu_w1_pixel(true, false), &clear_douti, NULL, NULL, NULL, 1,
                NULL, NULL);

   /* identity index buffer: glDrawArrays draws through it (STATE.md section 2) */
   p = ring_alloc(&c->gen, IDX_COUNT * 2, 4);
   for (int i = 0; i < IDX_COUNT; i += 2)
      p.p[i / 2] = (uint32_t)i | (uint32_t)(i + 1) << 16;
   c->idx = p.va;
   c->idx_count = IDX_COUNT;
   /* what a fetch of n dwords leaves out, at + 16 n: (0, 0, 1), (0, 1), (1),
    * each 16-byte aligned like every DMA source */
   p = ring_alloc(&c->gen, 64, DMA_ALIGN);
   memset(p.p, 0, 64);
   p.p[3] = p.p[6] = p.p[9] = p.p[12] = f32(1.0f);
   c->defaults = p.va;

   /* The clear's programs for every colour mask, made now: the persistent
    * part of the rings ends at the mark.  Made on demand they landed in the
    * frame part, which every frame reuses, so the next frame's masked clear
    * (an application that clears alpha at every swap) ran whatever
    * that frame had written there. */
   for (unsigned m = 0; m < 16; m++)
      clear_pds_masked(c, m);

   c->pds_pix.parts = c->pds_vtx.parts = c->gen.parts = c->vdm.parts = c->slots;
   ring_mark(&c->pds_pix);
   ring_mark(&c->pds_vtx);
   ring_mark(&c->gen);
   ring_mark(&c->vdm);
   return c;

fail:
   mesa_loge("sgx: context creation failed");
   ctx_free(c);
   return NULL;
}

void
sgxws_ctx_destroy(struct sgxws_ctx *c)
{
   sgxws_flush(c, 0);
   sgxws_wait_idle(c, 2000);
   ctx_free(c);
}

/* ------------------------------------------------------------------ render
 * target */

/* SGXAddRenderTarget's numbers (STATE.md sections 4 and 6; tools/state/build.py
 * rt_numbers) */
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
   struct sgxws_rt *s = calloc(1, sizeof(*s));

   if (!s)
      return NULL;
   s->w = w;
   s->h = h;
   rt_numbers(&s->n, w, h);
   if (c->dev->kif->rt_init(c, s)) {
      free(s);
      return NULL;
   }
   return s;
}

enum sgxws_reset
sgxws_reset_status(struct sgxws_ctx *c)
{
   enum sgxws_reset r = c->reset;

   c->reset = SGXWS_RESET_NONE;
   return r;
}

void
sgxws_rt_destroy(struct sgxws_ctx *c, struct sgxws_rt *s)
{
   if (c->rt == s)
      sgxws_flush(c, 0);
   sgxws_wait_idle(c, 2000);
   c->dev->kif->rt_fini(c, s);
   free(s);
}

/* ------------------------------------------------------------------ per-frame
 * programs */

/* The end-of-tile program (STATE.md section 4): LIMM r0-r3 = the four PBE words,
 * EMITPIX them, EMITPIX the sideband word (STATE.md section 4).  It is the
 * target's, so it is made once per target and kept while its pool is in
 * use: code is never rewritten under a render that may run it. */
static uint32_t
eot_program(struct sgxws_ctx *c)
{
   const struct sgxws_rt *s = c->rt;
   const struct sgxws_target *t = &c->target;
   struct sgxws_ring *pool = &c->eot_pool[c->eot_cur];
   uint32_t pbe[4], side;
   struct sgxws_ptr p;

   for (unsigned i = 0; i < c->n_eot; i++) {
      const struct sgxws_eot *e = &c->eot[i];

      if (e->va == t->va && e->stride == t->stride && e->w == s->w &&
          e->h == s->h && e->pbe_format == t->pbe_format &&
          e->t1_layout == t->t1_layout) {
         pool->last_seq = sgxws_next_seq(c);
         return e->code;
      }
   }
   if (c->n_eot == SGXWS_EOT_CACHE || ring_room(pool) < 7 * 8 + USSE_ALIGN) {
      /* the other pool, emptied once the last render that ran code from it is
       * done */
      unsigned other = !c->eot_cur;

      pool = &c->eot_pool[other];
      if (sgxws_seq_after(pool->last_seq, c->done_seq) &&
          sgxws_wait_seq(c, pool->last_seq, 2000))
         mesa_loge(
            "sgx: a render with the end-of-tile pool's code did not finish");
      if (c->dev->kif->code_reset && c->dev->kif->code_reset(c->dev, &pool->bo))
         mesa_loge("sgx: end-of-tile pool reset failed");
      memset(pool->bo.map, 0, pool->size);
      pool->used = 0x100;
      pool->uploaded = 0;
      c->eot_cur = other;
      c->n_eot = 0;
   }

   p = ring_alloc(pool, 7 * 8, USSE_ALIGN);
   sgx_pack(&pbe[0], PBE_STATE_WORD0, w0)
   { /* clip min [D] */
      w0.x_min = 0;
      w0.y_min = 0;
   }
   sgx_pack(&pbe[1], PBE_STATE_WORD1, w1)
   { /* clip max [C] */
      w1.x_max = s->w - 1;
      w1.y_max = s->h - 1;
   }
   sgx_pack(&pbe[2], PBE_STATE_WORD2, w2) /* b15 [D] */
      w2.pitch = (t->stride - 1) >> 1;    /* [C] */
   sgx_pack(&pbe[3], PBE_STATE_WORD3, w3) /* [D] */
      w3.address = SGX_DEV_ADDR(t->va);
   sgx_pack(&side, PBE_SIDEBAND, sb)
   {
      sb.format = t->pbe_format;
      sb.dither = true;
      sb.twiddled = t->t1_layout == SGX_TEXSTATE_LAYOUT_TWIDDLED;
   }
   p.p[0] = 0; /* phas.i #0, #0 [wait=7] */
   p.p[1] = 0xfa440700;
   for (uint32_t r = 0; r < 4; r++)
      usse_limm(p.p + 2 + 2 * r, pbe[r], r);
   p.p[10] = 0x00200080; /* emit the four PBE words */
   p.p[11] = 0xfb200100;
   p.p[12] = (side & 0x3f) << 22 | 0x00208180; /* emit the sideband word, end */
   p.p[13] = 0xfb240000 | (side & 0x3000) << 10 | ((side >> 3) & 0x1f8);
   c->eot[c->n_eot++] = (struct sgxws_eot){
      .va = t->va,
      .stride = t->stride,
      .w = s->w,
      .h = s->h,
      .pbe_format = t->pbe_format,
      .t1_layout = t->t1_layout,
      .code = p.va,
   };
   pool->last_seq = sgxws_next_seq(c);
   return p.va;
}

/* The pixel event program (PDS.md "pixel-event program decoded"):
 *   in1 & 8 -> task ds0[0];  in1 & 2 -> (in1 ^ ds1[0]) & 0x100 ? DOUTA +
 * end-of-tile task : task ds0[2]; in1 & 4 -> task ds0[2]. */
static uint32_t
pixel_event(struct sgxws_ctx *c, uint32_t eot, uint32_t *rows)
{
   struct pds p = {0};

   pds_d0(&p, 0, doutu_w0(c, c->pe_ptoff, false, 0));
   pds_d0(&p, 1, 0);
   pds_d1(&p, 0, 0);
   pds_d1(&p, 1, doutu_w0(c, eot, false, 4));
   pds_d0(&p, 2, doutu_w0(c, c->pe_emit, false, 4));
   pds_d0(&p, 3,
          sgx_pds_doutu_w1(&(struct sgx_pds_doutu){.b22 = true, .b23 = true}));
   pds_d1(&p, 2, 8);
   pds_d1(&p, 3, 2);
   pds_d0(&p, 4, 0x100);
   pds_d0(&p, 5, 0xff8);
   pds_d1(&p, 4, 4);
   pds_d1(&p, 5, 0);
   pds_d0(&p, 6, sgx_pds_doutu_w1(&(struct sgx_pds_doutu){.b23 = true}));
   pds_d0(&p, 7, 0x003ff000);
   pds_d1(&p, 6, 0);
   pds_d1(&p, 7, 0);
   pds_d0(&p, 8, sgx_pds_doutu_w1(&(struct sgx_pds_doutu){.b22 = true}));
   pds_d0(&p, 9, 0);
   pds_d1(&p, 8, 0);
   pds_d1(&p, 9, 0);
   /*  0 */ CODE(&p, pds_alu(SGX_PDS_OPC_AND, IN(1), DS1(2), DS1(0x30)));
   /*  1 */ CODE(&p, pds_tst(DS1(0x30)));
   /*  2 */ CODE(&p, pds_bra_p0(5));
   /*  3 */ CODE(&p, pds_movs(MV, 0, 0, 0, 1, 0, 0, DOUTU));
   /*  4 */ CODE(&p, pds_halt());
   /*  5 */ CODE(&p, pds_alu(SGX_PDS_OPC_AND, IN(1), DS1(3), DS1(0x30)));
   /*  6 */ CODE(&p, pds_tst(DS1(0x30)));
   /*  7 */ CODE(&p, pds_bra_p0(24));
   /*  8 */ CODE(&p, pds_alu(SGX_PDS_OPC_OP0XD_S1, IN(1), DS1(0), DS1(0x30)));
   /*  9 */ CODE(&p, pds_alu(SGX_PDS_OPC_AND, DS0(4), DS1(0x30), DS1(0x30)));
   /* 10 */ CODE(&p, pds_tst(DS1(0x30)));
   /* 11 */ CODE(&p, pds_bra_p0(27));
   /* 12 */ CODE(&p,
                 pds_movs(MV, 0x21, 0, 0, 2, 0, 0, DOUTA)); /* (in2, ds1[0]) */
   /* 13 */ CODE(&p, pds_shl(IN(0), 3, DS1(0x30)));
   /* 14 */ CODE(&p, pds_alu(SGX_PDS_OPC_AND, DS0(5), DS1(0x30), DS1(0x30)));
   /* 15 */ CODE(&p, pds_alu(SGX_PDS_OPC_OR, DS0(6), DS1(0x30), DS0(0x30)));
   /* 16 */ CODE(&p, pds_shl(IN(0), 3, DS1(0x30)));
   /* 17 */ CODE(&p, pds_alu(SGX_PDS_OPC_AND, DS0(7), DS1(0x30), DS1(0x30)));
   /* 18 */ CODE(&p, pds_alu(SGX_PDS_OPC_OR, DS0(0x30), DS1(0x30), DS0(0x30)));
   /* 19 */ CODE(&p, pds_shl(IN(1), 20, DS1(0x30)));
   /* 20 */ CODE(&p, pds_alu(SGX_PDS_OPC_AND, DS0(8), DS1(0x30), DS1(0x30)));
   /* 21 */ CODE(&p, pds_alu(SGX_PDS_OPC_OR, DS0(0x30), DS1(0x30), DS0(0x30)));
   /* 22 */ CODE(&p, pds_movs(MV, 0x18, 0, 3, 0, 0, 0,
                              DOUTU)); /* (ds1[1], ds0[0x30], ...) */
   /* 23 */ CODE(&p, pds_halt());
   /* 24 */ CODE(&p, pds_alu(SGX_PDS_OPC_AND, IN(1), DS1(4), DS1(0x30)));
   /* 25 */ CODE(&p, pds_tst(DS1(0x30)));
   /* 26 */ CODE(&p, pds_bra_p0(28));
   /* 27 */ CODE(&p, pds_movs(MV, 1, 0, 0, 1, 0, 0, DOUTU));
   /* 28 */ CODE(&p, pds_halt());
   return pds_emit(&c->pds_pix, &p, rows);
}

/* The background object: every tile starts by loading the target's old
 * contents (a sample of the surface as a strided texture) [D]. */
static void
background(struct sgxws_ctx *c)
{
   const struct sgxws_rt *s = c->rt;
   struct pds p = {0};
   uint32_t rows, va, tex[4];

   pds_d0(&p, 0, doutu_w0(c, c->copy_fs, false, 0));
   pds_d0(&p, 1, doutu_w1_pixel(false, true));
   pds_d1(&p, 0, 0);
   /* DOUTI: sample texture coordinate set 0, a pure sample (source 0x24), the
    * last */
   pds_d1(&p, 1,
          sgx_pds_douti_w0(&(struct sgx_pds_douti){.sample_set = 1,
                                                   .src = 0x24,
                                                   .last_sample = true}));
   pds_d0(&p, 2, 0);
   pds_d0(&p, 3, 0);
   /* DOUTT: T0 clamps S and T and, strided, holds the pitch (TEX.md), else
    * nearest, no mipmaps; T1 = the target's layout, format and size; T2 =
    * address; T3 [G] */
   if (c->target.t1_layout == SGX_TEXSTATE_LAYOUT_STRIDED) {
      sgx_pack(&tex[0], TEXSTATE_STRIDED_WORD0, t0)
      {
         t0.wrap_s = t0.wrap_t = SGX_TEXSTATE_WRAP_CLAMP;
         t0.pitch_lo = (c->target.stride - 1) & 7;
         t0.pitch_hi = (c->target.stride - 1) >> 3;
      }
   } else {
      sgx_pack(&tex[0], TEXSTATE_WORD0, t0)
      {
         t0.wrap_s = t0.wrap_t = SGX_TEXSTATE_WRAP_CLAMP;
         t0.no_mipmaps = true;
      }
   }
   sgx_pack(&tex[1], TEXSTATE_WORD1, t1)
   {
      t1.layout = c->target.t1_layout;
      t1.format = c->target.t1_format;
      t1.width = s->w - 1;
      t1.height = s->h - 1;
   }
   sgx_pack(&tex[2], TEXSTATE_WORD2, t2) t2.address =
      SGX_DEV_ADDR(c->target.va);
   sgx_pack(&tex[3], TEXSTATE_WORD3, t3)
   {
   }
   pds_d1(&p, 2, tex[0]);
   pds_d1(&p, 3, tex[1]);
   pds_d0(&p, 4, tex[2]);
   pds_d0(&p, 5, tex[3]);
   CODE(&p, pds_movs(MV, 0, 0, 0, 1, 2, 3, DOUTU));
   CODE(&p, pds_movs(MV, 1, 0, 3, 0, 1, 1, DOUTI));
   CODE(&p, pds_movs(MV, 2, 1, 2, 3, 0, 1, DOUTT));
   CODE(&p, pds_halt());
   va = pds_emit(&c->pds_pix, &p, &rows);
   c->bg_pds[0] = pds_word(c, c->pds_empty_p, 1);
   c->bg_pds[1] = pds_sizeinfo(1, 0, 0);
   c->bg_pds[2] = pds_word(c, va, rows);
}

/* ------------------------------------------------------------------ recording */

bool
sgxws_recording(struct sgxws_ctx *c)
{
   return c->rt != NULL;
}

const struct sgxws_bo *
sgxws_target_bo(const struct sgxws_ctx *c)
{
   return c->rt ? c->target.bo : NULL;
}

int
sgxws_begin(struct sgxws_ctx *c, struct sgxws_rt *rt,
            const struct sgxws_target *t)
{
   /* the same memory: the frame goes on.  The caller's buffer is the one
    * the frame now renders for (sgxws_target_bo): another resource of the
    * same memory may have begun it, and a readback of this one must find
    * its render here and end it (fixed-clip-enables under
    * ARB_framebuffer_object read the frame before, with this left stale) */
   if (c->rt == rt && c->target.va == t->va) {
      c->target.bo = t->bo;
      return 0;
   }
   if (c->rt) {
      int e = sgxws_flush(c, 0);

      if (e)
         return e;
   }
   c->rt = rt;
   c->target = *t;
   c->vdm_start = c->vdm.bo.va + c->vdm.used;
   c->draws = 0;
   c->ta_kicks = c->seg_draws = 0;
   c->last_blk_n = c->last_u_va = 0;
   c->n_use = 0;
   c->bias_set = false;
   c->bias_units = 0;
   c->bias_factor = 0.0f;
   c->clip = (struct sgxws_rect){0, 0, rt->w, rt->h};
   c->damage = (struct sgxws_rect){0, 0, 0, 0};

   /* the render details are the target's: written at the kick, once its
    * previous render is done */
   c->pixev = pixel_event(c, eot_program(c), &c->pixev_rows);
   background(c);
   return 0;
}

static uint32_t *
vdm_slot(struct sgxws_ctx *c)
{
   return ring_alloc(&c->vdm, 4, 4).p;
}

/* a state program in the VDM stream (STATE.md section 2): the program, then
 * its data rows and what it DMAs */
static void
vdm_state(struct sgxws_ctx *c, enum SGX_VDMCTRL_BLOCK_TYPE type, uint32_t va,
          uint32_t rows, enum SGX_VDMCTRL_STATE_KIND kind, uint32_t dwords,
          uint32_t buffer_index)
{
   sgx_pack(vdm_slot(c), VDMCTRL_PDS_STATE0, s0)
   {
      s0.block_type = type;
      s0.pds_addr = SGX_DEV_ADDR(va);
   }
   sgx_pack(vdm_slot(c), VDMCTRL_PDS_STATE1, s1)
   {
      s1.data_rows = rows;
      s1.kind = kind;
      s1.attr_size = (dwords + 3) >> 2;
      s1.buffer_index = buffer_index;
   }
}

/* A TA state block goes to the MTE through a VDM state program: DMA the block
 * into USSE attributes, run a task that emits them (STATE.md section 2) */
static void
state_block(struct sgxws_ctx *c, const uint32_t *blk, uint32_t n,
            uint32_t dma_dwords, uint32_t kind)
{
   struct sgxws_ptr b = ring_alloc(&c->pds_vtx, dma_dwords * 4, DMA_ALIGN);
   struct pds p = {0};
   uint32_t rows, va;

   /* (the frame rings' headroom covers an object and its clip objects) */
   memset(b.p, 0, dma_dwords * 4);
   memcpy(b.p, blk, n * 4);
   /* the VDM state program (PDS.md): tst; bra; doutd; tst; bra; doutd; doutu */
   pds_d0(&p, 0, b.va);
   pds_d0(&p, 1, dma_ctrl(dma_dwords, 0, kind, 1));
   pds_d0(&p, 2, 0);
   pds_d0(&p, 3, 0);
   pds_d0(&p, 4, doutu_w0(c, c->state_emit[n], true, 0));
   pds_d0(&p, 5, 0);
   CODE(&p, pds_tst(DS0(0)));
   CODE(&p, pds_bra_p0(6));
   CODE(&p, pds_movs(MV, 0, 0, 0, 1, 0, 0, DOUTD));
   CODE(&p, pds_tst(DS0(2)));
   CODE(&p, pds_bra_p0(6));
   CODE(&p, pds_movs(MV, 1, 0, 0, 1, 0, 0, DOUTD));
   CODE(&p, pds_movs(MV, 2, 0, 0, 1, 2, 0, DOUTU));
   CODE(&p, pds_halt());
   va = pds_emit(&c->pds_vtx, &p, &rows);
   vdm_state(c, SGX_VDMCTRL_BLOCK_TYPE_PDS_STATE, va, rows,
             SGX_VDMCTRL_STATE_KIND_TA_BLOCK, n, c->vdm_counter);
   c->vdm_counter = (c->vdm_counter + 1) & 3;
   c->last_blk_n = n <= ARRAY_SIZE(c->last_blk) ? n : 0;
   if (c->last_blk_n)
      memcpy(c->last_blk, blk, n * 4);
}

/* Vertices per USSE task: 128 / output dwords, at most 16 [D: 4 and 8
 * output dwords 16 vertices, 12 10, 16 8, 20 6, 24 5; the input dwords don't
 * count, 16 in still gives 16].  Half that (64 / output dwords) hung the GPU
 * at 12 output dwords. */
static uint32_t
vertices_per_task(const struct sgx_core *core, uint32_t out_dwords)
{
   uint32_t o = (out_dwords + 3) & ~3u,
            v = core->vertex_task_dwords / (o ? o : 4);

   return v > core->vertex_task_max ? core->vertex_task_max : v;
}

/* A draw: index list + vertex fetch program (STATE.md section 2; PDS.md,
 * program forms): per stride a MUL16 of the index into ds1[0x32],
 * then each DMA adds it to its address; the defaults DMA plainly. */
static void
vdm_fetch(struct sgxws_ctx *c, uint32_t prim, uint32_t count, uint32_t idx,
          const struct sgxws_fetch *f, unsigned nf, uint32_t vs,
          uint32_t in_dwords, uint32_t out_dwords, uint32_t temps, uint32_t vpt)
{
   struct pds p = {0};
   uint32_t rows, va, batch = 16, row = 0, nstride = 0, last = ~0u;

   for (unsigned i = 0; i < nf; i++, row++) {
      if (f[i].stride != last) {
         pds_d1(&p, 2 * nstride, f[i].stride);
         pds_d1(&p, 2 * nstride + 1, 0);
         CODE(&p, pds_alu(SGX_PDS_OPC_MUL16, IN(0), DS1(2 * nstride),
                          DS1(0x32))); /* index * stride */
         nstride++;
         last = f[i].stride;
      }
      pds_d0(&p, 2 * row, f[i].va);
      pds_d0(&p, 2 * row + 1, dma_ctrl(f[i].dwords, f[i].reg, 0, 0));
      CODE(&p, pds_movs(MV_X, row, 0x19, 0, 1, 2, 2, DOUTD)); /* + ds1[0x32] */
      if (f[i].defaults && f[i].dwords < 4) {
         row++;
         pds_d0(&p, 2 * row, c->defaults + f[i].dwords * 16);
         pds_d0(&p, 2 * row + 1,
                dma_ctrl(4 - f[i].dwords, f[i].reg + f[i].dwords, 0, 0));
         CODE(&p, pds_movs(MV, row, 0, 0, 1, 2, 2, DOUTD));
      }
   }
   pds_d0(&p, 2 * row, doutu_w0(c, vs, true, temps));
   pds_d0(&p, 2 * row + 1, 0);
   CODE(&p, pds_movs(MV_CC3, row, 0, 0, 1, 3, 3, DOUTU));
   CODE(&p, pds_halt());
   va = pds_emit(&c->pds_vtx, &p, &rows);
   sgx_pack(vdm_slot(c), VDMCTRL_INDEX_LIST0, w0)
   {
      w0.prim = prim;
      w0.index_count = count;
   }
   sgx_pack(vdm_slot(c), VDMCTRL_INDEX_LIST1, w1) w1.index_addr =
      SGX_DEV_ADDR(idx); /* 16-bit indices */
   sgx_pack(vdm_slot(c), VDMCTRL_INDEX_LIST2, w2) w2.batch = batch - 1;
   sgx_pack(vdm_slot(c), VDMCTRL_INDEX_LIST3, w3)
   {
   }
   sgx_pack(vdm_slot(c), VDMCTRL_INDEX_LIST4, w4)
   {
      w4.vertices_per_task = vpt - 1;
      w4.pds_addr = SGX_DEV_ADDR(va);
   }
   sgx_pack(vdm_slot(c), VDMCTRL_INDEX_LIST5, w5)
   {
      w5.out_size = (out_dwords + 3) >> 2;
      w5.n_min3 = vpt - 1 < 3 ? vpt - 1 : 3;
      w5.in_size = (in_dwords * 4 + 0xf) >> 4;
      w5.data_rows = rows;
   }
}

/* the vertices packed, attr_dwords apart at vb: DMAs of at most 16 dwords,
 * as the secondary DMAs (a longer one lost what came after: a lit vertex of
 * 5 attributes, 20 dwords, got garbage in its last one) */
static void
vdm_draw(struct sgxws_ctx *c, uint32_t prim, uint32_t count, uint32_t idx,
         uint32_t vb, uint32_t stride, uint32_t attr_dwords, uint32_t vs,
         uint32_t in_dwords, uint32_t out_dwords, uint32_t temps, uint32_t vpt)
{
   struct sgxws_fetch f[8];
   unsigned nf = 0;

   for (uint32_t off = 0; off < attr_dwords && nf < 8; off += 16)
      f[nf++] = (struct sgxws_fetch){
         .va = vb + off * 4,
         .stride = stride,
         .dwords = attr_dwords - off < 16 ? attr_dwords - off : 16,
         .reg = off};
   vdm_fetch(c, prim, count, idx, f, nf, vs, in_dwords, out_dwords, temps, vpt);
}

/* headroom one object needs in the frame rings, beyond its vertices */
#define OBJ_PDS 0x800
#define OBJ_VDM 0x200

static bool
frame_has_room(struct sgxws_ctx *c, uint32_t vertex_bytes)
{
   return ring_room(&c->gen) > vertex_bytes + 0x200 &&
          ring_room(&c->pds_pix) > OBJ_PDS &&
          ring_room(&c->pds_vtx) > OBJ_PDS &&
          ring_room(&c->vdm) > OBJ_VDM + 0x40;
}

/* make room for one more object, submitting the frame so far if need be (the
 * next render starts from its pixels, as every render does, and its depth) */
static int
make_room(struct sgxws_ctx *c, uint32_t vertex_bytes)
{
   struct sgxws_rt *rt = c->rt;
   struct sgxws_target t = c->target;
   int e;

   if (frame_has_room(c, vertex_bytes))
      return 0;
   c->room_flushes++;
   if ((e = sgxws_flush(c, 0)))
      return e;
   t.zls_load = t.zls_va != 0; /* and its depth and stencil */
   if ((e = sgxws_begin(c, rt, &t)))
      return e;
   return frame_has_room(c, vertex_bytes) ? 0 : -E2BIG;
}

/* A depth-biased draw: the frame takes its units and factor, or, when it
 * has others, is submitted and the draw starts the next */
static int
depth_bias(struct sgxws_ctx *c, const struct sgxws_state *st)
{
   struct sgxws_rt *rt = c->rt;
   struct sgxws_target t = c->target;
   int e;

   if (!st->depth_bias)
      return 0;
   if (c->bias_set &&
       (c->bias_units != st->bias_units || c->bias_factor != st->bias_factor)) {
      if ((e = sgxws_flush(c, 0)))
         return e;
      t.zls_load = t.zls_va != 0;
      if ((e = sgxws_begin(c, rt, &t)))
         return e;
   }
   c->bias_set = true;
   c->bias_units = st->bias_units;
   c->bias_factor = st->bias_factor;
   return 0;
}

/* TA state region clip, in tiles, inclusive */
static void
region_clip(uint32_t *w, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1)
{
   sgx_pack(&w[0], TA_STATE_REGION_CLIP0, r0)
   {
      r0.x_min = x0;
      r0.x_max = x1;
   }
   sgx_pack(&w[1], TA_STATE_REGION_CLIP1, r1)
   {
      r1.y_min = y0;
      r1.y_max = y1;
   }
}

/* A pass-type-3 object: writes the visible bits under it and draws nothing
 * (ISPC b25-27: 1 sets, 0 resets), with its region clip */
static void
vpgen_object(struct sgxws_ctx *c, bool set, const struct sgxws_rect *tiles,
             const float *xy, uint32_t count, uint32_t prim)
{
   struct sgxws_ptr v = ring_alloc(&c->gen, count * 8, DMA_ALIGN);
   uint32_t blk[9];

   memcpy(v.p, xy, count * 8);
   sgx_pack(&blk[0], TA_STATE_HEADER, h)
   { /* d485 */
      h.ispa = h.ispc = h.region_clip = h.outselect = h.mte = h.tsp0 = h.tsp1 =
         true;
   }
   sgx_pack(&blk[1], TA_STATE_ISPA, a)
   { /* 07e00100 */
      a.pass_type = SGX_TA_PASSTYPE_VIEWPORT_GENERATE;
      a.no_color_write = true;
      a.depth_func = SGX_TA_CMP_ALWAYS;
      a.ispc_present = true;
   }
   sgx_pack(&blk[2], TA_STATE_ISPC, ispc) ispc.func = set;
   region_clip(&blk[3], tiles->x0 / 32, (tiles->x1 - 1) / 32, tiles->y0 / 16,
               (tiles->y1 - 1) / 16);
   sgx_pack(&blk[5], TA_STATE_OUTSELECT, os) /* 4 dwords per vertex */
      os.out_dwords = 4;
   sgx_pack(&blk[6], TA_STATE_MTE, mte) /* as the clear's */
      mte.unknown_2_31 = SGX_TA_STATE_MTE_UNKNOWN_2_31_CLEAR;
   sgx_pack(&blk[7], TA_STATE_TSP0, tsp0)
   {
   }
   sgx_pack(&blk[8], TA_STATE_TSP1, tsp1)
   {
   }
   state_block(c, blk, 9, 9, 0);
   vdm_draw(c, prim, count, c->idx, v.va, 8, 2, c->vpgen_vs, 2, 4, 0,
            vertices_per_task(c->dev->core, 4));
}

/* the pixels a draw or clear may change: the clip it draws within */
static void
add_damage(struct sgxws_ctx *c)
{
   struct sgxws_rect *d = &c->damage;

   if (d->x1 <= d->x0) {
      *d = c->clip;
      return;
   }
   d->x0 = MIN2(d->x0, c->clip.x0);
   d->y0 = MIN2(d->y0, c->clip.y0);
   d->x1 = MAX2(d->x1, c->clip.x1);
   d->y1 = MAX2(d->y1, c->clip.y1);
}

/* Make r the visible pixels (STATE.md "Scissor and partial viewports"): reset
 * the bits over the whole target within the old rectangle's tiles, and set
 * them over r, which leaves r's tiles as the region clip the draws inherit.
 * Going back to the whole target is the same with r the whole target. */
static void
apply_clip(struct sgxws_ctx *c, const struct sgxws_rect *r)
{
   const struct sgxws_rt *s = c->rt;
   float tri[6] = {0, 0, 2.0f * s->w, 0, 0, 2.0f * s->h};
   float strip[8] = {(float)r->x0, (float)r->y0, (float)r->x1, (float)r->y0,
                     (float)r->x0, (float)r->y1, (float)r->x1, (float)r->y1};

   if (!memcmp(r, &c->clip, sizeof(*r)))
      return;
   vpgen_object(c, false, &c->clip, tri, 3, SGXWS_TRIANGLES);
   vpgen_object(c, true, r, strip, 4, SGXWS_TRIANGLE_STRIP);
   c->clip = *r;
}

/* A clear: one triangle over the whole surface with the iterated colour.
 * wmask: the colour bytes written (B b0, G b1, R b2, A b3; 0xf all, 0 none:
 * GL's glColorMask, which gallium hands the clear).  stencil: the value to
 * clear to, -1 to leave it (the clear object gets ISPC 0e0200ff: always,
 * zpass = replace, the value as its reference) */
int
sgxws_clear(struct sgxws_ctx *c, const float rgba[4], float depth,
            bool write_depth, unsigned wmask, int stencil)
{
   const struct sgxws_rt *s = c->rt;
   struct sgxws_ptr v;
   uint32_t argb =
      u8(rgba[3]) << 24 | u8(rgba[0]) << 16 | u8(rgba[1]) << 8 | u8(rgba[2]);
   float xy[3][2] = {{0, 0}, {2.0f * s->w, 0}, {0, 2.0f * s->h}};
   struct sgxws_rect all = {0, 0, s->w, s->h};
   uint32_t blk[13], n = 0;
   int e;

   /* a render that loads its depth (ZLS) doesn't start at 1.0 */
   bool zwrite =
      write_depth && (depth != 1.0f || c->draws || c->target.zls_load);

   /* nothing to write: no colour, and the depth a fresh render starts with */
   if (!(wmask & 0xf) && !zwrite && stencil < 0)
      return 0;
   if ((e = make_room(c, 3 * 16)))
      return e;
   apply_clip(c, &all);
   v = ring_alloc(&c->gen, 3 * 16, DMA_ALIGN);
   for (int i = 0; i < 3; i++) {
      v.p[4 * i + 0] = f32(xy[i][0]);
      v.p[4 * i + 1] = f32(xy[i][1]);
      v.p[4 * i + 2] = f32(depth);
      v.p[4 * i + 3] = argb;
   }
   /* TA state block: ISPA ISPB PDS region-clip output-select MTE TSP (header
    * 0xd4c3) */
   sgx_pack(&blk[n++], TA_STATE_HEADER, h)
   {
      h.ispa = h.ispb = h.pds = h.region_clip = h.outselect = h.mte = h.tsp0 =
         h.tsp1 = true;
      h.ispc = stencil >= 0;
   }
   /* always; depth write for a depth clear, except to the background depth
    * (1.0) before anything was drawn in a render that doesn't load depth */
   sgx_pack(&blk[n++], TA_STATE_ISPA, a)
   {
      a.depth_func = SGX_TA_CMP_ALWAYS;
      a.depth_write_disable = !zwrite;
      a.no_color_write = !(wmask & 0xf);
      a.ispb_present = true;
      if (stencil >= 0) {
         a.ispc_present = true;
         a.stencil_ref = stencil & 0xff;
      }
   }
   /* ISPB: colour mask (R8 G4 B2 A1, from the byte mask) */
   sgx_pack(&blk[n++], TA_STATE_ISPB, b) b.color_mask =
      (wmask & 4 ? 8 : 0) | (wmask & 2 ? 4 : 0) | (wmask & 1 ? 2 : 0) |
      (wmask & 8 ? 1 : 0);
   if (stencil >= 0) {
      sgx_pack(&blk[n++], TA_STATE_ISPC, ispc)
      { /* 0e0200ff */
         ispc.func = SGX_TA_CMP_ALWAYS;
         ispc.zpass = SGX_TA_STENCILOP_REPLACE;
         ispc.write_mask = 0xff;
      }
   }
   blk[n++] = pds_word(c, c->pds_empty_p, 1);
   blk[n++] = pds_sizeinfo(1, 0, 0);
   blk[n++] = pds_word(
      c, (wmask & 0xf) == 0xf ? c->clear_pds : clear_pds_masked(c, wmask & 0xf),
      2);
   region_clip(&blk[n], 0, s->n.xt - 1, 0, s->n.yt - 1); /* the whole surface */
   n += 2;
   sgx_pack(&blk[n++], TA_STATE_OUTSELECT, os)
   {
      os.out_dwords = 8; /* [D] */
      os.unknown_11_13 = SGX_TA_STATE_OUTSELECT_UNKNOWN_11_13_CLEAR;
   }
   sgx_pack(&blk[n++], TA_STATE_MTE, mte) mte.unknown_2_31 =
      SGX_TA_STATE_MTE_UNKNOWN_2_31_CLEAR;
   sgx_pack(&blk[n++], TA_STATE_TSP0, tsp0) /* one 4-component set */
      tsp0.set0 = 5;
   sgx_pack(&blk[n++], TA_STATE_TSP1, tsp1)
   {
   }
   state_block(c, blk, n, n, (n - 1) & 3);
   vdm_draw(c, SGXWS_TRIANGLES, 3, c->idx, v.va, 16, 4, c->clear_vs, 4, 8, 0,
            8);
   c->draws++;
   add_damage(c);
   return 0;
}

static uint32_t
ispa(const struct sgxws_state *st, uint32_t prim, unsigned face)
{
   uint32_t w;

   sgx_pack(&w, TA_STATE_ISPA, a)
   {
      a.ispb_present = true;
      a.pass_type = st->pass_type & 7;
      /* a line list as a triangle object draws nothing */
      if (prim == SGXWS_LINES)
         a.objtype = SGX_TA_OBJTYPE_LINE;
      else if (prim == SGXWS_POINTS)
         a.objtype = st->sprite ? SGX_TA_OBJTYPE_SPRITE : SGX_TA_OBJTYPE_POINT;
      if (st->stencil) {
         a.ispc_present = true;
         a.stencil_ref = st->stencil_ref[face];
         a.two_sided = st->two_sided;
      }
      /* depth off: always, no write */
      a.depth_func =
         st->depth_func ? st->depth_func - 0x200 : SGX_TA_CMP_ALWAYS;
      a.depth_write_disable = !st->depth_func || !st->depth_write;
   }
   return w;
}

/* MTE cull (STATE.md section 3): with gallium's front_ccw (st flips it for a
 * y-down window), 1 culls back faces of a CCW-front draw, 2 front faces
 * [D: cull back, front, and back with a CW front] */
static uint32_t
mte_ctrl(const struct sgxws_state *st)
{

   uint32_t w;

   sgx_pack(&w, TA_STATE_MTE, mte)
   {
      if (st->cull == SGXWS_BACK)
         mte.cull = st->front_ccw ? SGX_TA_CULL_CW : SGX_TA_CULL_CCW;
      else if (st->cull == SGXWS_FRONT)
         mte.cull = st->front_ccw ? SGX_TA_CULL_CCW : SGX_TA_CULL_CW;
      mte.unknown_2_31 = SGX_TA_STATE_MTE_UNKNOWN_2_31_DRAW;
   }
   return w;
}

static int kick_ta(struct sgxws_ctx *c);

int
sgxws_draw(struct sgxws_ctx *c, const struct sgxws_draw *d)
{
   const struct sgxws_state *st = d->state;
   const struct sgxws_prog *vs = d->vs, *fs = d->fs;
   uint32_t blk[23], n = 0, rows, sec, prim, vrows, ispb;
   uint32_t vw = vs->attr_dwords * 4, vsec_dwords = 10 + vs->const_dwords;
   uint32_t idx = c->idx, n_idx = d->count, idx_bytes = 0;
   uint32_t mem_bytes = ((d->vs_mem.dwords * 4 + 15) & ~15u) +
                        ((d->fs_mem.dwords * 4 + 15) & ~15u);
   struct sgxws_ptr v = {0}, u, vsec, vmem = {0}, fmem = {0};
   struct sgxws_fetch fetch[SGXWS_MAX_FETCH];
   int e;

   if ((e = depth_bias(c, st)))
      return e;
   if (d->n_fetch) {
      uint32_t block_bytes = 0, block_va[8];
      unsigned nd = 0;

      if (d->n_fetch > SGXWS_MAX_FETCH || d->n_blocks > 8)
         return -E2BIG;
      for (unsigned i = 0; i < d->n_fetch; i++)
         nd += 1 + (d->fetch[i].defaults && d->fetch[i].dwords < 4);
      if (nd > SGXWS_MAX_FETCH)
         return -E2BIG;
      for (unsigned b = 0; b < d->n_blocks; b++)
         block_bytes += (d->blocks[b].bytes + DMA_ALIGN - 1) & ~(DMA_ALIGN - 1);
      if (d->indices) {
         if (d->n_indices > SGXWS_MAX_INDICES)
            return -E2BIG;
         n_idx = d->n_indices;
         idx_bytes = (n_idx * 2 + 15) & ~15u;
      } else if (d->idx_va) {
         /* an index buffer in device memory, as it is */
         if (d->n_indices > SGXWS_MAX_INDICES)
            return -E2BIG;
         n_idx = d->n_indices;
         idx = d->idx_va;
      } else if (d->count > c->idx_count) {
         return -E2BIG;
      }
      if ((e = make_room(c, block_bytes + vsec_dwords * 4 + idx_bytes +
                               mem_bytes + DMA_ALIGN)))
         return e;
      for (unsigned b = 0; b < d->n_blocks; b++) {
         struct sgxws_ptr bp = ring_alloc(
            &c->gen, d->blocks[b].bytes ? d->blocks[b].bytes : 4, DMA_ALIGN);

         memcpy(bp.p, d->blocks[b].cpu, d->blocks[b].bytes);
         block_va[b] = bp.va;
      }
      for (unsigned i = 0; i < d->n_fetch; i++) {
         fetch[i] = d->fetch[i];
         if (fetch[i].block) {
            if (fetch[i].block > d->n_blocks)
               return -EINVAL;
            fetch[i].va += block_va[fetch[i].block - 1];
         }
      }
      if (d->indices) {
         struct sgxws_ptr ip = ring_alloc(&c->gen, idx_bytes, 16);

         memcpy(ip.p, d->indices, n_idx * 2);
         idx = ip.va;
      }
   } else if (d->verts_va) {
      if (d->idx_va) {
         if (d->n_indices > SGXWS_MAX_INDICES)
            return -E2BIG;
         n_idx = d->n_indices;
         idx = d->idx_va;
      } else if (d->count > c->idx_count) {
         return -E2BIG;
      }
      if ((e = make_room(c, vsec_dwords * 4 + mem_bytes + DMA_ALIGN)))
         return e;
      v.va = d->verts_va;
      v.p = NULL;
   } else {
      if (d->indices) {
         if (d->n_indices > SGXWS_MAX_INDICES)
            return -E2BIG;
         n_idx = d->n_indices;
         idx_bytes = (n_idx * 2 + 15) & ~15u;
      } else if (d->count > c->idx_count) {
         return -E2BIG;
      }
      if ((e = make_room(c, d->count * vw + vsec_dwords * 4 + idx_bytes +
                               mem_bytes + DMA_ALIGN)))
         return e;
      v = ring_alloc(&c->gen, d->count * vw, DMA_ALIGN);
      memcpy(v.p, d->verts, d->count * vw);
      if (d->indices) {
         struct sgxws_ptr ip = ring_alloc(&c->gen, idx_bytes, 16);

         memcpy(ip.p, d->indices, n_idx * 2);
         idx = ip.va;
      }
   }

   /* the constants in memory, in this frame's general ring: a block no
    * earlier draw of the frame used */
   if (d->vs_mem.dwords) {
      vmem = ring_alloc(&c->gen, d->vs_mem.dwords * 4, 16);
      memcpy(vmem.p, d->vs_mem.words, d->vs_mem.dwords * 4);
   }
   if (d->fs_mem.dwords) {
      fmem = ring_alloc(&c->gen, d->fs_mem.dwords * 4, 16);
      memcpy(fmem.p, d->fs_mem.words, d->fs_mem.dwords * 4);
   }

   /* fragment: the constants and the secondary program that DMAs them, then
    * the primary program that runs the shader */
   if (fs->sec_dwords) {
      /* the frame's last block again when the constants are (a memory
       * block's address is new each draw) */
      if (!d->fs_mem.dwords && c->last_u_va && !c->dev->state_always &&
          c->last_u_dwords == fs->sec_dwords &&
          !memcmp(c->last_u, d->fs_consts, fs->sec_dwords * 4)) {
         u.va = c->last_u_va;
      } else {
         u = ring_alloc(&c->pds_pix, fs->sec_dwords * 4, DMA_ALIGN);
         memcpy(u.p, d->fs_consts, fs->sec_dwords * 4);
         for (unsigned j = 0; j < d->fs_mem.n_bases && d->fs_mem.dwords; j++)
            u.p[d->fs_mem.base[j]] = fmem.va + 512 * j;
         c->last_u_va = 0;
         if (!d->fs_mem.dwords) {
            if (c->last_u_cap < fs->sec_dwords) {
               uint32_t *m = realloc(c->last_u, fs->sec_dwords * 4);

               if (m) {
                  c->last_u = m;
                  c->last_u_cap = fs->sec_dwords;
               }
            }
            if (c->last_u_cap >= fs->sec_dwords) {
               memcpy(c->last_u, d->fs_consts, fs->sec_dwords * 4);
               c->last_u_dwords = fs->sec_dwords;
               c->last_u_va = u.va;
            }
         }
      }
      /* a task that waits for the DMA (wdf) before it ends: the empty task
       * here locks the GPU up */
      sec = pds_dma_task(
         c, &c->pds_pix, u.va, fs->sec_dwords,
         doutu_w0(c, fs->sec ? fs->sec : c->sec_wait, false, fs->sec_temps), 4,
         &rows);
   } else {
      sec = c->pds_empty_p;
      rows = 1;
   }
   apply_clip(c, &st->clip);
   /* ISPB: line width * 16 (1.0 when the draw doesn't say), colour mask,
    * depth bias */
   sgx_pack(&ispb, TA_STATE_ISPB, b)
   {
      b.depth_bias = st->depth_bias;
      b.line_width = st->line_width > 15.9f ? 255u
                     : st->line_width > 1.0f
                        ? (uint32_t)(st->line_width * 16.0f)
                        : 16u;
      b.color_mask = st->color_mask & 0xf;
   }
   n++; /* the header, below */
   blk[n++] = ispa(st, d->prim, 0);
   blk[n++] = ispb;
   if (st->stencil) {
      blk[n++] = st->ispc[0];
      if (st->two_sided) {
         blk[n++] = ispa(st, d->prim, 1);
         blk[n++] = ispb;
         blk[n++] = st->ispc[1];
      }
   }
   blk[n++] = pds_word(c, sec, rows);
   /* PDS middle word: the fragment program's primary attributes in units
    * of 4 dwords, the secondaries [D]; b25-28 = 12 divided by the units
    * [D: 0-5 vec4 varyings give units 1 1 1 2 2 3, b25-28 12 12 6 6 4; 0xb
    * with one unit works too]. A program with more attributes than the word
    * declares locks the GPU up (two F32 vec4 varyings). */
   {
      unsigned units = fs->pa_dwords > 4 ? (fs->pa_dwords + 3) / 4 : 1,
               per = 12 / units;

      blk[n++] = fs->pds_middle ? fs->pds_middle
                                : pds_sizeinfo(units, fs->sec_dwords,
                                               per < 0xb ? per : 0xbu);
   }
   /* DOUTU word 1: the program has plain iterations, samples (PDS.md) */
   {
      bool iter = false, smp = false;

      for (unsigned i = 0; i < fs->n_douti; i++) {
         iter |= fs->douti[i].iterate;
         smp |= fs->douti_tex[i] != 0;
      }
      prim = pds_pixel(c, doutu_w0(c, fs->code, false, fs->temps),
                       doutu_w1_pixel(iter, smp), fs->douti, fs->douti_tex,
                       fs->douti_t3_clr, fs->douti_t3_set, fs->n_douti, d->tex,
                       &rows);
   }
   blk[n++] = pds_word(c, prim, rows);
   /* viewport: gallium's transform [C x offset, D rest] */
   sgx_pack(&blk[n], TA_STATE_VIEWPORT, vp)
   {
      vp.x_offset = st->vp_translate[0];
      vp.x_scale = st->vp_scale[0];
      vp.y_offset = st->vp_translate[1];
      vp.y_scale = st->vp_scale[1];
      vp.z_offset = st->vp_translate[2];
      vp.z_scale = st->vp_scale[2];
   }
   n += SGX_TA_STATE_VIEWPORT_length;
   blk[n++] = 0; /* header 0x200 */
   sgx_pack(&blk[n++], TA_STATE_OUTSELECT, os)
   {
      os.out_dwords = vs->out_dwords;
      os.unknown_11_13 = vs->colour
                            ? SGX_TA_STATE_OUTSELECT_UNKNOWN_11_13_DRAW_COLOUR
                            : SGX_TA_STATE_OUTSELECT_UNKNOWN_11_13_DRAW;
      os.point_size = vs->point_size;
      os.clip_planes = vs->clip_planes;
   }
   sgx_pack(&blk[n++], TA_STATE_WCLAMP, wc) wc.value = 1e-5f;
   blk[n++] = mte_ctrl(st);
   blk[n++] = vs->tsp_sizes; /* TSP0, TSP1: the vertex program's */
   blk[n++] = vs->tsp_prec | d->tsp_f16;
   sgx_pack(&blk[0], TA_STATE_HEADER, h)
   {
      h.ispa = h.ispb = h.pds = h.viewport = h.unknown_word = h.outselect =
         h.wclamp = h.mte = true;
      h.tsp0 = h.tsp1 = true;
      h.ispc = st->stencil;
      h.ispa2 = h.ispb2 = h.ispc2 = st->stencil && st->two_sided;
   }
   /* a draw's block is DMAed as 25 dwords (kind 0), the 22-word two-sided one
    * as 27 (kind 2) [D]; the header says where it ends. 25 for the two-sided
    * block draws nothing or the wrong colours from run to run (dEQP
    * fragment_ops.random; 27 and 28 draw it, 24 and 25 don't) */
   {
      uint32_t dma = n + 5 > 25 ? n + 5 : 25;

      /* the MTE has it from the last draw: no new state (a block per draw
       * costs the TA in every tile the draw touches). SGX_STATE_ALWAYS=1
       * sends it anyway */
      if (c->dev->state_always || c->last_blk_n != n ||
          memcmp(c->last_blk, blk, n * 4)) {
         state_block(c, blk, n, dma, (dma - 1) & 3);
         c->state_sent++;
      } else {
         c->state_skipped++;
      }
   }

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
   for (unsigned j = 0; j < d->vs_mem.n_bases && d->vs_mem.dwords; j++)
      vsec.p[10 + d->vs_mem.base[j]] = vmem.va + 512 * j;
   sec = pds_dma_task(c, &c->pds_vtx, vsec.va, vsec_dwords,
                      doutu_w0(c, c->empty_v, true, 0), 0, &vrows);
   vdm_state(c, SGX_VDMCTRL_BLOCK_TYPE_PDS_STATE, sec, vrows,
             SGX_VDMCTRL_STATE_KIND_DMA, vsec_dwords, 0);

   c->gpu_indices += n_idx;
   c->gpu_verts += d->count;
   if (d->n_fetch)
      vdm_fetch(c, d->prim, n_idx, idx, fetch, d->n_fetch, vs->code,
                vs->in_dwords, vs->out_dwords, vs->temps,
                vertices_per_task(c->dev->core, vs->out_dwords));
   else
      vdm_draw(c, d->prim, n_idx, idx, v.va, vw, vs->attr_dwords, vs->code,
               vs->in_dwords, vs->out_dwords, vs->temps,
               vertices_per_task(c->dev->core, vs->out_dwords));
   c->draws++;
   add_damage(c);
   /* SGX_TA_SPLIT: the GPU tiles what is recorded while the rest is */
   if (c->dev->ta_split && ++c->seg_draws >= c->dev->ta_split)
      return kick_ta(c);
   return 0;
}

/* ------------------------------------------------------------------ submission
 */

/* the end of the stream: an empty secondary update, the TA terminate, VDM
 * terminate */
void
sgxws_vdm_end(struct sgxws_ctx *c)
{
   vdm_state(c, SGX_VDMCTRL_BLOCK_TYPE_PDS_STATE, c->empty_state, 1,
             SGX_VDMCTRL_STATE_KIND_DMA, 0, 0);
   /* the TA terminate's 2 attributes */
   vdm_state(c, SGX_VDMCTRL_BLOCK_TYPE_PDS_STATE_TERMINATE, c->terminate, 3,
             SGX_VDMCTRL_STATE_KIND_TA_BLOCK, 2, 0);
   sgx_pack(vdm_slot(c), VDMCTRL_STREAM_TERMINATE0, t)
   {
   }
}

void
sgxws_use_bo(struct sgxws_ctx *c, const struct sgxws_bo *bo, bool write)
{
   for (unsigned i = 0; i < c->n_use; i++) {
      if (c->use[i].bo == bo) {
         c->use[i].write |= write;
         return;
      }
   }
   /* (the caller made room: sgxws_use_bo_room) */
   assert(c->n_use < ARRAY_SIZE(c->use));
   if (c->n_use < ARRAY_SIZE(c->use))
      c->use[c->n_use++] = (__typeof__(c->use[0])){bo, write};
}

bool
sgxws_use_bo_room(const struct sgxws_ctx *c, const struct sgxws_bo *const *bos,
                  unsigned n)
{
   unsigned more = 0;

   for (unsigned k = 0; k < n; k++) {
      bool listed = false;

      for (unsigned i = 0; i < c->n_use && !listed; i++)
         listed = c->use[i].bo == bos[k];
      for (unsigned j = 0; j < k && !listed; j++)
         listed = bos[j] == bos[k];
      more += !listed;
   }
   return c->n_use + more <= ARRAY_SIZE(c->use);
}

uint64_t
sgxws_now_ns(void)
{
   struct timespec ts;

   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* wait for the kick that last used ring slot i */
static int
slot_wait(struct sgxws_ctx *c, unsigned i, unsigned ms)
{
   uint32_t seq = c->slot_kick[i].seq;

   if (!sgxws_seq_after(seq, c->done_seq))
      return 0;
   if (c->dev->kif->wait_seq(c, seq, ms))
      return -ETIMEDOUT;
   if (c->slot_kick[i].kicked_ns) {
      uint64_t ns = sgxws_now_ns() - c->slot_kick[i].kicked_ns;

      c->ns_latency += ns;
      c->n_latency++;
      if (c->slot_kick[i].scanout) {
         c->ns_latency_scanout += ns;
         c->n_latency_scanout++;
      }
   }
   /* renders finish in kick order */
   c->done_seq = seq;
   return 0;
}

uint32_t
sgxws_done_seq(struct sgxws_ctx *c)
{
   /* oldest first: each one done moves done_seq on */
   for (unsigned n = 1; n <= c->slots; n++) {
      unsigned i = (c->slot + n) % c->slots;

      if (slot_wait(c, i, 0))
         break;
   }
   return c->done_seq;
}

static int sgxws_wait_seq_(struct sgxws_ctx *c, uint32_t seq,
                           unsigned timeout_ms);

int
sgxws_wait_seq(struct sgxws_ctx *c, uint32_t seq, unsigned timeout_ms)
{
   uint64_t t0 = sgxws_now_ns();
   int e = sgxws_wait_seq_(c, seq, timeout_ms);

   c->ns_fence_wait += sgxws_now_ns() - t0;
   return e;
}

static int
sgxws_wait_seq_(struct sgxws_ctx *c, uint32_t seq, unsigned timeout_ms)
{
   /* the kicks still in the slots, up to seq; the older ones were waited
    * for when their slot was used again */
   for (unsigned n = 1; n <= c->slots; n++) {
      unsigned i = (c->slot + n) % c->slots;

      if (!sgxws_seq_after(c->slot_kick[i].seq, seq) &&
          slot_wait(c, i, timeout_ms))
         return -ETIMEDOUT;
   }
   return 0;
}

int
sgxws_wait_idle(struct sgxws_ctx *c, unsigned timeout_ms)
{
   return sgxws_wait_seq_(c, c->kicks, timeout_ms);
}

/* code written since the last kick, to the GPU */
static int
ring_sync(struct sgxws_ctx *c, struct sgxws_ring *r)
{
   uint32_t end = (r->used + 7) & ~7u;
   int e;

   if (end <= r->uploaded)
      return 0;
   if ((e = c->dev->kif->code_upload(c->dev, &r->bo, r->uploaded,
                                     end - r->uploaded)))
      return e;
   r->uploaded = end;
   return 0;
}

static int
code_sync(struct sgxws_ctx *c)
{
   int e;

   if (!c->dev->kif->code_upload)
      return 0;
   if ((e = ring_sync(c, &c->eot_pool[0])) ||
       (e = ring_sync(c, &c->eot_pool[1])))
      return e;
   for (unsigned v = 0; v < 2; v++) {
      for (unsigned i = 0; i < c->usse[v].n; i++) {
         if ((e = ring_sync(c, &c->usse[v].chunk[i]->r)))
            return e;
      }
   }
   return 0;
}

/*
 * Submitting a frame doesn't wait for its render: the next frame records into
 * the next ring slot meanwhile, so the CPU's next frame overlaps the GPU's
 * last.  What the CPU must not touch while a render runs:
 *  - the ring slot it was recorded in, until it is done (the slot is used
 *    again c->slots submissions later);
 *  - under services, its render target's structures (render details, region
 *    headers, sync lists): the next frame into the same target is kicked
 *    once the one before is done, so a target has one render in flight.
 * Different targets (the back buffer, then the swap's copy to fb0) are in
 * flight together; the microkernel runs them in order.
 */
static int sgxws_flush_(struct sgxws_ctx *c, unsigned timeout_ms);

/* under services, the target's last render is done before its next scene's
 * first kick (the client writes its render details) */
static void
target_wait(struct sgxws_ctx *c)
{
   struct sgxws_rt *s = c->rt;
   uint64_t t0 = sgxws_now_ns();

   if (!c->dev->kif->rt_one_in_flight || c->ta_kicks)
      return;
   if (sgxws_seq_after(s->seq, c->done_seq) &&
       c->slot_kick[s->seq_slot].seq == s->seq &&
       slot_wait(c, s->seq_slot, 2000))
      mesa_loge("sgx: the target's last render did not finish");
   c->ns_target_wait += sgxws_now_ns() - t0;
}

/* a TA kick of what is recorded, the scene going on: the next objects start
 * a new VDM segment after this one's terminate */
static int
kick_ta(struct sgxws_ctx *c)
{
   const struct sgxws_kif *kif = c->dev->kif;
   int e;

   c->seg_draws = 0;
   /* the frame's kicks that fit in the backend's command ring with c->slots
    * frames in flight (the last is the frame's own kick) */
   if (kif->ring_kicks && c->ta_kicks + 2 > kif->ring_kicks / c->slots)
      return 0;
   target_wait(c);
   sgxws_vdm_end(c);
   if ((e = code_sync(c)) || (e = kif->kick_ta(c)))
      return e;
   c->ta_kicks++;
   c->last_blk_n = 0; /* the next kick's MTE starts without state */
   c->vdm.used = (c->vdm.used + 3) & ~3u;
   c->vdm_start = c->vdm.bo.va + c->vdm.used;
   return 0;
}

int
sgxws_flush(struct sgxws_ctx *c, unsigned timeout_ms)
{
   uint64_t t0 = sgxws_now_ns();
   int e = sgxws_flush_(c, timeout_ms);

   c->ns_flush += sgxws_now_ns() - t0;
   return e;
}

static int
sgxws_flush_(struct sgxws_ctx *c, unsigned timeout_ms)
{
   const struct sgxws_kif *kif = c->dev->kif;
   struct sgxws_rt *s = c->rt;
   uint32_t seq = c->kicks + 1;
   int e;

   if (!s)
      return 0;

   sgxws_vdm_end(c);
   target_wait(c);
   if ((e = code_sync(c)))
      mesa_loge("sgx: shader upload refused: %d", e);
   else
      e = kif->kick(c, seq);
   c->kicks = seq;
   c->slot_kick[c->slot].seq = seq;
   c->slot_kick[c->slot].kicked_ns = sgxws_now_ns();
   c->slot_kick[c->slot].scanout = c->target.scanout;
   s->seq = seq;
   s->seq_slot = c->slot;
   if (!e && timeout_ms) {
      uint64_t t0 = sgxws_now_ns();

      e = slot_wait(c, s->seq_slot, timeout_ms);
      c->ns_sync_wait += sgxws_now_ns() - t0;
   }
   if (!e && c->dev->frame_gap_us) {
      struct timespec ts = {0, (long)c->dev->frame_gap_us * 1000};

      nanosleep(&ts, NULL);
   }
   if (c->dev->verbose || e) {
      char info[96] = "";

      if (kif->frame_info)
         kif->frame_info(c, info, sizeof(info));
      mesa_logi(
         "sgx: frame %u (target %#x %ux%u frame %u, kick %u, slot %u, %.3f s): "
         "%u objects, %s, rc %d",
         c->frames, (unsigned)c->target.va, s->w, s->h, s->frames, seq, c->slot,
         sgxws_now_ns() / 1e9, c->draws, info, e);
   }

   s->frames++;
   c->frames++;
   c->rt = NULL;
   /* the next frame records into the next slot, once the render there is done */
   c->slot = (c->slot + 1) % c->slots;
   {
      uint64_t t0 = sgxws_now_ns(), dt;
      uint32_t waited = c->slot_kick[c->slot].seq, done = c->done_seq;

      if (slot_wait(c, c->slot, 2000) && !e)
         e = -ETIMEDOUT;
      dt = sgxws_now_ns() - t0;
      c->ns_slot_wait += dt;
      if (c->dev->verbose && dt > 2000000)
         mesa_logi(
            "sgx: slot %u: waited %.1f ms for kick %u (last kick %u, done "
            "%u)",
            c->slot, dt / 1e6, waited, c->kicks, done);
   }
   ring_use_slot(&c->pds_pix, c->slot);
   ring_use_slot(&c->pds_vtx, c->slot);
   ring_use_slot(&c->gen, c->slot);
   ring_use_slot(&c->vdm, c->slot);
   return e;
}

/* ------------------------------------------------------------------ built-in
 * shaders */

void
sgxws_prog_builtin(struct sgxws_ctx *c, enum sgxws_builtin which,
                   struct sgxws_prog *s)
{
   memset(s, 0, sizeof(*s));
   switch (which) {
   case SGXWS_VS_XY:
   case SGXWS_VS_XYZ:
      s->vertex = true;
      s->code = usse_load(
         c, which == SGXWS_VS_XY ? &c->fixed.vs_xy : &c->fixed.vs_xyz, true);
      s->in_dwords = 4;
      s->out_dwords = 4;
      s->attr_dwords = which == SGXWS_VS_XY ? 2 : 3;
      break;
   case SGXWS_FS_UNIFORM:
      s->sec = usse_load(c, &c->fixed.fs_uniform_sec, false);
      s->code = usse_load(c, &c->fixed.fs_uniform, false);
      s->sec_dwords = SGXWS_FS_UNIFORM_DWORDS;
      break;
   }
}

/* ------------------------------------------------------------------ compiled
 * programs */

#define PHAS_HI    0xfa440000u /* phas.i #pc, #temps/4 [wait] (docs/ISA.md) */
#define NOP_END_LO 0x00000000u
#define NOP_END_HI 0xf8040140u /* nop [end] */

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
sgxws_upload_vs(struct sgxws_ctx *c, const uint64_t *code, unsigned count,
                unsigned temps)
{
   /* fixed.vs_xy: [0] phas.i @p1, [1-4] the prologue, [7] phas.i @p2 [wait=2],
    * [9] emit.t3 r0, #0, #0 [end f12 f10], [10-11] the epilogue phase */
   const uint32_t *t = c->fixed.vs_xy.w;
   uint32_t *w = malloc((count + 12) * 8), va;
   uint8_t reloc[2];
   unsigned n = 0, p1, p2;

   if (!w)
      return 0;
   p1 = 6;
   p2 = p1 + 1 + count + 1;
   put(w, &n, p1,
       PHAS_HI |
          ((temps + 3) / 4)); /* phase 0 -> phase 1 with its temporaries */
   for (unsigned i = 1; i <= 4; i++)
      put(w, &n, t[2 * i], t[2 * i + 1]);
   put(w, &n, NOP_END_LO, NOP_END_HI);
   put(w, &n, p2, t[2 * 7 + 1]); /* phase 1 -> the epilogue */
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
sgxws_blend_words(const struct sgxws_blend *b, unsigned color_pa, bool end,
                  uint32_t w[2])
{
   uint32_t hi = 0x80800000u;

   hi |= end ? 1u << 18 | 1 : 2;
   hi |= (b->c1 & 7u) << 6 | (b->c1 & SGXWS_BS_INV ? 1u << 24 : 0);
   hi |= (b->c2 & 7u) << 3 | (b->c2 & SGXWS_BS_INV ? 1u << 15 : 0);
   hi |= alpha_sel(b->a1) << 20 | (b->a1 & SGXWS_BS_INV ? 1u << 11 : 0);
   hi |= alpha_sel(b->a2) << 9 | (b->a2 & SGXWS_BS_INV ? 1u << 2 : 0);
   /* src1 l7-13 (bank l30-31), src2 l0-6 (bank l28-29): pa = 2, o = 1 */
   w[0] = (b->swap ? 0x60000000u | color_pa : 0x90000000u | color_pa << 7) |
          (end ? 0 : color_pa << 21) | (b->cop & 3u) << 18 |
          (b->aop & 3u) << 16;
   w[1] = hi;
}

/* A constant-colour blend (STATE.md "Constant-colour blending"):
 *    sopwm r1, Y, o0      [c1=zero c2=Fd]           dst * Fd, all four bytes
 *    sopwm r1, Y, o0      [mask=8 c1=zero c2=Fda]   the alpha byte: dst.a * Fda
 *    sopwm r0, pa<c>, X   [c1=Fs c2=zero]           src * Fs
 *    sopwm r0, pa<c>, X   [mask=8 c1=Fsa c2=zero]   src.a * Fsa
 *    sop2  o0, r0, r1     [end c1=one c2=one cop a1=one a2=one aop]
 * X and Y are the colour, the tile or the constant, whichever the factor
 * reads.  With end false the sop2 writes pa<c> back for the colour mask; with
 * swap it reads r1, r0 (a reverse subtract).  With atrade the two alpha
 * products go to the other register (r0.a = dst.a * Fda, r1.a = src.a * Fsa),
 * after both whole-colour ones, so alpha subtracts the other way round. */
#define BLEND_CONST_INSTS 5

static void
blend_const_words(const struct sgxws_blend *b, unsigned color_pa, bool end,
                  uint32_t w[BLEND_CONST_INSTS][2])
{
   /* per factor: src1 of the dst side and its selectors; src2 of the src side
    * and its */
   enum { COLOUR, TILE, CONSTANT };
   static const uint8_t reads[15] = {
      COLOUR, COLOUR, COLOUR, COLOUR,   COLOUR,   COLOUR,   TILE,    TILE,
      TILE,   TILE,   TILE,   CONSTANT, CONSTANT, CONSTANT, CONSTANT};
   static const uint32_t d_hi[15] = {0,      0x8000, 0x10,   0x8010, 0x18,
                                     0x8018, 0x38,   0x8038, 0x30,   0x8030,
                                     0x8,    0x10,   0x8010, 0x18,   0x8018};
   static const uint32_t s_hi[15] = {0,         0x1000000, 0x80,     0x1000080,
                                     0xc0,      0x10000c0, 0x1c0,    0x10001c0,
                                     0x180,     0x1000180, 0x40,     0x180,
                                     0x1000180, 0x1c0,     0x10001c0};
   const unsigned f[4] = {b->fd, b->fda, b->fs, b->fsa};

   for (unsigned i = 0; i < 4; i++) {
      unsigned k = f[i] < 15 ? f[i] : 0;
      uint32_t lo, hi;

      if (i < 2) { /* src1 at l7-13, bank l30-31: pa 2, o 1, sa 3 */
         lo = reads[k] == COLOUR ? 0x80000000u | color_pa << 7
              : reads[k] == TILE ? 0x40000000u
                                 : 0xc0000000u | SGXWS_BLEND_CONST_SA << 7;
         lo |= 0x10200000; /* r1, src2 o0 */
         hi = d_hi[k] | (i ? 0x90800800 : 0x90807800);
      } else { /* src2 at l0-6, bank l28-29: pa 2, o 1, sa 3 */
         lo = reads[k] == COLOUR ? 0x20000000u | color_pa
              : reads[k] == TILE ? 0x10000000u
                                 : 0x30000000u | SGXWS_BLEND_CONST_SA;
         lo |= color_pa << 7 | 0x80000000; /* r0, src1 pa<c> */
         hi = s_hi[k] | (i == 3 ? 0x90800800 : 0x90807800);
      }
      if (b->atrade && (i & 1))
         lo ^= 1u << 21; /* r1 <-> r0 */
      w[i][0] = lo;
      w[i][1] = hi;
   }
   if (b->atrade) {
      /* dst*Fd, src*Fs, then the alphas: neither alpha may be overwritten */
      uint32_t t[2] = {w[1][0], w[1][1]};

      w[1][0] = w[2][0], w[1][1] = w[2][1];
      w[2][0] = t[0], w[2][1] = t[1];
   }
   /* cop l18-19, aop l16-17; without end pa<c> (dst bank h1, number l21-27) */
   w[4][0] = (b->swap ? 1u << 7 : 1u) | (b->cop & 3u) << 18 |
             (b->aop & 3u) << 16 | (end ? 0 : color_pa << 21);
   w[4][1] = 0x81808804 | (end ? 1u << 18 | 1 : 2);
}

/* the blend's instructions; keep_going drops the end bit of one that would end
 * the program */
static void
put_blend(uint32_t *w, unsigned *n, const struct sgxws_blend *b,
          unsigned color_pa, bool end, bool keep_going)
{
   if (b->constant) {
      uint32_t k[BLEND_CONST_INSTS][2];

      blend_const_words(b, color_pa, end, k);
      for (unsigned i = 0; i < BLEND_CONST_INSTS; i++)
         put(w, n, k[i][0],
             i == BLEND_CONST_INSTS - 1 && keep_going ? k[i][1] & ~(1u << 18)
                                                      : k[i][1]);
   } else {
      uint32_t k[2];

      sgxws_blend_words(b, color_pa, end, k);
      put(w, n, k[0], keep_going ? k[1] & ~(1u << 18) : k[1]);
   }
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

static uint32_t
upload_fs(struct sgxws_ctx *c, const uint64_t *code, unsigned count,
          const uint64_t *code1, unsigned count1, unsigned color_pa,
          const struct sgxws_blend *blend, int discard_pa, unsigned temps);

uint32_t
sgxws_upload_fs(struct sgxws_ctx *c, const uint64_t *code, unsigned count,
                unsigned color_pa, const struct sgxws_blend *blend,
                int discard_pa, unsigned temps)
{
   return upload_fs(c, code, count, NULL, 0, color_pa, blend, discard_pa,
                    temps);
}

uint32_t
sgxws_upload_fs_split(struct sgxws_ctx *c, const uint64_t *code, unsigned n,
                      const uint64_t *code1, unsigned n1, unsigned color_pa1,
                      const struct sgxws_blend *blend, int discard_pa,
                      unsigned temps)
{
   return upload_fs(c, code, n, code1, n1, color_pa1, blend, discard_pa,
                    temps);
}

static uint32_t
upload_fs(struct sgxws_ctx *c, const uint64_t *code, unsigned count,
          const uint64_t *code1, unsigned count1, unsigned color_pa,
          const struct sgxws_blend *blend, int discard_pa, unsigned temps)
{
   uint32_t *w = malloc((count + count1 + 12) * 8), va, b[2];
   bool masked = blend && blend->wmask != 0xf;
   unsigned n = 0;

   if (!w)
      return 0;
   /* a constant-colour blend works in r0, r1 */
   if (blend && blend->blend && blend->constant && temps < 2)
      temps = 2;
   if (discard_pa >= 0) {
      /*
       * A program that discards runs in two phases (ISA.md "Phases, visibility
       * feedback and discard"):
       * phase 0 = phas to phase 1, pcoeff (hands the ISP the depth plane
       * the PDS iterated into pa<n>..), the shader with the keep predicate in
       * p1, and atst8 [end] in place of the compiler's feedback nop (its last
       * instruction); phase 1, on an even instruction, = the colour out
       * without end, then phas.i #0, #0 [end].
       */
      uint8_t reloc[1] = {0};

      put(w, &n, 0,
          0xfa440100u | ((temps + 3) / 4)); /* phas.i #phase1, #t [wait=1] */
      put(w, &n,
          0xb0000004u | (uint32_t)discard_pa << 14 |
             (uint32_t)(discard_pa + 1) << 7,
          0xf8300004u); /* pcoeff pa<n>, pa<n+1>, sa4 */
      put_code(w, &n, code, count - 1);
      put(w, &n, 0xf0000184u | color_pa << 21 | color_pa << 14,
          0xf9340406u); /* atst8 [end] */
      if (n & 1)
         put(w, &n, 0, 0xf8000140u); /* nop: phase 1 on 16 bytes */
      w[0] = n;                      /* relocated to an absolute pc */
      /* split: the colour's code runs here, only for the pixels that pass */
      if (code1)
         put_code(w, &n, code1, count1);
      if (blend && blend->blend)
         put_blend(w, &n, blend, color_pa, !masked, true); /* without end */
      if (masked) {
         wmask_words(blend->wmask, color_pa, b);
         put(w, &n, b[0], b[1] & ~(1u << 18));
      } else if (!blend || !blend->blend) {
         put(w, &n, 0xa0000000u | color_pa << 7,
             0x28a10001u); /* mov o0, pa<c> [skipinv rcnt] */
      }
      put(w, &n, 0, 0xfa4c0700u); /* phas.i #0, #0 [end wait=7] */
      va = sgxws_usse_upload(c, false, w, n, reloc, 1);
      free(w);
      return va;
   }
   put(w, &n, 0, PHAS_HI | 0x700); /* phas.i #0, #0 [wait=7] */
   put_code(w, &n, code, count);
   if (blend && blend->blend)
      put_blend(w, &n, blend, color_pa, !masked, false);
   if (masked) {
      wmask_words(blend->wmask, color_pa, b);
      put(w, &n, b[0], b[1]);
   } else if (!blend || !blend->blend) {
      /* mov o0, pa<c> [skipinv end mask=1]; without the mask this mov locks
       * the GPU up */
      put(w, &n, 0xa0000000u | color_pa << 7, 0x28851001u);
   }
   va = sgxws_usse_upload(c, false, w, n, NULL, 0);
   free(w);
   return va;
}

uint32_t
sgxws_upload_fs_secondary(struct sgxws_ctx *c, const uint64_t *code,
                          unsigned count)
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

/* The clear's pixel program with a colour mask (wmask: bytes of the packed
 * BGRA8, as sgxws_blend's): the iterated colour through the colour mask's
 * sopwm instead of copy_fs's mov. */
static uint32_t
clear_pds_masked(struct sgxws_ctx *c, unsigned wmask)
{
   if (!c->clear_pds_masked[wmask]) {
      uint32_t w[4], fs;

      w[0] = 0;
      w[1] = PHAS_HI | 0x700; /* phas.i #0, #0 [wait=7] */
      wmask_words(wmask, 0, w + 2);
      fs = sgxws_usse_upload(c, false, w, 2, NULL, 0);
      c->clear_pds_masked[wmask] =
         pds_pixel(c, doutu_w0(c, fs, false, 0), doutu_w1_pixel(true, false),
                   &clear_douti, NULL, NULL, NULL, 1, NULL, NULL);
   }
   return c->clear_pds_masked[wmask];
}

uint32_t
sgxws_next_seq(const struct sgxws_ctx *c)
{
   return c->kicks + 1;
}

unsigned
sgxws_frame_objects(const struct sgxws_ctx *c)
{
   return c->draws;
}

void
sgxws_get_stats(struct sgxws_ctx *c, struct sgxws_stats *st, bool reset)
{
   st->ns_flush = c->ns_flush;
   st->ns_cmd = c->ns_cmd;
   st->ns_kick = c->ns_kick;
   st->ns_target_wait = c->ns_target_wait;
   st->ns_slot_wait = c->ns_slot_wait;
   st->ns_sync_wait = c->ns_sync_wait;
   st->ns_fence_wait = c->ns_fence_wait;
   st->ns_latency = c->ns_latency;
   st->ns_latency_scanout = c->ns_latency_scanout;
   st->n_latency = c->n_latency;
   st->n_latency_scanout = c->n_latency_scanout;
   st->room_flushes = c->room_flushes;
   st->gpu_indices = c->gpu_indices;
   st->gpu_verts = c->gpu_verts;
   st->state_sent = c->state_sent;
   st->state_skipped = c->state_skipped;
   st->bo_allocs = c->dev->bo_allocs;
   st->bo_cached = c->dev->bo_cached;
   st->bo_busy = c->dev->bo_busy;
   if (reset) {
      c->dev->bo_allocs = c->dev->bo_cached = c->dev->bo_busy = 0;
      c->state_sent = c->state_skipped = 0;
      c->ns_flush = c->ns_cmd = c->ns_kick = c->ns_target_wait =
         c->ns_slot_wait = 0;
      c->ns_sync_wait = c->ns_fence_wait = c->ns_latency =
         c->ns_latency_scanout = 0;
      c->n_latency = c->n_latency_scanout = c->room_flushes = 0;
      c->gpu_indices = c->gpu_verts = 0;
   }
}
