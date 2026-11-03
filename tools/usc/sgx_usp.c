/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * The DDK 1.7 USP container (docs/ISA.md "The USP container"; usp.py
 * parses it), built from a compiled struct sgx_shader: header, list A
 * (iterated inputs), list C (constants in SA), the (empty) secondary update
 * program, masks and the block stream. Header values the compiler doesn't
 * decide are the ones libusc writes for the same kind of shader (corpus c1).
 *
 * Only the A/B harness needs this: it lets the DDK's GLES driver run our
 * programs (sgxc, hwcmp). The compiler itself doesn't know the container
 * (docs/COMPILER-UPSTREAM.md §3).
 */
#include <stdlib.h>
#include <string.h>

#include "compiler/shader_enums.h"

#include "sgx_usp.h"

struct buf {
   uint8_t *d;
   size_t size, cap;
   bool oom;
};

static void
put(struct buf *b, const void *p, size_t n)
{
   if (b->size + n > b->cap) {
      size_t cap = b->cap ? 2 * b->cap : 4096;
      while (cap < b->size + n)
         cap *= 2;
      uint8_t *d = realloc(b->d, cap);
      if (!d) {
         b->oom = true;
         return;
      }
      b->d = d;
      b->cap = cap;
   }
   memcpy(b->d + b->size, p, n);
   b->size += n;
}

static void
u16(struct buf *b, unsigned v)
{
   uint16_t x = v;
   put(b, &x, 2);
}

static void
u32(struct buf *b, uint32_t v)
{
   put(b, &v, 4);
}

static void
put16(struct buf *b, unsigned off, unsigned v)
{
   uint16_t x = v;
   memcpy(b->d + off, &x, 2);
}

static void
put32(struct buf *b, unsigned off, uint32_t v)
{
   memcpy(b->d + off, &v, 4);
}

static void
inst(struct buf *b, uint64_t w)
{
   u32(b, (uint32_t)w);
   u32(b, (uint32_t)(w >> 32));
}

/* format control 2 (C10 on), MOE increments 1, bases 0 */
static void
moe_state(struct buf *b)
{
   u16(b, 2);
   for (unsigned i = 0; i < 4; i++)
      u16(b, 0x8001);
   for (unsigned i = 0; i < 4; i++)
      u16(b, 0);
}

/* block 2, at most 512 instructions each */
static void
code(struct buf *b, const uint64_t *w, unsigned n)
{
   while (n) {
      unsigned k = n < 512 ? n : 512;
      u32(b, 2);
      u16(b, k);
      moe_state(b);
      for (unsigned i = 0; i < k; i++)
         u16(b, 0);
      for (unsigned i = 0; i < k; i++)
         inst(b, w[i]);
      w += k;
      n -= k;
   }
}

/* blocks 5 and 6. Non-dependent: the PDS leaves the texel in pa, unpacked in
 * place as RGBA8 (libusc's layout for corpus c1 `tex`, with swizzle xyzw:
 * channel n in byte n). Dependent: the smp template (libusc's encoded smp
 * with the destination zeroed and texture state #0) writes the raw texel
 * into a temporary, unpacked in place the same way. The driver's code may
 * use temporaries from n_temps on. */
static void
sample_blocks(struct buf *b, const struct sgx_shader *s, const struct sgx_sample_info *x)
{
   unsigned rawt = x->dependent ? 1 : 3;     /* 4-valued type: 1 r, 3 pa */
   unsigned raw = x->reg;
   u32(b, 5);
   u16(b, x->dependent ? 0 : 1 | (x->proj ? 2 : 0));
   moe_state(b);
   u32(b, x->id);
   u16(b, 0);                    /* iflags */
   u16(b, x->sampler);
   u16(b, 0x688);                /* xyzw */
   u16(b, 1); u16(b, x->dependent ? raw : s->n_temps);   /* smp dest (2-valued: 1 r) */
   u16(b, rawt); u16(b, raw);    /* usrc */
   u16(b, 1); u16(b, s->n_temps); /* scratch */
   u16(b, 0); u16(b, 0);         /* ilive */
   u16(b, 8);                    /* no forced format */
   if (x->dependent) {
      inst(b, x->smp);
   } else {
      u16(b, x->set + 1);
      u16(b, x->dim);
      u16(b, 3); u16(b, x->coord_pa >= 0 ? (unsigned)x->coord_pa : raw);
   }

   u32(b, 6);
   moe_state(b);
   u32(b, x->id);
   u16(b, 0);                    /* iflags */
   u16(b, 15); u16(b, 15);       /* channels, mask */
   for (unsigned i = 0; i < 4; i++) u16(b, rawt);
   for (unsigned i = 0; i < 4; i++) u16(b, raw);
   for (unsigned i = 0; i < 4; i++) u16(b, 4);       /* u8 */
   for (unsigned i = 0; i < 4; i++) u16(b, i);
   u16(b, rawt); u16(b, raw);    /* source */
   u16(b, 1); u16(b, x->dependent ? raw : s->n_temps); /* smp dest */
   u16(b, 1); u16(b, s->n_temps); /* scratch */
   u16(b, 0);                    /* ilive */
}

int
sgx_write_usp(const struct sgx_shader *s, uint8_t **usp, size_t *size)
{
   struct buf b = { 0 };
   bool frag = s->stage != MESA_SHADER_VERTEX;
   if (s->mem_dwords)
      return -1;        /* constants in memory: no list B writer */
   for (unsigned i = 0; i < 0x84; i += 4)
      u32(&b, 0);

   /* list A: iterated inputs; libusc puts a dummy colour iteration in a
    * shader with none (c1 `add`: 0xc, 0, 0xb, 4, 4, 1) */
   unsigned n_a = 0;
   if (frag) {
      bool real = false;
      for (unsigned i = 0; i < s->n_iters; i++) {
         const struct sgx_iter_info *it = &s->iters[i];
         if (it->sample) {
            /* flags 1 = projected (corpus c3 tx_nd_proj4) */
            u16(&b, it->proj ? 1 : 0); u16(&b, it->sampler); u16(&b, it->set + 1); u16(&b, 0); u16(&b, 0); u16(&b, 1);
         } else {
            u16(&b, 4); u16(&b, 0); u16(&b, it->set + 1); u16(&b, it->comps); u16(&b, it->f16 ? 2 : 1); u16(&b, it->regs);
         }
         real |= !it->pointcoord;
         n_a++;
      }
      /* gl_PointCoord alone counts as nothing and comes first (DDK capture) */
      if (!real) {
         u16(&b, 0xc); u16(&b, 0); u16(&b, 0xb); u16(&b, 4); u16(&b, 4); u16(&b, 1);
         n_a++;
      }
   }
   /* list C: constant k -> sa slot: kind 1 f32, 2 F16 half (shift 0/16),
    * 3 C10 channel (10-byte record: kind, k, 0, slot, shift); ours: 0x10
    * F16 of a C10 value (kind 2's record), 0x11 a literal (kind, value low,
    * value high, slot, shift) */
   for (unsigned i = 0; i < s->n_consts; i++) {
      const struct sgx_const_map *m = &s->consts[i];
      if (m->fmt == 3) {
         u16(&b, 3); u16(&b, m->k); u16(&b, 0); u16(&b, m->slot); u16(&b, m->shift);
      } else if (m->fmt == 0x11) {
         u16(&b, 0x11); u16(&b, m->value & 0xffff); u16(&b, m->value >> 16); u16(&b, m->slot); u16(&b, m->shift);
      } else {
         u16(&b, m->fmt); u16(&b, m->k); u16(&b, m->slot); u16(&b, m->shift);
      }
   }
   /* no secondary update program; vertex: the live input mask */
   unsigned n_inmask = 0;
   if (!frag) {
      for (unsigned i = 0; i < 128; i++)
         if (s->in_mask[i / 32] >> (i % 32) & 1)
            n_inmask = i + 1;
      for (unsigned i = 0; i < (n_inmask + 31) / 32; i++)
         u32(&b, s->in_mask[i]);
   }
   u32(&b, 0xffffffff); u32(&b, 0xffffffff); u32(&b, 0); u32(&b, 0);

   /* blocks: start label 3; discard's feedback nop between labels 4 and 5 */
   u32(&b, 4); u16(&b, 3);
   for (unsigned i = 0; i < s->n_blocks; i++) {
      const struct sgx_block *k = &s->blocks[i];
      switch (k->kind) {
      case SGX_BLOCK_CODE:
         code(&b, s->code + k->at, k->n);
         break;
      case SGX_BLOCK_LABEL:
         u32(&b, 4); u16(&b, k->label);
         break;
      case SGX_BLOCK_BRANCH:
         /* the branch as encoded, offset 0: the finalizer fills it in */
         u32(&b, 3); inst(&b, s->code[k->at] & ~0x3ffffull); u16(&b, k->label);
         break;
      case SGX_BLOCK_FEEDBACK:
         u32(&b, 4); u16(&b, 4);
         code(&b, s->code + k->at, 1);
         u32(&b, 4); u16(&b, 5);
         break;
      case SGX_BLOCK_SAMPLE:
         sample_blocks(&b, s, &s->samples[k->at]);
         break;
      }
   }
   u32(&b, 7);
   if (b.oom) {
      free(b.d);
      return -1;
   }

   /* the header */
   unsigned n_slots = s->sa_slots;
   put32(&b, 0x00, 0x55535020);
   put32(&b, 0x04, 0x23);
   put32(&b, 0x08, b.size - 12);
   put32(&b, 0x0c, 1);
   put32(&b, 0x10, frag ? 0 : 1);
   put32(&b, 0x14, (s->n_samples ? 2 : 0) | (s->discards ? 8 : 0));
   put32(&b, 0x18, (frag ? 0x80040104 : 0xe0040104) | (s->discards ? 0x820 : 0));
   put32(&b, 0x1c, s->discards ? 0x1 : 0x40);
   put32(&b, 0x20, 9);
   put32(&b, 0x24, 0x2793);
   put32(&b, 0x2c, s->n_consts ? 8 : 0);
   put16(&b, 0x34, 0xffff);
   put16(&b, 0x36, 0xffff);
   put16(&b, 0x3c, s->sa_base);
   put16(&b, 0x3e, frag ? 0x77 : 0x1f6);
   put16(&b, 0x40, 0xffff);
   put16(&b, 0x44, s->n_pa);
   put16(&b, 0x46, s->n_temps);
   put16(&b, 0x4e, n_a);
   put16(&b, 0x50, s->n_consts);
   if (frag) {
      put16(&b, 0x66, 3);
      put16(&b, 0x68, s->colour_pa);
      put16(&b, 0x6a, 4);
      put16(&b, 0x6c, 1);
   }
   put16(&b, 0x74, 3);
   put16(&b, 0x76, s->discards ? 4 : 0xffff);
   put16(&b, 0x78, s->discards ? 5 : 0xffff);
   put16(&b, 0x7a, 0xffff);
   put16(&b, 0x7e, n_slots);
   put16(&b, 0x80, n_inmask);
   put16(&b, 0x82, 0x80);
   *usp = b.d;
   *size = b.size;
   return 0;
}
