/*
 * The DDK 1.7 USP container (docs/ISA.md "The USP container"; usp.py
 * parses it): header, list A (iterated inputs), list C (constants in SA),
 * the (empty) secondary update program, masks and the block stream. Header
 * values the compiler doesn't decide are the ones libusc writes for the same
 * kind of shader (corpus c1).
 */
#include <string.h>

#include "nir.h"
#include "util/u_dynarray.h"

#include "sgx_priv.h"

struct buf {
   struct util_dynarray d;
};

static void
u16(struct buf *b, unsigned v)
{
   uint16_t x = v;
   util_dynarray_append_array(&b->d, uint8_t, (uint8_t *)&x, 2);
}

static void
u32(struct buf *b, uint32_t v)
{
   util_dynarray_append_array(&b->d, uint8_t, (uint8_t *)&v, 4);
}

static void
put16(struct buf *b, unsigned off, unsigned v)
{
   uint16_t x = v;
   memcpy((uint8_t *)b->d.data + off, &x, 2);
}

static void
put32(struct buf *b, unsigned off, uint32_t v)
{
   memcpy((uint8_t *)b->d.data + off, &v, 4);
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

struct code {
   uint64_t w[512];
   unsigned n;
};

static void
flush(struct buf *b, struct code *cd)
{
   if (!cd->n)
      return;
   u32(b, 2);
   u16(b, cd->n);
   u16(b, 2);
   for (unsigned i = 0; i < 4; i++)
      u16(b, 0x8001);
   for (unsigned i = 0; i < 4; i++)
      u16(b, 0);
   for (unsigned i = 0; i < cd->n; i++)
      u16(b, 0);
   for (unsigned i = 0; i < cd->n; i++)
      inst(b, cd->w[i]);
   cd->n = 0;
}

#define LBL_FEEDBACK 0x7ffe
#define LBL_PHASE1 0x7fff

/* blocks 5 and 6 for a non-dependent read whose texel the PDS leaves in
 * pa, unpacked in place as RGBA8 (libusc's layout for corpus c1 `tex`, with
 * swizzle xyzw: channel n in byte n) */
static unsigned
phys(struct sgx_ctx *c, const struct sgx_ref *r)
{
   return r->file == SGX_F_V ? sgx_vreg(c, r->n)->phys + r->comp : r->n;
}

/* blocks 5 and 6. Non-dependent: the PDS leaves the texel in pa, unpacked in
 * place as RGBA8 (libusc's layout for corpus c1 `tex`, with swizzle xyzw:
 * channel n in byte n). Dependent: the smp template (libusc's encoded smp
 * with the destination zeroed and texture state #0) writes the raw texel
 * into a temporary, unpacked in place the same way. The driver's code may
 * use temporaries from n_temps on. */
static int
sample_blocks(struct buf *b, struct sgx_ctx *c, struct sgx_sample *s)
{
   unsigned rawt = 3, raw = s->pa;      /* 4-valued type: 1 r, 3 pa */
   uint64_t smp = 0;
   if (s->dependent) {
      struct sgx_ins *i = s->ins;
      raw = phys(c, &i->op[0]);
      rawt = 1;
      struct usse_inst m;
      usse_init(&m, s->lod == 1 ? USSE_SMP_1D_BIAS : USSE_SMP_1D);
      usse_set(&m, USSE_F_SKIPINV, 1);
      usse_set(&m, USSE_F_DIM, s->dim == 3 ? 2 : 1);
      m.nops = s->lod ? 4 : 3;
      m.op[0] = usse_reg(USSE_K_R, 0);
      struct sgx_ref cr = i->op[1];
      m.op[1] = usse_reg(cr.file == SGX_F_PA ? USSE_K_PA : cr.file == SGX_F_U ? USSE_K_SA : USSE_K_R,
                         cr.file == SGX_F_U ? c->sa_base + c->const_slot[cr.n] : phys(c, &cr));
      m.op[2] = usse_reg(USSE_K_IMM, 0);
      if (s->lod) {
         struct sgx_ref lr = i->op[2];
         unsigned k = lr.file == SGX_F_C ? USSE_K_C : lr.file == SGX_F_PA ? USSE_K_PA :
                      lr.file == SGX_F_U ? USSE_K_SA : USSE_K_R;
         unsigned n = lr.file == SGX_F_U ? c->sa_base + c->const_slot[lr.n] : phys(c, &lr);
         m.op[3] = usse_reg(k, n);
         m.op[3].neg = lr.neg;
         m.op[3].abs = lr.abs;
      }
      const char *err;
      if (usse_encode(&m, USSE_FC_DEFAULT, &smp, &err)) {
         sgx_fail(c, "smp template: %s", err);
         return -1;
      }
   }
   u32(b, 5);
   u16(b, s->dependent ? 0 : 1 | (s->proj ? 2 : 0));
   moe_state(b);
   u32(b, s->id);
   u16(b, 0);                    /* iflags */
   u16(b, s->sampler);
   u16(b, 0x688);                /* xyzw */
   u16(b, 1); u16(b, s->dependent ? raw : c->n_temps);   /* smp dest (2-valued: 1 r) */
   u16(b, rawt); u16(b, raw);    /* usrc */
   u16(b, 1); u16(b, c->n_temps); /* scratch */
   u16(b, 0); u16(b, 0);         /* ilive */
   u16(b, 8);                    /* no forced format */
   if (s->dependent) {
      inst(b, smp);
   } else {
      u16(b, s->set + 1);
      u16(b, s->dim);
      u16(b, 3); u16(b, s->coord_pa >= 0 ? s->coord_pa : s->pa);
   }

   u32(b, 6);
   moe_state(b);
   u32(b, s->id);
   u16(b, 0);                    /* iflags */
   u16(b, 15); u16(b, 15);       /* channels, mask */
   for (unsigned i = 0; i < 4; i++) u16(b, rawt);
   for (unsigned i = 0; i < 4; i++) u16(b, raw);
   for (unsigned i = 0; i < 4; i++) u16(b, 4);       /* u8 */
   for (unsigned i = 0; i < 4; i++) u16(b, i);
   u16(b, rawt); u16(b, raw);    /* source */
   u16(b, 1); u16(b, s->dependent ? raw : c->n_temps); /* smp dest */
   u16(b, 1); u16(b, c->n_temps); /* scratch */
   u16(b, 0);                    /* ilive */
   return 0;
}

int
sgx_write_usp(struct sgx_ctx *c, struct sgx_shader *out)
{
   struct buf b;
   bool frag = c->stage == MESA_SHADER_FRAGMENT;
   util_dynarray_init(&b.d, NULL);
   for (unsigned i = 0; i < 0x84; i += 4)
      u32(&b, 0);

   /* list A: iterated inputs; libusc puts a dummy colour iteration in a
    * shader with none (c1 `add`: 0xc, 0, 0xb, 4, 4, 1) */
   unsigned n_a = 0, n_pa = 0;
   if (frag) {
      if (!c->n_iters) {
         u16(&b, 0xc); u16(&b, 0); u16(&b, 0xb); u16(&b, 4); u16(&b, 4); u16(&b, 1);
         n_a = 1;
         n_pa = 1;
      }
      for (unsigned i = 0; i < c->n_iters; i++) {
         struct sgx_iter *it = &c->iters[i];
         if (it->sample) {
            /* flags 1 = projected (corpus c3 tx_nd_proj4) */
            u16(&b, it->proj ? 1 : 0); u16(&b, it->sampler); u16(&b, it->set + 1); u16(&b, 0); u16(&b, 0); u16(&b, 1);
            n_pa = MAX2(n_pa, it->pa + 1);
         } else {
            unsigned regs = it->f16 ? (it->comps + 1) / 2 : it->comps;
            u16(&b, 4); u16(&b, 0); u16(&b, it->set + 1); u16(&b, it->comps); u16(&b, it->f16 ? 2 : 1); u16(&b, regs);
            n_pa = MAX2(n_pa, it->pa + regs);
         }
         n_a++;
      }
      n_pa = MAX2(n_pa, c->colour_pa + 1);
   }
   /* list C: constant k -> sa slot: kind 1 f32, 2 F16 half (shift 0/16),
    * 3 C10 channel (10-byte record: kind, k, 0, slot, shift) */
   for (unsigned k = 0; k < ARRAY_SIZE(c->const_used); k++) {
      if (c->const_slot[k] < 0)
         continue;
      unsigned fmt = c->const_fmt[k] ? c->const_fmt[k] : 1;
      if (fmt == 3) {
         u16(&b, 3); u16(&b, k); u16(&b, 0); u16(&b, c->const_slot[k]); u16(&b, c->const_shift[k]);
      } else {
         u16(&b, fmt); u16(&b, k); u16(&b, c->const_slot[k]); u16(&b, c->const_shift[k]);
      }
   }
   /* no secondary update program; vertex: the live input mask */
   uint32_t inmask[4] = { 0 };
   unsigned n_inmask = 0;
   if (!frag) {
      list_for_each_entry(struct sgx_ins, i, &c->ins, link) {
         for (unsigned k = i->ndst; k < i->nops; k++) {
            const struct sgx_ref *r = &i->op[k];
            if (r->file != SGX_F_PA)
               continue;
            unsigned n = i->rpt > 1 && r->step ? i->rpt : 1;
            for (unsigned j = 0; j < n && r->n + j < 128; j++)
               inmask[(r->n + j) / 32] |= 1u << ((r->n + j) % 32);
         }
      }
      for (unsigned i = 0; i < 128; i++)
         if (inmask[i / 32] >> (i % 32) & 1)
            n_inmask = i + 1;
      for (unsigned i = 0; i < (n_inmask + 31) / 32; i++)
         u32(&b, inmask[i]);
      n_pa = n_inmask;
   }
   u32(&b, 0xffffffff); u32(&b, 0xffffffff); u32(&b, 0); u32(&b, 0);

   /* blocks, and the flat program alongside */
   struct code cd = { .n = 0 };
   struct util_dynarray flat;
   util_dynarray_init(&flat, NULL);
   unsigned label_at[512];
   struct { unsigned at, label; uint64_t w; } fix[256];
   unsigned nfix = 0;
   for (unsigned i = 0; i < 512; i++)
      label_at[i] = ~0u;

   u32(&b, 4); u16(&b, 3);       /* start label */
   list_for_each_entry(struct sgx_ins, i, &c->ins, link) {
      uint64_t w;
      unsigned at = util_dynarray_num_elements(&flat, uint64_t);
      switch (i->kind) {
      case SGX_INS_ALU:
         if (sgx_encode_ins(c, i, &w))
            goto fail;
         if (cd.n == ARRAY_SIZE(cd.w))
            flush(&b, &cd);
         cd.w[cd.n++] = w;
         util_dynarray_append_typed(&flat, uint64_t, w);
         if (i->form == USSE_PCK_F32_U8 || i->form == USSE_PCK_U8_F32 || i->form == USSE_LIMM ||
             i->form == USSE_MOV)
            out->n_alu++;
         else
            out->n_alu++;
         break;
      case SGX_INS_LABEL:
         flush(&b, &cd);
         u32(&b, 4); u16(&b, i->label);
         if (i->label < 512)
            label_at[i->label] = at;
         break;
      case SGX_INS_BR: {
         struct usse_inst m;
         usse_init(&m, USSE_BR);
         if (i->pred)
            usse_set(&m, USSE_F_PRED, i->pred);
         m.nops = 1;
         m.op[0] = usse_reg(USSE_K_IMM, 0);
         const char *err;
         if (usse_encode(&m, USSE_FC_DEFAULT, &w, &err)) {
            sgx_fail(c, "branch: %s", err);
            goto fail;
         }
         flush(&b, &cd);
         u32(&b, 3); inst(&b, w); u16(&b, i->label);
         fix[nfix].at = at;
         fix[nfix].label = i->label;
         fix[nfix++].w = w;
         util_dynarray_append_typed(&flat, uint64_t, w);
         out->n_flow++;
         break;
      }
      case SGX_INS_FEEDBACK: {
         struct usse_inst m;
         usse_init(&m, USSE_NOP);
         const char *err;
         usse_encode(&m, USSE_FC_DEFAULT, &w, &err);
         flush(&b, &cd);
         u32(&b, 4); u16(&b, 4);
         cd.w[cd.n++] = w;
         flush(&b, &cd);
         u32(&b, 4); u16(&b, 5);
         util_dynarray_append_typed(&flat, uint64_t, w);
         break;
      }
      case SGX_INS_SAMPLE:
         flush(&b, &cd);
         if (sample_blocks(&b, c, i->smp))
            goto fail;
         out->n_tex++;
         break;
      }
   }
   flush(&b, &cd);
   u32(&b, 7);

   /* flat program: resolve branch offsets (target index - this index) */
   uint64_t *fw = flat.data;
   for (unsigned k = 0; k < nfix; k++) {
      unsigned t = label_at[fix[k].label];
      if (t == ~0u) {
         sgx_fail(c, "branch to an undefined label");
         goto fail;
      }
      struct usse_inst m;
      usse_init(&m, USSE_BR);
      uint64_t w = fix[k].w;
      int off = (int)t - (int)fix[k].at;
      w = (w & ~0x3ffffull) | ((uint64_t)off & 0x3ffff);
      fw[fix[k].at] = w;
   }

   /* the header */
   unsigned n_c = 0;
   for (unsigned k = 0; k < ARRAY_SIZE(c->const_used); k++)
      n_c += c->const_slot[k] >= 0;
   unsigned n_slots = c->n_sa;
   put32(&b, 0x00, 0x55535020);
   put32(&b, 0x04, 0x23);
   put32(&b, 0x08, b.d.size - 12);
   put32(&b, 0x0c, 1);
   put32(&b, 0x10, frag ? 0 : 1);
   put32(&b, 0x14, (c->n_samples ? 2 : 0) | (c->discards ? 8 : 0));
   put32(&b, 0x18, (frag ? 0x80040104 : 0xe0040104) | (c->discards ? 0x820 : 0));
   put32(&b, 0x1c, c->discards ? 0x1 : 0x40);
   put32(&b, 0x20, 9);
   put32(&b, 0x24, 0x2793);
   put32(&b, 0x2c, n_c ? 8 : 0);
   put16(&b, 0x34, 0xffff);
   put16(&b, 0x36, 0xffff);
   put16(&b, 0x3c, c->sa_base);
   put16(&b, 0x3e, frag ? 0x77 : 0x1f6);
   put16(&b, 0x40, 0xffff);
   put16(&b, 0x44, n_pa);
   put16(&b, 0x46, c->n_temps);
   put16(&b, 0x4e, n_a);
   put16(&b, 0x50, n_c);
   if (frag) {
      put16(&b, 0x66, 3);
      put16(&b, 0x68, c->colour_pa);
      put16(&b, 0x6a, 4);
      put16(&b, 0x6c, 1);
   }
   put16(&b, 0x74, 3);
   put16(&b, 0x76, c->discards ? 4 : 0xffff);
   put16(&b, 0x78, c->discards ? 5 : 0xffff);
   put16(&b, 0x7a, 0xffff);
   put16(&b, 0x7e, n_slots);
   put16(&b, 0x80, n_inmask);
   put16(&b, 0x82, 0x80);

   out->usp = malloc(b.d.size);
   memcpy(out->usp, b.d.data, b.d.size);
   out->usp_size = b.d.size;
   out->n_code = util_dynarray_num_elements(&flat, uint64_t);
   out->code = malloc(MAX2(flat.size, 8));
   memcpy(out->code, flat.data, flat.size);
   out->n_temps = c->n_temps;
   out->n_pa = n_pa;
   c->n_pa = n_pa;
   util_dynarray_fini(&flat);
   util_dynarray_fini(&b.d);
   return 0;
fail:
   util_dynarray_fini(&flat);
   util_dynarray_fini(&b.d);
   return -1;
}
