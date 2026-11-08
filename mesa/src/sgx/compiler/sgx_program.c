/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * The finished program: the instruction list encoded into one flat array
 * with branch offsets resolved, its structure (struct sgx_block), its texture
 * samples, and the decoded lists the driver loads (constants, iterations,
 * the vertex input mask).
 */
#include <stdio.h>
#include <string.h>

#include "util/u_dynarray.h"
#include "nir.h"

#include "sgx_priv.h"

static unsigned
phys(struct sgx_ctx *c, const struct sgx_ref *r)
{
   return r->file == SGX_F_V ? sgx_vreg(c, r->n)->phys + r->comp : r->n;
}

/* A dependent sample's smp: destination r0 and texture state #0, which the
 * driver replaces (docs/ISA.md "The texture-sample sequence"). */
static int
smp_template(struct sgx_ctx *c, const struct sgx_sample *s, uint64_t *w)
{
   struct sgx_ins *i = s->ins;
   struct sgx_isa_instr m;
   sgx_form_init(&m, SGX_OP_SMP);
   m.skipinv = true;
   m.dim = s->dim == 3 ? 2 : 1;
   m.lod = s->lod == 1 ? 1 : 0;
   m.dst = (struct sgx_isa_src){.file = SGX_ISA_FILE_R};
   struct sgx_ref cr = i->op[1];
   unsigned cn = cr.file == SGX_F_U ? sgx_const_sa(c, cr.n) : phys(c, &cr);
   if (cr.h16 && cr.file == SGX_F_V)
      cn = sgx_vreg(c, cr.n)->phys + cr.comp / 2;
   else if (cr.h16 && cr.file != SGX_F_U)
      cn = cr.n / 2;
   m.src[0] = (struct sgx_isa_src){
      .file = cr.file == SGX_F_PA  ? SGX_ISA_FILE_PA
              : cr.file == SGX_F_U ? SGX_ISA_FILE_SA
                                   : SGX_ISA_FILE_R,
      .num = cn,
   };
   if (s->f16coord)
      m.cfmt = 1;
   m.src[1] = (struct sgx_isa_src){.file = SGX_ISA_FILE_IMM};
   if (s->lod) {
      struct sgx_ref lr = i->op[2];
      m.src[2] = (struct sgx_isa_src){
         .file = lr.file == SGX_F_C    ? SGX_ISA_FILE_C
                 : lr.file == SGX_F_PA ? SGX_ISA_FILE_PA
                 : lr.file == SGX_F_U  ? SGX_ISA_FILE_SA
                                       : SGX_ISA_FILE_R,
         .num = lr.file == SGX_F_U ? sgx_const_sa(c, lr.n) : phys(c, &lr),
         .neg = lr.neg,
         .abs = lr.abs,
      };
   }
   *w = sgx_isa_encode(&m, NULL);
   return 0;
}

static void
block(struct util_dynarray *b, unsigned kind, unsigned label, unsigned at,
      unsigned n)
{
   struct sgx_block x = {.kind = kind, .label = label, .at = at, .n = n};
   util_dynarray_append_typed(b, struct sgx_block, x);
}

int
sgx_finish_program(struct sgx_ctx *c, struct sgx_shader *out)
{
   bool frag = c->stage == MESA_SHADER_FRAGMENT;
   struct util_dynarray flat, blocks;
   util_dynarray_init(&flat, NULL);
   util_dynarray_init(&blocks, NULL);
   unsigned label_at[512];
   struct {
      unsigned at, label, pred;
   } fix[256];
   unsigned nfix = 0;
   int run = -1; /* the open SGX_BLOCK_CODE, or -1 */
   for (unsigned i = 0; i < 512; i++)
      label_at[i] = ~0u;

   list_for_each_entry (struct sgx_ins, i, &c->ins, link) {
      uint64_t w;
      unsigned at = util_dynarray_num_elements(&flat, uint64_t);
      if (i->kind != SGX_INS_ALU)
         run = -1;
      switch (i->kind) {
      case SGX_INS_ALU:
         if (sgx_encode_ins(c, i, &w))
            goto fail;
         if (run < 0) {
            run = util_dynarray_num_elements(&blocks, struct sgx_block);
            block(&blocks, SGX_BLOCK_CODE, 0, at, 0);
         }
         util_dynarray_element(&blocks, struct sgx_block, run)->n++;
         util_dynarray_append_typed(&flat, uint64_t, w);
         out->n_alu++;
         break;
      case SGX_INS_LABEL:
         block(&blocks, SGX_BLOCK_LABEL, i->label, at, 0);
         if (i->label < 512)
            label_at[i->label] = at;
         break;
      case SGX_INS_BR: {
         if (nfix == ARRAY_SIZE(fix)) {
            sgx_fail(c, "too many branches");
            goto fail;
         }
         block(&blocks, SGX_BLOCK_BRANCH, i->label, at, 1);
         fix[nfix].at = at;
         fix[nfix].pred = i->pred;
         fix[nfix++].label = i->label;
         util_dynarray_append_typed(&flat, uint64_t, 0);
         out->n_flow++;
         break;
      }
      case SGX_INS_FEEDBACK: {
         struct sgx_isa_instr m;
         sgx_form_init(&m, SGX_OP_NOP);
         w = sgx_isa_encode(&m, NULL);
         block(&blocks, SGX_BLOCK_FEEDBACK, 0, at, 1);
         util_dynarray_append_typed(&flat, uint64_t, w);
         break;
      }
      case SGX_INS_SAMPLE: {
         struct sgx_sample *s = i->smp;
         if (out->n_samples == ARRAY_SIZE(out->samples)) {
            sgx_fail(c, "too many texture samples");
            goto fail;
         }
         struct sgx_sample_info *o = &out->samples[out->n_samples];
         o->at = at;
         o->id = s->id;
         o->sampler = s->sampler;
         o->set = s->set;
         o->dim = s->dim;
         o->dependent = s->dependent;
         o->proj = s->proj;
         o->coord_pa = s->coord_pa;
         o->reg = s->dependent ? phys(c, &s->ins->op[0]) : s->pa;
         if (s->dependent && smp_template(c, s, &o->smp))
            goto fail;
         block(&blocks, SGX_BLOCK_SAMPLE, 0, out->n_samples++, 0);
         out->n_tex++;
         break;
      }
      }
   }

   /* branches, now that the targets are known: target index - this index */
   uint64_t *fw = flat.data;
   for (unsigned k = 0; k < nfix; k++) {
      unsigned t = label_at[fix[k].label];
      if (t == ~0u) {
         sgx_fail(c, "branch to an undefined label");
         goto fail;
      }
      struct sgx_isa_instr m;
      sgx_form_init(&m, SGX_OP_BR);
      m.pred = fix[k].pred;
      m.target = (int)t - (int)fix[k].at;
      fw[fix[k].at] = sgx_isa_encode(&m, NULL);
   }

   /* PA registers: the iterations, the colour, and the DDK driver's
    * placeholder colour iteration when nothing else is iterated
    * (gl_PointCoord alone counts as nothing) */
   unsigned n_pa = 0;
   if (frag) {
      bool real = false;
      for (unsigned i = 0; i < c->n_iters; i++) {
         struct sgx_iter *it = &c->iters[i];
         unsigned regs = it->sample ? 1
                         : it->f16  ? (it->comps + 1) / 2
                                    : it->comps;
         n_pa = MAX2(n_pa, it->pa + regs);
         real |= !it->pointcoord;
      }
      if (!real)
         n_pa++;
      n_pa = MAX2(n_pa, c->colour_pa + 1);
      if (c->has_depth)
         n_pa = MAX2(n_pa, c->depth_pa + 1);
   }

   /* vertex: the live input mask */
   uint32_t inmask[4] = {0};
   if (!frag) {
      list_for_each_entry (struct sgx_ins, i, &c->ins, link) {
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
            n_pa = i + 1;
   }

   out->sa_base = c->sa_base;
   out->sa_slots = c->n_sa_phys;
   out->mem_dwords = c->mem_dwords;
   out->n_mem_bases = c->n_mem_bases;
   for (unsigned j = 0; j < c->n_mem_bases; j++)
      out->mem_base_slot[j] = c->mem_base_sa[j];
   out->uses_front_face = c->face_vreg >= 0;
   out->n_consts = 0;
   unsigned n_map = c->n_lits;
   for (unsigned k = 0; k < ARRAY_SIZE(c->const_used); k++)
      n_map += c->const_slot[k] >= 0;
   if (n_map > ARRAY_SIZE(out->consts)) {
      sgx_fail(c, "%u constants, the map holds %zu", n_map,
               ARRAY_SIZE(out->consts));
      goto fail;
   }
   for (unsigned k = 0; k < ARRAY_SIZE(c->const_used); k++) {
      if (c->const_slot[k] < 0)
         continue;
      struct sgx_const_map *m = &out->consts[out->n_consts++];
      m->k = k;
      m->slot = c->slot_sa[c->const_slot[k]] < 0 ? SGX_SLOT_NONE
                                                 : c->slot_sa[c->const_slot[k]];
      m->mem = c->mem_dwords ? c->const_slot[k] + 1 : 0;
      m->shift = c->const_shift[k];
      m->fmt = c->const_fmt[k] == 4   ? 3
               : c->const_fmt[k] == 5 ? 0x10
               : c->const_fmt[k]      ? c->const_fmt[k]
                                      : 1;
   }
   for (unsigned i = 0; i < c->n_lits; i++) {
      struct sgx_const_map *m = &out->consts[out->n_consts++];
      m->k = 0xffff;
      m->slot = c->slot_sa[c->lit_slot[i]] < 0 ? SGX_SLOT_NONE
                                               : c->slot_sa[c->lit_slot[i]];
      m->mem = c->mem_dwords ? c->lit_slot[i] + 1 : 0;
      m->shift = 0;
      m->fmt = 0x11;
      m->value = c->lit_bits[i];
   }
   out->n_iters = frag ? MIN2(c->n_iters, ARRAY_SIZE(out->iters)) : 0;
   for (unsigned i = 0; i < out->n_iters; i++) {
      struct sgx_iter *it = &c->iters[i];
      struct sgx_iter_info *o = &out->iters[i];
      o->set = it->set;
      o->comps = it->comps;
      o->pa = it->pa;
      o->regs = it->sample ? 1 : it->f16 ? (it->comps + 1) / 2 : it->comps;
      o->f16 = it->f16;
      o->sample = it->sample;
      o->proj = it->proj;
      o->sampler = it->sampler;
      o->pointcoord = it->pointcoord;
   }
   out->colour_pa = c->colour_pa;
   out->discards = c->discards;
   out->writes_depth = c->has_depth;
   out->depth_pa = c->depth_pa;
   memcpy(out->in_mask, inmask, sizeof(inmask));
   for (unsigned i = 0; i < SGX_MAX_SETS; i++)
      out->set_comps[i] = frag ? 0 : c->vary->set_comps[i];
   out->point_size = !frag && c->vary->point_size;
   out->n_out_regs = frag ? 0 : c->n_out_regs;
   out->n_code = util_dynarray_num_elements(&flat, uint64_t);
   out->code = malloc(MAX2(flat.size, 8));
   memcpy(out->code, flat.data, flat.size);
   /* the words as sgxc's .code (high, low), for tools/isa/dis.py */
   if (c->opts->verbose) {
      for (unsigned i = 0; i < out->n_code; i++)
         fprintf(stderr, "%08x %08x\n", (unsigned)(out->code[i] >> 32),
                 (unsigned)out->code[i]);
   }
   out->n_blocks = util_dynarray_num_elements(&blocks, struct sgx_block);
   out->blocks = malloc(MAX2(blocks.size, sizeof(struct sgx_block)));
   memcpy(out->blocks, blocks.data, blocks.size);
   out->n_temps = c->n_temps;
   out->n_pa = n_pa;
   c->n_pa = n_pa;
   util_dynarray_fini(&flat);
   util_dynarray_fini(&blocks);
   return 0;
fail:
   util_dynarray_fini(&flat);
   util_dynarray_fini(&blocks);
   return -1;
}
