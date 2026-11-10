/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * SGX545 compiler: NIR options, varying set allocation, I/O assignment and
 * the NIR lowering pipeline that runs before instruction selection.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "nir.h"
#include "nir_builder.h"
#include "nir_deref.h"
#include "nir_legacy.h"

#include "sgx_priv.h"

static int place(const struct sgx_compile_options *o, enum sgx_layout_kind k,
                 const char *name);
static unsigned first_free(const struct sgx_compile_options *o,
                           enum sgx_layout_kind k);

static const nir_shader_compiler_options options = {
   .lower_fdiv = true,
   .lower_flrp32 = true,
   .lower_fpow = true,
   .lower_fmod = true,
   .lower_fsign = true,
   .lower_fceil = true,
   .lower_ftrunc = true,
   .lower_fround_even = true,
   .lower_fdph = true,
   .lower_bitops = true,
   .lower_extract_byte = true,
   .lower_extract_word = true,
   .lower_insert_byte = true,
   .lower_insert_word = true,
   /* fmad: the product is rounded before the add (hwt_p7: not fused) */
   .float_mul_add32 =
      nir_float_muladd_support_has_fmad | nir_float_muladd_support_fuse,
   /* fmad16, as libusc fuses a mediump a * b + c (one rounding) */
   .float_mul_add16 =
      nir_float_muladd_support_has_fmad | nir_float_muladd_support_fuse,
   .no_integers = true,
   .max_unroll_iterations = 8, /* (libusc unrolls nothing; a 16-trip loop with a
                                  break became 125 instructions) */
   .lower_int64_options = ~0,
   /* (not lower_doubles_options = ~0: GLSL ES has no doubles, and with it
    * 144 dEQP fragment loops came out wrong on hardware) */
   .force_indirect_unrolling = nir_var_all,
   .force_indirect_unrolling_sampler = true,
};

const nir_shader_compiler_options *
sgx_nir_options(void)
{
   return &options;
}

/* ------------------------------------------------------------------------
 * varyings -> texture coordinate sets, as glslcompiler allocates them
 * (ISA.md "Varyings to sets"): by type mat4, mat2, vec4,
 * mat3, vec3, vec2, float; within a type the largest array first, then
 * declaration order; one set per row while sets last.
 */
static int
type_rank(const struct glsl_type *t)
{
   t = glsl_without_array(t);
   unsigned c = glsl_get_vector_elements(t), m = glsl_get_matrix_columns(t);
   if (m > 1)
      return m == 4 ? 0 : m == 2 ? 1 : 3;
   return c == 4 ? 2 : c == 3 ? 4 : c == 2 ? 5 : 6;
}

/* is the variable read (or written) anywhere? Works on deref and on
 * lowered I/O (Mesa's GLSL compiler hands over either) */
static bool
var_used(nir_shader *nir, nir_variable *var, bool store)
{
   unsigned slots = glsl_count_vec4_slots(var->type, false, false);
   nir_foreach_function_impl (impl, nir) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            switch (in->intrinsic) {
            case nir_intrinsic_store_deref:
            case nir_intrinsic_load_deref:
            case nir_intrinsic_interp_deref_at_centroid:
               if ((in->intrinsic == nir_intrinsic_store_deref) == store &&
                   nir_deref_instr_get_variable(nir_src_as_deref(in->src[0])) ==
                      var)
                  return true;
               break;
            case nir_intrinsic_copy_deref:
               if (nir_deref_instr_get_variable(
                      nir_src_as_deref(in->src[store ? 0 : 1])) == var)
                  return true;
               break;
            case nir_intrinsic_store_output:
            case nir_intrinsic_load_input:
            case nir_intrinsic_load_interpolated_input: {
               if ((in->intrinsic == nir_intrinsic_store_output) != store)
                  break;
               int loc = nir_intrinsic_io_semantics(in).location;
               if (loc >= var->data.location &&
                   loc < var->data.location + (int)slots)
                  return true;
               break;
            }
            default:
               break;
            }
         }
      }
   }
   return false;
}

void
sgx_assign_varyings(nir_shader *nir, bool vertex, struct sgx_varyings *out)
{
   nir_variable *vars[32];
   unsigned n = 0;
   nir_variable_mode mode = vertex ? nir_var_shader_out : nir_var_shader_in;

   memset(out, 0, sizeof(*out));
   nir_foreach_variable_with_modes (var, nir, mode) {
      /* (before linking, user varyings have no location yet) */
      bool builtin =
         !strncmp(var->name, "gl_", 3) ||
         (var->data.location >= 0 && var->data.location < VARYING_SLOT_VAR0);
      if (vertex && builtin &&
          (var->data.location == VARYING_SLOT_PSIZ ||
           !strcmp(var->name, "gl_PointSize")) &&
          var_used(nir, var, true))
         out->point_size = true;
      /* the point-sprite coordinate is iterated from set 0, the set a vertex
       * shader that writes gl_PointSize reserves (DDK capture: gl_PointCoord
       * = list A iterator 1) */
      if (!vertex && builtin &&
          (var->data.location == VARYING_SLOT_PNTC ||
           !strcmp(var->name, "gl_PointCoord")) &&
          var_used(nir, var, false))
         out->point_size = true;
      if (builtin || n >= 32)
         continue;
      if (!var_used(nir, var, vertex))
         continue;
      vars[n++] = var;
   }
   /* stable sort: rank, then larger arrays first */
   for (unsigned i = 1; i < n; i++) {
      for (unsigned j = i; j > 0; j--) {
         nir_variable *a = vars[j - 1], *b = vars[j];
         int ra = type_rank(a->type), rb = type_rank(b->type);
         unsigned la =
            glsl_type_is_array(a->type) ? glsl_get_length(a->type) : 1;
         unsigned lb =
            glsl_type_is_array(b->type) ? glsl_get_length(b->type) : 1;
         if (ra < rb || (ra == rb && la >= lb))
            break;
         vars[j - 1] = b;
         vars[j] = a;
      }
   }
   unsigned set = 0;
   if (out->point_size) {
      out->set_comps[0] = 2;
      set = 1;
   }
   for (unsigned i = 0; i < n; i++) {
      nir_variable *var = vars[i];
      const struct glsl_type *e = glsl_without_array(var->type);
      unsigned len =
         glsl_type_is_array(var->type) ? glsl_get_length(var->type) : 1;
      unsigned cols = glsl_get_matrix_columns(e);
      struct sgx_varying *v = &out->v[out->n++];
      snprintf(v->name, sizeof v->name, "%s", var->name);
      v->rows = len * cols;
      v->comps = glsl_get_vector_elements(e);
      v->precision = var->data.precision;
      v->set = set;
      for (unsigned r = 0; r < v->rows; r++, set++)
         if (set < SGX_MAX_SETS)
            out->set_comps[set] = v->comps;
   }
   sgx_pack_varyings(out);
}

bool
sgx_pack_varyings(struct sgx_varyings *out)
{
   bool fits = true;
   for (unsigned i = 0; i < out->n; i++)
      fits &= out->v[i].set + out->v[i].rows <= SGX_MAX_SETS &&
              out->v[i].comp + out->v[i].comps <= 4;
   if (fits)
      return true;

   /* glslcompiler: a set per row while sets remain, then rows into free
    * components of earlier sets (ISA.md "Varyings to sets"). It packs by
    * precision too, but a varying's precision may differ between the stages,
    * each placed on its own, so not here (a mixed set is iterated in f32). A
    * sprite's set 0 is the point coordinate's alone. */
   uint8_t used[SGX_MAX_SETS] = {0};
   unsigned next = out->point_size ? 1 : 0;
   memset(out->set_comps, 0, sizeof(out->set_comps));
   if (out->point_size) {
      used[0] = 0xf;
      out->set_comps[0] = 2;
   }
   for (unsigned i = 0; i < out->n; i++) {
      struct sgx_varying *v = &out->v[i];
      unsigned mask = (1u << v->comps) - 1;
      bool placed = false;
      if (next + v->rows <= SGX_MAX_SETS) {
         v->set = next;
         v->comp = 0;
         next += v->rows;
         placed = true;
      }
      for (unsigned st = 0; st + v->rows <= next && !placed; st++) {
         for (unsigned c = 0; c + v->comps <= 4 && !placed; c++) {
            bool ok = true;
            for (unsigned r = 0; r < v->rows && ok; r++)
               ok = !(used[st + r] & mask << c);
            if (ok) {
               v->set = st;
               v->comp = c;
               placed = true;
            }
         }
      }
      if (!placed)
         return false;
      for (unsigned r = 0; r < v->rows; r++) {
         used[v->set + r] |= mask << v->comp;
         out->set_comps[v->set + r] =
            MAX2(out->set_comps[v->set + r], v->comp + v->comps);
      }
   }
   return true;
}

/* ------------------------------------------------------------------------
 * sin/cos: the hardware's fsin/fcos take quarter turns and are accurate
 * only on [-2, 2) (hwt_p3), so reduce: t = 4 * fract(x / 2pi + c) - 2, and
 * sin(x) = fsin(t) for c = 0.5, cos(x) = fsin(t) for c = 0.75.
 * fsin_agx is NIR's sin(t * pi / 2).
 */
static bool
lower_sincos_instr(nir_builder *b, nir_alu_instr *alu, void *data)
{
   if (alu->op != nir_op_fsin && alu->op != nir_op_fcos)
      return false;
   b->cursor = nir_before_instr(&alu->instr);
   nir_def *x = nir_ssa_for_alu_src(b, alu, 0);
   /* (a mediump argument reduced in f32: F16 would keep few bits of the
    * fraction of a larger angle) */
   if (x->bit_size == 16)
      x = nir_f2f32(b, x);
   double c = alu->op == nir_op_fsin ? 0.5 : 0.75;
   nir_def *u = nir_ffract(
      b, nir_fmad(b, x, nir_imm_float(b, 0.5 / M_PI), nir_imm_float(b, c)));
   nir_def *t = nir_fmad(b, u, nir_imm_float(b, 4.0), nir_imm_float(b, -2.0));
   nir_def *r = nir_fsin_agx(b, t);
   if (alu->def.bit_size == 16)
      r = nir_f2f16(b, r);
   nir_def_replace(&alu->def, r);
   return true;
}

static bool
lower_sincos(nir_shader *s)
{
   return nir_shader_alu_pass(s, lower_sincos_instr, nir_metadata_control_flow,
                              NULL);
}

/* sampler derefs -> texture/sampler indices. The USP's sampler number is
 * glslcompiler's: it counts the samplers a shader uses, in order of use
 * (corpus c3 tx_nd_s1: s1 alone is sampler 0; tx_nd_two: s, s1 = 0, 1),
 * unless the caller pinned it (-s). */
struct tex_ctx {
   struct sgx_ctx *c;
   nir_variable *seen[16];
   unsigned n;
};

static unsigned
sampler_number(struct tex_ctx *t, nir_variable *var)
{
   const struct sgx_compile_options *o = t->c->opts;
   int pinned = place(o, SGX_LAYOUT_SAMPLER, var->name);
   if (pinned >= 0)
      return pinned;
   unsigned next = first_free(o, SGX_LAYOUT_SAMPLER);
   for (unsigned i = 0; i < t->n; i++)
      if (t->seen[i] == var)
         return next + i;
   if (t->n < 16)
      t->seen[t->n++] = var;
   return next + t->n - 1;
}

static bool
lower_tex_derefs_instr(nir_builder *b, nir_instr *instr, void *data)
{
   struct tex_ctx *tc = data;
   if (instr->type != nir_instr_type_tex)
      return false;
   nir_tex_instr *tex = nir_instr_as_tex(instr);
   bool progress = false;
   for (int i = tex->num_srcs - 1; i >= 0; i--) {
      nir_tex_src_type t = tex->src[i].src_type;
      if (t != nir_tex_src_texture_deref && t != nir_tex_src_sampler_deref)
         continue;
      nir_variable *var =
         nir_deref_instr_get_variable(nir_src_as_deref(tex->src[i].src));
      unsigned unit = var ? sampler_number(tc, var) : 0;
      if (t == nir_tex_src_texture_deref)
         tex->texture_index = unit;
      else
         tex->sampler_index = unit;
      nir_tex_instr_remove_src(tex, i);
      progress = true;
   }
   return progress;
}

static bool
lower_tex_derefs(nir_shader *s, struct sgx_ctx *c)
{
   struct tex_ctx t = {.c = c};
   return nir_shader_instructions_pass(s, lower_tex_derefs_instr,
                                       nir_metadata_control_flow, &t);
}

/* gl_FrontFacing as the constant true (see sgx_compile_options.front_sa) */
static bool
front_true_instr(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
   if ((in->intrinsic != nir_intrinsic_load_input &&
        in->intrinsic != nir_intrinsic_load_front_face) ||
       (in->intrinsic == nir_intrinsic_load_input &&
        nir_intrinsic_io_semantics(in).location != VARYING_SLOT_FACE))
      return false;
   b->cursor = nir_before_instr(&in->instr);
   nir_def_replace(&in->def, nir_imm_intN_t(b, ~0ull, in->def.bit_size));
   return true;
}

/* The 32-bit value of component comp of a 16-bit integer that a conversion
 * made (f2i16(x), i2i16(y), b2i16(c), a constant), through moves and vectors;
 * NULL when it isn't one */
static nir_def *
int16_as_32(nir_builder *b, nir_def *d, unsigned comp)
{
   if (nir_def_is_const(d) && d->bit_size == 16)
      return nir_imm_int(b, nir_src_comp_as_int(nir_src_for_ssa(d), comp));
   nir_alu_instr *p = nir_def_as_alu_or_null(d);
   if (!p)
      return NULL;
   if (nir_op_is_vec(p->op))
      return int16_as_32(b, p->src[comp].src.ssa, p->src[comp].swizzle[0]);
   nir_def *x = p->src[0].src.ssa;
   unsigned xc = p->src[0].swizzle[comp];
   switch (p->op) {
   case nir_op_mov:
      return int16_as_32(b, x, xc);
   case nir_op_f2i16:
   case nir_op_f2u16: {
      nir_def *f = nir_channel(b, x, xc);
      if (f->bit_size != 32)
         f = nir_f2f32(b, f);
      return p->op == nir_op_f2i16 ? nir_f2i32(b, f) : nir_f2u32(b, f);
   }
   case nir_op_i2i16:
   case nir_op_u2u16:
      return x->bit_size == 32 ? nir_channel(b, x, xc) : int16_as_32(b, x, xc);
   case nir_op_b2i16:
      return nir_b2i32(b, nir_channel(b, x, xc));
   case nir_op_bcsel: {
      nir_def *t = int16_as_32(b, p->src[1].src.ssa, p->src[1].swizzle[comp]);
      nir_def *e = int16_as_32(b, p->src[2].src.ssa, p->src[2].swizzle[comp]);
      return t && e ? nir_bcsel(b, nir_channel(b, x, xc), t, e) : NULL;
   }
   default:
      return NULL;
   }
}

/* What nir_lower_bit_size leaves of 16-bit integers (mediump int, under
 * sgxc -p) are conversions at the edges: read them from the 32-bit value */
static bool
int16_conv_instr(nir_builder *b, nir_alu_instr *alu, void *data)
{
   if ((alu->op == nir_op_i2i32 || alu->op == nir_op_u2u32) &&
       nir_src_bit_size(alu->src[0].src) == 32) {
      /* (what's left of a 16-bit round trip) */
      b->cursor = nir_before_instr(&alu->instr);
      nir_def_replace(&alu->def, nir_ssa_for_alu_src(b, alu, 0));
      return true;
   }
   if (nir_op_infos[alu->op].num_inputs != 1 ||
       nir_src_bit_size(alu->src[0].src) != 16)
      return false;
   if (alu->op != nir_op_i2i32 && alu->op != nir_op_u2u32 &&
       alu->op != nir_op_i2f32 && alu->op != nir_op_u2f32 &&
       alu->op != nir_op_i2f16 && alu->op != nir_op_u2f16)
      return false;
   b->cursor = nir_before_instr(&alu->instr);
   nir_def *xs[NIR_MAX_VEC_COMPONENTS];
   for (unsigned k = 0; k < alu->def.num_components; k++) {
      xs[k] = int16_as_32(b, alu->src[0].src.ssa, alu->src[0].swizzle[k]);
      if (!xs[k])
         return false;
   }
   nir_def *x = nir_vec(b, xs, alu->def.num_components);
   nir_def *r = x;
   if (alu->op == nir_op_i2f32 || alu->op == nir_op_i2f16)
      r = nir_i2f32(b, x);
   else if (alu->op == nir_op_u2f32 || alu->op == nir_op_u2f16)
      r = nir_u2f32(b, x);
   if (alu->def.bit_size == 16)
      r = nir_f2f16(b, r);
   nir_def_replace(&alu->def, r);
   return true;
}

/* float(i) for a mediump result: nir_lower_int_to_float only takes 32-bit
 * conversions, so the i2f32 it turns into a move, then f2f16 */
static bool
i2f16_instr(nir_builder *b, nir_alu_instr *alu, void *data)
{
   if ((alu->op != nir_op_i2f16 && alu->op != nir_op_u2f16) ||
       nir_src_bit_size(alu->src[0].src) != 32)
      return false;
   b->cursor = nir_before_instr(&alu->instr);
   nir_def *x = nir_ssa_for_alu_src(b, alu, 0);
   nir_def_replace(&alu->def,
                   nir_f2f16(b, alu->op == nir_op_i2f16 ? nir_i2f32(b, x)
                                                        : nir_u2f32(b, x)));
   return true;
}

static bool
csel16_instr(nir_builder *b, nir_alu_instr *alu, void *data)
{
   if (alu->op != nir_op_bcsel || alu->def.bit_size != 16)
      return false;
   b->cursor = nir_before_instr(&alu->instr);
   nir_def *cond = nir_b2f32(b, nir_ssa_for_alu_src(b, alu, 0));
   nir_def_replace(&alu->def,
                   nir_b32csel(b, cond, nir_ssa_for_alu_src(b, alu, 1),
                               nir_ssa_for_alu_src(b, alu, 2)));
   return true;
}

/* An f32 op exact under rounding (select, min, max, saturate, negate, abs,
 * move, vec) that only f2f16 reads, over values that are F16 underneath: done
 * in F16, so chains (if / return selects of mediump values) and the uniforms
 * under them stay F16 instead of widening and narrowing again. */
static bool
narrow16_op(nir_op op)
{
   return op == nir_op_bcsel || op == nir_op_fmin || op == nir_op_fmax ||
          op == nir_op_fsat || op == nir_op_fneg || op == nir_op_fabs ||
          nir_op_is_vec_or_mov(op);
}

/* an f32 value whose F16 costs nothing: the f2f32 of one, a constant, a
 * uniform (an F16 constant then), or such an op over them */
static bool
f16_free(nir_def *d, unsigned depth)
{
   nir_instr *p = nir_def_instr(d);
   if (p->type == nir_instr_type_load_const)
      return true;
   if (p->type == nir_instr_type_intrinsic)
      return nir_instr_as_intrinsic(p)->intrinsic == nir_intrinsic_load_uniform;
   if (p->type != nir_instr_type_alu || depth > 8)
      return false;
   nir_alu_instr *a = nir_instr_as_alu(p);
   if (a->op == nir_op_f2f32)
      return nir_src_bit_size(a->src[0].src) == 16;
   if (!narrow16_op(a->op))
      return false;
   for (unsigned j = a->op == nir_op_bcsel ? 1 : 0;
        j < nir_op_infos[a->op].num_inputs; j++)
      if (!f16_free(a->src[j].src.ssa, depth + 1))
         return false;
   return true;
}

static bool
narrow16_instr(nir_builder *b, nir_alu_instr *alu, void *data)
{
   nir_op op = alu->op;
   if (alu->def.bit_size != 32 || !narrow16_op(op))
      return false;
   for (unsigned j = op == nir_op_bcsel ? 1 : 0;
        j < nir_op_infos[op].num_inputs; j++)
      if (!f16_free(alu->src[j].src.ssa, 0))
         return false;
   if (list_is_empty(&alu->def.uses))
      return false;
   nir_foreach_use_including_if (use, &alu->def) {
      nir_instr *u = nir_src_is_if(use) ? NULL : nir_src_use_instr(use);
      if (!u || u->type != nir_instr_type_alu ||
          (nir_instr_as_alu(u)->op != nir_op_f2f16 &&
           nir_instr_as_alu(u)->op != nir_op_f2fmp))
         return false;
   }
   b->cursor = nir_before_instr(&alu->instr);
   unsigned n = nir_op_infos[op].num_inputs;
   /* (each source's f2f16 takes its swizzle: no new 32-bit move to narrow) */
   nir_alu_instr *h = nir_alu_instr_create(b->shader, op);
   for (unsigned j = 0; j < n; j++) {
      if (op == nir_op_bcsel && j == 0) {
         nir_alu_src_copy(&h->src[0], &alu->src[0]);
         continue;
      }
      nir_alu_instr *cv = nir_alu_instr_create(b->shader, nir_op_f2f16);
      nir_alu_src_copy(&cv->src[0], &alu->src[j]);
      nir_def_init(&cv->instr, &cv->def,
                   nir_ssa_alu_instr_src_components(alu, j), 16);
      nir_builder_instr_insert(b, &cv->instr);
      h->src[j].src = nir_src_for_ssa(&cv->def);
      for (unsigned k = 0; k < NIR_MAX_VEC_COMPONENTS; k++)
         h->src[j].swizzle[k] = k;
   }
   nir_def_init(&h->instr, &h->def, alu->def.num_components, 16);
   nir_builder_instr_insert(b, &h->instr);
   nir_foreach_use_safe (use, &alu->def) {
      nir_alu_instr *cv = nir_instr_as_alu(nir_src_use_instr(use));
      b->cursor = nir_before_instr(&cv->instr);
      unsigned swz[NIR_MAX_VEC_COMPONENTS];
      for (unsigned j = 0; j < cv->def.num_components; j++)
         swz[j] = cv->src[0].swizzle[j];
      nir_def_rewrite_uses(
         &cv->def, nir_swizzle(b, &h->def, swz, cv->def.num_components));
   }
   return true;
}

static bool
narrow16(nir_shader *s)
{
   bool progress = false, p = true;
   for (unsigned round = 0; p && round < 64; round++) {
      p = false;
      NIR_PASS(p, s, nir_shader_alu_pass, narrow16_instr,
               nir_metadata_control_flow, NULL);
      if (p) {
         NIR_PASS(_, s, nir_opt_algebraic);
         NIR_PASS(_, s, nir_opt_constant_folding);
         NIR_PASS(_, s, nir_opt_dce);
      }
      progress |= p;
   }
   return progress;
}

static nir_alu_instr *
alu_of(nir_src s)
{
   nir_instr *i = nir_def_instr(s.ssa);
   return i->type == nir_instr_type_alu ? nir_instr_as_alu(i) : NULL;
}

/* The colour is packed to U8 with pck's saturation: a clamp to [0, 1] of
 * what is stored there is the pack's own (libusc drops it, sat_m) */
static bool
colour_sat_instr(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
   if (in->intrinsic != nir_intrinsic_store_output)
      return false;
   unsigned loc = nir_intrinsic_io_semantics(in).location;
   if (loc != FRAG_RESULT_COLOR && loc != FRAG_RESULT_DATA0)
      return false;
   nir_alu_instr *a = alu_of(in->src[0]);
   nir_alu_instr *cv = NULL;
   if (a && a->op == nir_op_f2f32) {
      cv = a;
      a = alu_of(a->src[0].src);
   }
   if (!a || a->op != nir_op_fsat)
      return false;
   b->cursor = nir_before_instr(&in->instr);
   nir_def *x = nir_ssa_for_alu_src(b, a, 0);
   if (cv) {
      nir_alu_instr *w = nir_alu_instr_create(b->shader, nir_op_f2f32);
      w->src[0].src = nir_src_for_ssa(x);
      for (unsigned k = 0; k < NIR_MAX_VEC_COMPONENTS; k++)
         w->src[0].swizzle[k] = cv->src[0].swizzle[k];
      nir_def_init(&w->instr, &w->def, cv->def.num_components, 32);
      nir_builder_instr_insert(b, &w->instr);
      x = &w->def;
   }
   nir_src_rewrite(&in->src[0], x);
   return true;
}

/* ------------------------------------------------------------------------
 * fs_mediump: inputs, texels and uniforms that no texture coordinate needs
 * become F16, then every float op over F16 values is rebuilt in F16 (what
 * Mesa's precision lowering does for mediump GLSL), until nothing changes. */

static bool
feeds_coord(nir_def *d, unsigned depth)
{
   nir_foreach_use_including_if (use, d) {
      if (nir_src_is_if(use))
         continue;
      nir_instr *u = nir_src_use_instr(use);
      if (u->type == nir_instr_type_tex)
         return true;
      if (u->type == nir_instr_type_alu && depth < 4 &&
          nir_op_is_vec_or_mov(nir_instr_as_alu(u)->op) &&
          feeds_coord(&nir_instr_as_alu(u)->def, depth + 1))
         return true;
   }
   return false;
}

static bool
demote_op(nir_op op)
{
   switch (op) {
   case nir_op_fadd:
   case nir_op_fmul:
   case nir_op_ffma:
   case nir_op_flrp:
   case nir_op_fneg:
   case nir_op_fabs:
   case nir_op_fsat:
   case nir_op_fmin:
   case nir_op_fmax:
   case nir_op_frcp:
   case nir_op_frsq:
   case nir_op_fsqrt:
   case nir_op_fexp2:
   case nir_op_flog2:
   case nir_op_ffloor:
   case nir_op_ffract:
   case nir_op_fdot2:
   case nir_op_fdot3:
   case nir_op_fdot4:
   case nir_op_bcsel:
   case nir_op_mov:
   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4:
      return true;
   default:
      return false;
   }
}

/* the F16 value an op source is underneath (the f2f32 of one, a constant),
 * through the source's swizzle; NULL if it isn't one */
static nir_def *
src16(nir_builder *b, nir_alu_instr *alu, unsigned j)
{
   unsigned nc = nir_ssa_alu_instr_src_components(alu, j);
   unsigned swz[NIR_MAX_VEC_COMPONENTS];
   for (unsigned k = 0; k < nc; k++)
      swz[k] = alu->src[j].swizzle[k];
   if (nir_src_is_const(alu->src[j].src)) {
      /* (the f2f16 takes the swizzle: a 32-bit move of it would be
       * demoted again, forever) */
      nir_alu_instr *cv = nir_alu_instr_create(b->shader, nir_op_f2f16);
      nir_alu_src_copy(&cv->src[0], &alu->src[j]);
      nir_def_init(&cv->instr, &cv->def, nc, 16);
      nir_builder_instr_insert(b, &cv->instr);
      return &cv->def;
   }
   nir_alu_instr *p = alu_of(alu->src[j].src);
   if (!p || p->op != nir_op_f2f32 || nir_src_bit_size(p->src[0].src) != 16)
      return NULL;
   for (unsigned k = 0; k < nc; k++)
      swz[k] = p->src[0].swizzle[swz[k]];
   return nir_swizzle(b, p->src[0].src.ssa, swz, nc);
}

/* every use reads d as a float (through moves, vecs and selects) */
static bool
float_uses(nir_def *d, unsigned depth)
{
   nir_foreach_use_including_if (use, d) {
      if (nir_src_is_if(use))
         return false;
      nir_instr *u = nir_src_use_instr(use);
      if (u->type == nir_instr_type_intrinsic) {
         nir_intrinsic_instr *in = nir_instr_as_intrinsic(u);
         if (in->intrinsic == nir_intrinsic_store_output &&
             nir_alu_type_get_base_type(nir_intrinsic_src_type(in)) ==
                nir_type_float)
            continue;
         return false;
      }
      if (u->type != nir_instr_type_alu)
         return false;
      nir_alu_instr *a = nir_instr_as_alu(u);
      unsigned j = container_of(use, nir_alu_src, src) - a->src;
      if (nir_op_is_vec_or_mov(a->op) || (a->op == nir_op_bcsel && j > 0)) {
         if (depth >= 4 || !float_uses(&a->def, depth + 1))
            return false;
      } else if (nir_alu_type_get_base_type(
                    nir_op_infos[a->op].input_types[j]) != nir_type_float) {
         return false;
      }
   }
   return true;
}

/* every constant operand (bcsel's condition aside) is an F16 value exactly */
static bool
consts_f16_exact(nir_alu_instr *alu)
{
   for (unsigned j = 0; j < nir_op_infos[alu->op].num_inputs; j++) {
      if ((alu->op == nir_op_bcsel && j == 0) ||
          !nir_src_is_const(alu->src[j].src))
         continue;
      for (unsigned k = 0; k < nir_ssa_alu_instr_src_components(alu, j); k++) {
         float f =
            nir_src_comp_as_float(alu->src[j].src, alu->src[j].swizzle[k]);
         if (_mesa_half_to_float(_mesa_float_to_half(f)) != f)
            return false;
      }
   }
   return true;
}

static bool
demote_instr(nir_builder *b, nir_alu_instr *alu, void *data)
{
   nir_op op = alu->op;
   if (alu->def.bit_size != 32 || !demote_op(op) || feeds_coord(&alu->def, 0))
      return false;
   /* at least one operand F16 underneath, or constants that are F16 values
    * read as floats: constants alone don't say the value is a float
    * (bcsel(b, 3, 0) over ints, a 32-bit loop bound), nor that F16 holds
    * them (a highp atan's -pi/2) */
   unsigned n = nir_op_infos[op].num_inputs, f16 = 0;
   for (unsigned j = 0; j < n; j++) {
      if (op == nir_op_bcsel && j == 0)
         continue;
      nir_alu_instr *p = alu_of(alu->src[j].src);
      if (p && p->op == nir_op_f2f32 && nir_src_bit_size(p->src[0].src) == 16)
         f16++;
      else if (!nir_src_is_const(alu->src[j].src))
         return false;
   }
   if (!f16 && !(consts_f16_exact(alu) && float_uses(&alu->def, 0)))
      return false;
   b->cursor = nir_before_instr(&alu->instr);
   nir_def *x[NIR_MAX_VEC_COMPONENTS];
   for (unsigned j = 0; j < n; j++)
      x[j] = op == nir_op_bcsel && j == 0 ? nir_ssa_for_alu_src(b, alu, 0)
                                          : src16(b, alu, j);
   nir_def *h = nir_build_alu_src_arr(b, op, x);
   nir_def_replace(&alu->def, nir_f2f32(b, h));
   return true;
}

/* an f32 value d is F16 from here on: its float users read f2f32(f2f16(d)) */
static void
seed16(nir_builder *b, nir_def *d)
{
   b->cursor = nir_after_instr(nir_def_instr(d));
   nir_def *w = nir_f2f32(b, nir_f2f16(b, d));
   nir_def_rewrite_uses_after(d, w);
}

static bool
low_prec(unsigned p)
{
   return p == GLSL_PRECISION_MEDIUM || p == GLSL_PRECISION_LOW;
}

/* the precision a texel has: its sampler's */
static unsigned
tex_prec(nir_tex_instr *t)
{
   int i = nir_tex_instr_src_index(t, nir_tex_src_texture_deref);
   if (i < 0)
      return 0;
   nir_variable *v =
      nir_deref_instr_get_variable(nir_src_as_deref(t->src[i].src));
   return v ? v->data.precision : 0;
}

/* a load or texel that is a float (an int or bool uniform or input, 32 bits
 * as st hands them over, must not go through F16) */
static bool
float_value(nir_instr *instr)
{
   if (instr->type == nir_instr_type_tex)
      return nir_alu_type_get_base_type(nir_instr_as_tex(instr)->dest_type) ==
             nir_type_float;
   nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
   if (in->intrinsic == nir_intrinsic_load_deref) {
      const struct glsl_type *t =
         glsl_without_array_or_matrix(nir_src_as_deref(in->src[0])->type);
      return glsl_get_base_type(t) == GLSL_TYPE_FLOAT ||
             glsl_get_base_type(t) == GLSL_TYPE_FLOAT16;
   }
   return nir_intrinsic_has_dest_type(in) &&
          nir_alu_type_get_base_type(nir_intrinsic_dest_type(in)) ==
             nir_type_float;
}

struct mediump_ctx {
   bool all;
   struct set *coord; /* input variables a texture coordinate reads */
};

/* a load or texel mediump makes F16: f2f16 at its definition */
static bool
mediump_seed_instr(nir_builder *b, nir_instr *instr, void *data)
{
   const struct mediump_ctx *m = data;
   bool progress = false;
   nir_def *d = NULL;
   if (instr->type == nir_instr_type_tex) {
      nir_tex_instr *t = nir_instr_as_tex(instr);
      if ((t->op == nir_texop_tex || t->op == nir_texop_txb ||
           t->op == nir_texop_txl) &&
          (m->all || low_prec(tex_prec(t))))
         d = &t->def;
   } else if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
      if (in->intrinsic == nir_intrinsic_load_deref) {
         nir_variable *v =
            nir_deref_instr_get_variable(nir_src_as_deref(in->src[0]));
         if (v && v->data.mode == nir_var_shader_in &&
             v->data.location >= VARYING_SLOT_VAR0 &&
             (m->all ? !_mesa_set_search(m->coord, v)
                     : low_prec(v->data.precision)))
            d = &in->def;
         else if (v && v->data.mode == nir_var_uniform &&
                  (m->all || low_prec(v->data.precision)))
            d = &in->def;
      } else if ((in->intrinsic == nir_intrinsic_load_interpolated_input ||
                  in->intrinsic == nir_intrinsic_load_input) &&
                 nir_intrinsic_io_semantics(in).location >= VARYING_SLOT_VAR0) {
         nir_io_semantics sem = nir_intrinsic_io_semantics(in);
         if (m->all && !feeds_coord(&in->def, 0) && !sem.medium_precision) {
            sem.medium_precision = 1;
            nir_intrinsic_set_io_semantics(in, sem);
            progress = true;
         }
         if (sem.medium_precision)
            d = &in->def;
      } else if (in->intrinsic == nir_intrinsic_load_uniform && m->all) {
         d = &in->def;
      }
   }
   if (d && d->bit_size == 32 && !list_is_empty(&d->uses) &&
       float_value(instr) &&
       (instr->type != nir_instr_type_tex || !feeds_coord(d, 0))) {
      seed16(b, d);
      progress = true;
   }
   return progress;
}

/* all: fs_mediump, every input, texel and uniform no coordinate needs (and
 * their variables become mediump); otherwise those the program declares
 * mediump or lowp. Ops whose operands are all F16 then run in F16: GLSL ES
 * gives an operation its operands' highest precision. */
static bool
lower_mediump(nir_shader *s, bool all)
{
   struct mediump_ctx m = {.all = all, .coord = _mesa_pointer_set_create(NULL)};
   if (all) {
      /* inputs: a variable none of whose loads is a coordinate is mediump */
      nir_foreach_function_impl (impl, s) {
         nir_foreach_block (blk, impl) {
            nir_foreach_instr (instr, blk) {
               if (instr->type != nir_instr_type_intrinsic)
                  continue;
               nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
               if (in->intrinsic == nir_intrinsic_load_deref) {
                  nir_variable *v =
                     nir_deref_instr_get_variable(nir_src_as_deref(in->src[0]));
                  if (v && v->data.mode == nir_var_shader_in &&
                      feeds_coord(&in->def, 0))
                     _mesa_set_add(m.coord, v);
               }
            }
         }
      }
      nir_foreach_shader_in_variable (var, s)
         if (var->data.location >= VARYING_SLOT_VAR0 &&
             !_mesa_set_search(m.coord, var))
            var->data.precision = GLSL_PRECISION_MEDIUM;
      nir_foreach_variable_with_modes (var, s, nir_var_uniform)
         if (!glsl_type_is_sampler(glsl_without_array(var->type)))
            var->data.precision = GLSL_PRECISION_MEDIUM;
   }
   bool progress = false;
   NIR_PASS(progress, s, nir_shader_instructions_pass, mediump_seed_instr,
            nir_metadata_control_flow, &m);
   _mesa_set_destroy(m.coord, NULL);
   bool p = true;
   for (unsigned round = 0; p && round < 64; round++) {
      p = false;
      NIR_PASS(p, s, nir_shader_alu_pass, demote_instr,
               nir_metadata_control_flow, NULL);
      progress |= p;
   }
   NIR_PASS(progress, s, nir_opt_constant_folding);
   NIR_PASS(progress, s, nir_opt_dce);
   return progress;
}

/* A 16-bit min / max / saturate of values that are f32 underneath (an f2f16,
 * or a dot product, which has no F16 form and is computed in f32): done in
 * f32 and rounded once. Rounding commutes with them, so the result is the
 * same, one conversion fewer (max(dot(n, l), 0.0)). */
static nir_def *
f32_of(nir_builder *b, nir_alu_instr *alu, unsigned j)
{
   unsigned nc = alu->def.num_components;
   nir_alu_instr *p = alu_of(alu->src[j].src);
   if (nir_src_is_const(alu->src[j].src))
      return nir_f2f32(b, nir_ssa_for_alu_src(b, alu, j));
   if (!p || !list_is_singular(&p->def.uses))
      return NULL;
   if ((p->op == nir_op_f2f16 || p->op == nir_op_f2fmp) &&
       nir_src_bit_size(p->src[0].src) == 32) {
      unsigned swz[NIR_MAX_VEC_COMPONENTS];
      for (unsigned k = 0; k < nc; k++)
         swz[k] = p->src[0].swizzle[alu->src[j].swizzle[k]];
      return nir_swizzle(b, p->src[0].src.ssa, swz, nc);
   }
   if ((p->op == nir_op_fdot2 || p->op == nir_op_fdot3 ||
        p->op == nir_op_fdot4) &&
       nc == 1) {
      nir_def *a = nir_f2f32(b, nir_ssa_for_alu_src(b, p, 0));
      nir_def *c = nir_f2f32(b, nir_ssa_for_alu_src(b, p, 1));
      return nir_build_alu2(b, p->op, a, c);
   }
   return NULL;
}

static bool
minmax32_instr(nir_builder *b, nir_alu_instr *alu, void *data)
{
   if ((alu->op != nir_op_fmin && alu->op != nir_op_fmax &&
        alu->op != nir_op_fsat) ||
       alu->def.bit_size != 16)
      return false;
   unsigned n = nir_op_infos[alu->op].num_inputs;
   bool any = false;
   for (unsigned j = 0; j < n; j++) {
      nir_alu_instr *p = alu_of(alu->src[j].src);
      any |= p && !nir_src_is_const(alu->src[j].src);
   }
   if (!any)
      return false;
   b->cursor = nir_before_instr(&alu->instr);
   nir_def *x[2] = {NULL, NULL};
   for (unsigned j = 0; j < n; j++) {
      /* (checked before anything is built) */
      nir_alu_instr *p = alu_of(alu->src[j].src);
      if (!nir_src_is_const(alu->src[j].src) &&
          !(p && list_is_singular(&p->def.uses) &&
            (((p->op == nir_op_f2f16 || p->op == nir_op_f2fmp) &&
              nir_src_bit_size(p->src[0].src) == 32) ||
             ((p->op == nir_op_fdot2 || p->op == nir_op_fdot3 ||
               p->op == nir_op_fdot4) &&
              alu->def.num_components == 1))))
         return false;
   }
   for (unsigned j = 0; j < n; j++)
      x[j] = f32_of(b, alu, j);
   nir_def *r = n == 1 ? nir_build_alu1(b, alu->op, x[0])
                       : nir_build_alu2(b, alu->op, x[0], x[1]);
   nir_def_replace(&alu->def, nir_f2f16(b, r));
   return true;
}

/* ------------------------------------------------------------------------
 * float16 (mediump, from Mesa's precision lowering): comparisons of 16-bit
 * values become comparisons of their f2f32 (the backend reads the F16
 * halves directly as format-select operands), so booleans stay 32-bit */
static bool
widen_cmp16_instr(nir_builder *b, nir_alu_instr *alu, void *data)
{
   /* (float compares only: an integer one is widened as an integer, by
    * int16_to_32) */
   if (!nir_alu_instr_is_comparison(alu) ||
       nir_src_bit_size(alu->src[0].src) != 16 ||
       nir_alu_type_get_base_type(nir_op_infos[alu->op].input_types[0]) !=
          nir_type_float)
      return false;
   b->cursor = nir_before_instr(&alu->instr);
   for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
      nir_def *w = nir_f2f32(b, nir_ssa_for_alu_src(b, alu, i));
      nir_src_rewrite(&alu->src[i].src, w);
      for (unsigned c = 0; c < NIR_MAX_VEC_COMPONENTS; c++)
         alu->src[i].swizzle[c] = c < w->num_components ? c : 0;
   }
   return true;
}

/* 16-bit integer ALU (mediump ints) back to 32-bit: GLES2 ints become floats */
static unsigned
int16_to_32(const nir_instr *instr, void *data)
{
   if (instr->type == nir_instr_type_phi) {
      /* a 16-bit integer phi (a mediump loop counter): an integer op feeds
       * it */
      nir_phi_instr *phi = nir_instr_as_phi((nir_instr *)instr);
      if (phi->def.bit_size != 16)
         return 0;
      nir_foreach_phi_src (src, phi) {
         nir_alu_instr *p = nir_def_as_alu_or_null(src->src.ssa);
         if (p && p->op != nir_op_mov && !nir_op_is_vec(p->op) &&
             p->op != nir_op_bcsel) {
            nir_alu_type t =
               nir_alu_type_get_base_type(nir_op_infos[p->op].output_type);
            if (t == nir_type_int || t == nir_type_uint)
               return 32;
         }
      }
      return 0;
   }
   if (instr->type != nir_instr_type_alu)
      return 0;
   const nir_alu_instr *alu = nir_instr_as_alu(instr);
   const nir_op_info *info = &nir_op_infos[alu->op];
   /* moves, vectors and selects carry data of any type; a conversion from
    * or to 32 bits (f2i16 of an f32) changes the size itself */
   if (alu->op == nir_op_mov || nir_op_is_vec(alu->op) ||
       alu->op == nir_op_bcsel)
      return 0;
   for (unsigned i = 0; i < info->num_inputs; i++)
      if (!nir_alu_type_get_type_size(info->input_types[i]) &&
          nir_src_bit_size(alu->src[i].src) >= 32)
         return 0;
   bool int16 = alu->def.bit_size == 16 &&
                nir_alu_type_get_base_type(info->output_type) != nir_type_float;
   for (unsigned i = 0; i < info->num_inputs; i++)
      int16 |=
         nir_src_bit_size(alu->src[i].src) == 16 &&
         nir_alu_type_get_base_type(info->input_types[i]) != nir_type_float;
   return int16 ? 32 : 0;
}

static bool
vec_cmp_instr(const nir_instr *instr, const void *data)
{
   if (instr->type != nir_instr_type_alu)
      return false;
   switch (nir_instr_as_alu(instr)->op) {
   case nir_op_ball_fequal2:
   case nir_op_ball_fequal3:
   case nir_op_ball_fequal4:
   case nir_op_ball_iequal2:
   case nir_op_ball_iequal3:
   case nir_op_ball_iequal4:
   case nir_op_bany_fnequal2:
   case nir_op_bany_fnequal3:
   case nir_op_bany_fnequal4:
   case nir_op_bany_inequal2:
   case nir_op_bany_inequal3:
   case nir_op_bany_inequal4:
      return true;
   default:
      return false;
   }
}

static bool
vec_to_regs_32(const nir_instr *instr, unsigned wrmask, const void *data)
{
   return nir_instr_as_alu(instr)->def.bit_size == 32;
}

/* ------------------------------------------------------------------------
 * uniforms -> load_uniform with base = the constant number of the first
 * component. Each deref step adds vec4-aligned offsets (array elements,
 * struct members, matrix columns), unless the caller numbered that member
 * itself (-u var.member=k): glslcompiler's layout of struct members isn't
 * known, so it can be pinned member by member. */

static unsigned
slots4(const struct glsl_type *t)
{
   return 4 * glsl_count_vec4_slots(t, false, false);
}

struct unif_ctx {
   struct sgx_ctx *c;
   bool failed;
};

static bool
lower_uniform_instr(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
   struct unif_ctx *u = data;
   if (in->intrinsic != nir_intrinsic_load_deref)
      return false;
   nir_deref_instr *d = nir_src_as_deref(in->src[0]);
   if (!nir_deref_mode_is(d, nir_var_uniform))
      return false;
   nir_deref_path path;
   nir_deref_path_init(&path, d, NULL);
   nir_variable *var = path.path[0]->var;
   if (glsl_type_is_sampler(glsl_without_array(var->type))) {
      nir_deref_path_finish(&path);
      return false;
   }
   char name[256];
   snprintf(name, sizeof name, "%s", var->name);
   unsigned k = var->data.driver_location;
   nir_def *dyn = NULL; /* a dynamic offset in components (one level) */
   unsigned dyn_range = 0;
   b->cursor = nir_before_instr(&in->instr);
   unsigned prec = var->data.precision;
   const struct glsl_type *t = var->type;
   for (unsigned i = 1; path.path[i]; i++) {
      nir_deref_instr *p = path.path[i];
      if (p->deref_type == nir_deref_type_struct) {
         prec = glsl_get_struct_field_data(t, p->strct.index)->precision;
         for (unsigned f = 0; f < p->strct.index; f++)
            k += slots4(glsl_get_struct_field(t, f));
         size_t l = strlen(name);
         snprintf(name + l, sizeof name - l, ".%s",
                  glsl_get_struct_elem_name(t, p->strct.index));
         int pinned = place(u->c->opts, SGX_LAYOUT_UNIFORM, name);
         if (pinned >= 0)
            k = pinned;
         t = glsl_get_struct_field(t, p->strct.index);
      } else if (p->deref_type == nir_deref_type_array &&
                 nir_src_is_const(p->arr.index)) {
         const struct glsl_type *e = glsl_type_is_matrix(t)
                                        ? glsl_get_column_type(t)
                                        : glsl_get_array_element(t);
         k += nir_src_as_uint(p->arr.index) * slots4(e);
         size_t l = strlen(name);
         snprintf(name + l, sizeof name - l, "[%u]",
                  (unsigned)nir_src_as_uint(p->arr.index));
         int pinned = place(u->c->opts, SGX_LAYOUT_UNIFORM, name);
         if (pinned >= 0)
            k = pinned;
         t = e;
      } else if (p->deref_type == nir_deref_type_array && !dyn) {
         /* a[i]: the offset is computed at run time; the whole array is
          * laid out (f32, contiguous) and read through an index register */
         const struct glsl_type *e = glsl_type_is_matrix(t)
                                        ? glsl_get_column_type(t)
                                        : glsl_get_array_element(t);
         unsigned len = glsl_type_is_matrix(t) ? glsl_get_matrix_columns(t)
                                               : glsl_get_length(t);
         nir_def *i = nir_u2u32(b, p->arr.index.ssa);
         dyn = nir_imul_imm(b, i, slots4(e));
         dyn_range = len * slots4(e);
         t = e;
      } else {
         u->failed = true;
         nir_deref_path_finish(&path);
         return false;
      }
   }
   nir_deref_path_finish(&path);
   if (dyn) {
      unsigned n = in->def.num_components;
      for (unsigned j = 0; j < dyn_range && k + j < ARRAY_SIZE(u->c->const_fmt);
           j++)
         u->c->const_fmt[k + j] = 1;
      nir_def *v =
         nir_load_uniform(b, n, 32, dyn, .base = k, .range = dyn_range,
                          .dest_type = nir_type_float32);
      if (in->def.bit_size == 16)
         v = nir_f2f16(b, v);
      nir_def_replace(&in->def, v);
      return true;
   }
   /* the constants' format in SA, as the DDK loads them: F16 pairs for
    * mediump (16-bit loads), C10 for lowp vectors used as F16, else f32 */
   unsigned n = in->def.num_components, fmt = in->def.bit_size == 16 ? 2 : 1;
   if (u->c->opts->verbose)
      fprintf(stderr, "uniform %s: constant %u, %u x %u bits, precision %u\n",
              name, k, n, in->def.bit_size, prec);
   if (fmt == 1 && u->c->p16 && prec == GLSL_PRECISION_LOW && !(k & 3) &&
       !u->c->opts->driver_consts) {
      /* every use an f2f16, and what those are read for at most xyz */
      bool all16 = true;
      unsigned read = 0;
      nir_foreach_use_including_if (use, &in->def) {
         nir_instr *ui = nir_src_is_if(use) ? NULL : nir_src_use_instr(use);
         if (!ui || ui->type != nir_instr_type_alu ||
             (nir_instr_as_alu(ui)->op != nir_op_f2f16 &&
              nir_instr_as_alu(ui)->op != nir_op_f2fmp)) {
            all16 = false;
            break;
         }
         nir_alu_instr *cv = nir_instr_as_alu(ui);
         unsigned r16 = nir_def_components_read(&cv->def);
         for (unsigned j = 0; j < cv->def.num_components; j++)
            if (r16 >> j & 1)
               read |= 1u << cv->src[0].swizzle[j];
      }
      if (all16 && !(read & ~7u))
         fmt = 3;
   }
   if (fmt == 2 && u->c->p16 && prec == GLSL_PRECISION_LOW && !(k & 3) &&
       n >= 3 && !(nir_def_components_read(&in->def) & ~7u)) {
      /* a lowp vector loaded 16-bit (sgxc's frontend) and read for xyz: C10
       * as the DDK loads it, then to F16 where read; or with driver_consts
       * the F16 of its C10 value from the upload */
      fmt = u->c->opts->driver_consts ? 5 : 3;
   }
   for (unsigned j = 0; j < n && k + j < ARRAY_SIZE(u->c->const_fmt); j++)
      u->c->const_fmt[k + j] = fmt == 3 && j == 3   ? 1
                               : fmt == 5 && j == 3 ? 2
                                                    : fmt;
   for (unsigned j = 0; j < n && k + j < ARRAY_SIZE(u->c->const_prec); j++)
      u->c->const_prec[k + j] = prec;
   if (fmt == 2 && prec == GLSL_PRECISION_LOW && n == 4 && !(k & 3) &&
       k + 3 < ARRAY_SIZE(u->c->const_lowp))
      u->c->const_lowp[k] = 1;
   /* (a bool is a 32-bit value, true when nonzero) */
   unsigned bits = in->def.bit_size == 1 ? 32 : in->def.bit_size;
   nir_def *v = nir_load_uniform(
      b, n, bits, nir_imm_int(b, 0), .base = k, .range = n,
      .dest_type = bits == 16 ? nir_type_float16 : nir_type_float32);
   if (in->def.bit_size == 1)
      v = nir_ine_imm(b, v, 0);
   nir_def_replace(&in->def, v);
   return true;
}

static bool
lower_uniforms(nir_shader *s, struct sgx_ctx *c)
{
   struct unif_ctx u = {c, false};
   bool p = nir_shader_intrinsics_pass(s, lower_uniform_instr,
                                       nir_metadata_control_flow, &u);
   if (u.failed)
      sgx_fail(c, "indirectly indexed uniform (not supported yet)");
   return p;
}

/* the precision of the uniform component at comp, from the variables (for a
 * driver that hands over load_uniform already lowered; assign_uniforms has
 * numbered them in components, the numbering such a driver's loads use) */
static unsigned
type_prec_at(const struct glsl_type *t, unsigned prec, unsigned comp)
{
   if (glsl_type_is_struct(t)) {
      for (unsigned f = 0; f < glsl_get_length(t); f++) {
         const struct glsl_type *ft = glsl_get_struct_field(t, f);
         unsigned n = 4 * glsl_count_vec4_slots(ft, false, false);
         if (comp < n) {
            unsigned fp = glsl_get_struct_field_data(t, f)->precision;
            return type_prec_at(ft, fp ? fp : prec, comp);
         }
         comp -= n;
      }
      return 0;
   }
   if (glsl_type_is_array(t)) {
      const struct glsl_type *e = glsl_get_array_element(t);
      unsigned n = 4 * glsl_count_vec4_slots(e, false, false);
      return n ? type_prec_at(e, prec, comp % n) : 0;
   }
   return prec;
}

static unsigned
uniform_prec(struct sgx_ctx *c, unsigned k)
{
   if (c->opts->fs_mediump && c->nir->info.stage == MESA_SHADER_FRAGMENT)
      return GLSL_PRECISION_MEDIUM;
   if (k < ARRAY_SIZE(c->const_prec) && c->const_prec[k])
      return c->const_prec[k];
   nir_foreach_variable_with_modes (var, c->nir, nir_var_uniform) {
      if (glsl_type_is_sampler(glsl_without_array(var->type)) ||
          var->data.driver_location < 0)
         continue;
      unsigned base = var->data.driver_location,
               n = 4 * glsl_count_vec4_slots(var->type, false, false);
      if (k >= base && k < base + n)
         return type_prec_at(var->type, var->data.precision, k - base);
   }
   return 0;
}

/* f32 uniforms that F16 math reads (every use an f2f16), in the format the
 * DDK loads such a constant in: mediump as F16 constants (list C kind 2,
 * rounded at upload), a lowp vector's xyz as C10 (kind 3), highp left f32
 * (converted where read with pck.f16.f32, truncating as libusc's own
 * conversion does). sgxc's frontend loads mediump uniforms 16-bit already;
 * the gallium driver's arrive as f32 load_uniform. */
static bool
uniform_format_instr(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
   struct sgx_ctx *c = data;
   bool no_const = sgx_debug(c, SGX_DBG_NO_F16_CONST);
   if (in->intrinsic == nir_intrinsic_load_uniform && in->def.bit_size == 16 &&
       no_const) {
      /* (nof16const: an f32 load converted in the shader) */
      b->cursor = nir_before_instr(&in->instr);
      nir_def *v = nir_load_uniform(
         b, in->def.num_components, 32, in->src[0].ssa,
         .base = nir_intrinsic_base(in), .range = nir_intrinsic_range(in),
         .dest_type = nir_type_float32);
      unsigned k = nir_intrinsic_base(in);
      unsigned range = nir_src_is_const(in->src[0]) ? in->def.num_components
                                                    : nir_intrinsic_range(in);
      for (unsigned j = 0; j < range && k + j < ARRAY_SIZE(c->const_fmt); j++)
         c->const_fmt[k + j] = 1;
      nir_def_replace(&in->def, nir_f2f16(b, v));
      return true;
   }
   if (in->intrinsic == nir_intrinsic_load_uniform && in->def.bit_size == 16) {
      /* a 16-bit load the frontend made (a driver with 16-bit loads):
       * F16 constants */
      unsigned k = nir_intrinsic_base(in);
      unsigned range = nir_src_is_const(in->src[0]) ? in->def.num_components
                                                    : nir_intrinsic_range(in);
      for (unsigned j = 0; j < range && k + j < ARRAY_SIZE(c->const_fmt); j++)
         if (!c->const_fmt[k + j])
            c->const_fmt[k + j] = 2;
      return false;
   }
   if (no_const || in->intrinsic != nir_intrinsic_load_uniform ||
       in->def.bit_size != 32 || !nir_src_is_const(in->src[0]) ||
       nir_src_as_uint(in->src[0]) != 0 ||
       nir_alu_type_get_base_type(nir_intrinsic_dest_type(in)) !=
          nir_type_float)
      return false;
   unsigned k = nir_intrinsic_base(in), n = in->def.num_components;
   if (k + n > ARRAY_SIZE(c->const_fmt))
      return false;
   bool ok = true;
   unsigned read = 0;
   for (unsigned j = 0; j < n; j++)
      ok &= c->const_fmt[k + j] <= 1;
   nir_foreach_use_including_if (use, &in->def) {
      nir_instr *u = nir_src_is_if(use) ? NULL : nir_src_use_instr(use);
      if (!u || u->type != nir_instr_type_alu ||
          (nir_instr_as_alu(u)->op != nir_op_f2f16 &&
           nir_instr_as_alu(u)->op != nir_op_f2fmp)) {
         ok = false;
         break;
      }
      nir_alu_instr *cv = nir_instr_as_alu(u);
      unsigned r16 = nir_def_components_read(&cv->def);
      for (unsigned j = 0; j < cv->def.num_components; j++)
         if (r16 >> j & 1)
            read |= 1u << cv->src[0].swizzle[j];
   }
   if (!ok)
      return false;
   unsigned prec = uniform_prec(c, k);
   bool c10 = prec == GLSL_PRECISION_LOW && !(k & 3) && !(read & ~7u);
   if (c10 && !c->opts->driver_consts) {
      for (unsigned j = 0; j < n; j++)
         c->const_fmt[k + j] = j == 3 ? 1 : 3;
      return false;
   }
   if (prec != GLSL_PRECISION_MEDIUM && prec != GLSL_PRECISION_LOW)
      return false;
   /* F16 constants; a C10 vector's xyz as the F16 of its C10 value (the
    * driver converts at upload what libusc's secondary program does) */
   b->cursor = nir_after_instr(&in->instr);
   nir_def *h = nir_load_uniform(b, n, 16, in->src[0].ssa, .base = k,
                                 .range = n, .dest_type = nir_type_float16);
   nir_foreach_use_safe (use, &in->def) {
      nir_alu_instr *cv = nir_instr_as_alu(nir_src_use_instr(use));
      b->cursor = nir_before_instr(&cv->instr);
      unsigned swz[NIR_MAX_VEC_COMPONENTS];
      for (unsigned j = 0; j < cv->def.num_components; j++)
         swz[j] = cv->src[0].swizzle[j];
      nir_def_rewrite_uses(&cv->def,
                           nir_swizzle(b, h, swz, cv->def.num_components));
   }
   for (unsigned j = 0; j < n; j++)
      c->const_fmt[k + j] = c10 && j < 3 ? 5 : 2;
   /* a whole lowp vec4: a C10 expression may read it (find_c10) */
   if (prec == GLSL_PRECISION_LOW && n == 4 && !(k & 3))
      c->const_lowp[k] = 1;
   return true;
}

static bool
uniform_formats(nir_shader *s, struct sgx_ctx *c)
{
   return nir_shader_intrinsics_pass(s, uniform_format_instr,
                                     nir_metadata_control_flow, c);
}

/* ------------------------------------------------------------------------ */

static unsigned
type_size_vec4(const struct glsl_type *type, bool bindless)
{
   return glsl_count_vec4_slots(type, false, bindless);
}

/* uniforms: offsets in components, every vec4 slot 4 components */
static unsigned
type_size_comps(const struct glsl_type *type, bool bindless)
{
   return 4 * glsl_count_vec4_slots(type, false, bindless);
}

/* the caller's layout (sgx_compile_options.layout), if it has one */
static int
place(const struct sgx_compile_options *o, enum sgx_layout_kind k,
      const char *name)
{
   return o->layout && o->layout->place
             ? o->layout->place(o->layout->data, k, name)
             : -1;
}

static unsigned
first_free(const struct sgx_compile_options *o, enum sgx_layout_kind k)
{
   return o->layout && o->layout->first_free
             ? o->layout->first_free(o->layout->data, k)
             : 0;
}

/* A loop that can leave early (a break besides its trip count, or one the
 * frontend merged into it, so the trip count isn't exact) stays a loop, as
 * libusc keeps it: unrolled, its exits become a select tree that holds every
 * iteration's value to the end (corpus loop_v: 31 temporaries, 4 as a loop,
 * libusc 9). Loops that index arrays are still unrolled. */
static bool
keep_early_exit_loops_cf(struct exec_list *list)
{
   bool progress = false;
   foreach_list_typed (nir_cf_node, node, node, list) {
      switch (node->type) {
      case nir_cf_node_if: {
         nir_if *nif = nir_cf_node_as_if(node);
         progress |= keep_early_exit_loops_cf(&nif->then_list);
         progress |= keep_early_exit_loops_cf(&nif->else_list);
         break;
      }
      case nir_cf_node_loop: {
         nir_loop *loop = nir_cf_node_as_loop(node);
         progress |= keep_early_exit_loops_cf(&loop->body);
         if (loop->control == nir_loop_control_none && loop->info &&
             !loop->info->force_unroll &&
             (list_length(&loop->info->loop_terminator_list) > 1 ||
              !loop->info->exact_trip_count_known)) {
            loop->control = nir_loop_control_dont_unroll;
            progress = true;
         }
         break;
      }
      default:
         break;
      }
   }
   return progress;
}

static bool
keep_early_exit_loops(nir_shader *s)
{
   bool progress = false;
   nir_foreach_function_impl (impl, s) {
      nir_metadata_require(impl, nir_metadata_loop_analysis,
                           s->options->force_indirect_unrolling,
                           (int)s->options->force_indirect_unrolling_sampler);
      progress |= keep_early_exit_loops_cf(&impl->body);
      nir_progress(true, impl, nir_metadata_all);
   }
   return progress;
}

static void
optimize(nir_shader *s)
{
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, s, nir_lower_vars_to_ssa);
      NIR_PASS(progress, s, nir_opt_copy_prop_vars);
      NIR_PASS(progress, s, nir_opt_dead_write_vars);
      NIR_PASS(progress, s, nir_opt_copy_prop);
      NIR_PASS(progress, s, nir_opt_remove_phis);
      NIR_PASS(progress, s, nir_opt_dce);
      NIR_PASS(progress, s, nir_opt_dead_cf);
      NIR_PASS(progress, s, nir_opt_cse);
      /* components nothing reads (mat4 * v of which only .z is used); not a
       * load's leading ones: their offset would be added in bytes, ours count
       * components */
      NIR_PASS(progress, s, nir_opt_shrink_vectors, false);
      NIR_PASS(progress, s, nir_opt_peephole_select,
               &(nir_opt_peephole_select_options){.limit = 200,
                                                  .indirect_load_ok = true,
                                                  .expensive_alu_ok = true,
                                                  .discard_ok = true});
      NIR_PASS(progress, s, nir_opt_algebraic);
      NIR_PASS(progress, s, nir_opt_constant_folding);
      NIR_PASS(progress, s, nir_opt_undef);
      NIR_PASS(progress, s, nir_opt_loop_unroll);
      NIR_PASS(progress, s, nir_opt_if, nir_opt_if_optimize_phi_true_false);
   } while (progress);
}

static void
optimize_late(nir_shader *s)
{
   bool progress;
   do {
      progress = false;
      NIR_PASS(progress, s, nir_opt_algebraic_late);
      if (progress) {
         NIR_PASS(_, s, nir_opt_constant_folding);
         NIR_PASS(_, s, nir_opt_copy_prop);
         NIR_PASS(_, s, nir_opt_dce);
         NIR_PASS(_, s, nir_opt_cse);
      }
   } while (progress);
}

/* the varyings a (possibly linker-packed) variable holds: "packed:a,b" */
static unsigned
var_names(nir_variable *var, char names[][64], unsigned max)
{
   const char *n = var->name;
   unsigned k = 0;
   if (!strncmp(n, "packed:", 7))
      n += 7;
   while (*n && k < max) {
      const char *e = strchr(n, ',');
      size_t len = e ? (size_t)(e - n) : strlen(n);
      snprintf(names[k++], 64, "%.*s", (int)MIN2(len, 63), n);
      n += len + (e ? 1 : 0);
   }
   return k;
}

static const struct sgx_varying *
find_varying(const struct sgx_varyings *v, const char *name)
{
   for (unsigned i = 0; i < v->n; i++)
      if (!strcmp(v->v[i].name, name))
         return &v->v[i];
   return NULL;
}

/* per component of a varying variable, the register: base[set] + the
 * varying's component in the set */
static void
map_varying(int *tab, nir_variable *var, const struct sgx_varyings *vary,
            const int *set_base)
{
   char names[8][64];
   unsigned n = var_names(var, names, 8);
   unsigned comp = var->data.location_frac;
   for (unsigned i = 0; i < n; i++) {
      const struct sgx_varying *vy = find_varying(vary, names[i]);
      if (!vy)
         continue;
      for (unsigned r = 0; r < vy->rows; r++) {
         if (vy->set + r >= SGX_MAX_SETS || set_base[vy->set + r] < 0)
            continue;
         for (unsigned j = 0; j < vy->comps && comp + j < 4; j++) {
            unsigned at = (var->data.location + r) * 4 + comp + j;
            if (at < 512)
               tab[at] = set_base[vy->set + r] + vy->comp + j;
         }
      }
      comp += vy->rows == 1 ? vy->comps : 0;
   }
}

/* Assign I/O registers (c->in_reg / c->out_reg, per component). */
static void
assign_io(struct sgx_ctx *c)
{
   nir_shader *s = c->nir;
   const struct sgx_compile_options *o = c->opts;

   for (unsigned i = 0; i < 512; i++)
      c->in_reg[i] = c->out_reg[i] = -1;
   if (s->info.stage == MESA_SHADER_VERTEX) {
      unsigned next_attr = first_free(o, SGX_LAYOUT_ATTRIB);
      nir_foreach_shader_in_variable (var, s) {
         int a = place(o, SGX_LAYOUT_ATTRIB, var->name);
         unsigned rows = type_size_vec4(var->type, false);
         /* st splits a matrix attribute into one variable per column, all
          * with the matrix's name: a column goes after the earlier ones */
         if (a >= 0 && var->name) {
            nir_foreach_shader_in_variable (prev, s) {
               if (prev == var)
                  break;
               if (prev->name && !strcmp(prev->name, var->name))
                  a += type_size_vec4(prev->type, false);
            }
         }
         if (a < 0) {
            a = next_attr;
            next_attr += rows;
         }
         for (unsigned r = 0; r < rows; r++)
            for (unsigned j = 0; j < 4; j++)
               if ((var->data.location + r) * 4 + j < 512)
                  c->in_reg[(var->data.location + r) * 4 + j] = 4 * (a + r) + j;
      }
      /* outputs: o0-o3 position, then the sets, then the point size */
      int obase[SGX_MAX_SETS];
      unsigned k = 4;
      for (unsigned i = 0; i < SGX_MAX_SETS; i++) {
         obase[i] = c->vary->set_comps[i] ? (int)k : -1;
         k += c->vary->set_comps[i];
      }
      c->psize_reg = k;
      c->n_out_regs = k + (c->vary->point_size ? 1 : 0);
      for (unsigned j = 0; j < 4; j++)
         c->out_reg[VARYING_SLOT_POS * 4 + j] = j;
      if (c->vary->point_size)
         c->out_reg[VARYING_SLOT_PSIZ * 4] = c->psize_reg;
      nir_foreach_shader_out_variable (var, s)
         if (var->data.location >= VARYING_SLOT_VAR0)
            map_varying(c->out_reg, var, c->vary, obase);
   } else {
      /* in F16 mode, a varying every load of which is mediump is iterated as
       * F16 (list A format 2: two components per register, as the DDK does) */
      bool hp[128] = {0}, mp[128] = {0};
      nir_foreach_function_impl (impl, s) {
         nir_foreach_block (b, impl) {
            nir_foreach_instr (instr, b) {
               /* (sgxc's inputs are interpolated loads; the gallium driver's
                * are still derefs here, before nir_lower_io: their
                * variable's precision says) */
               if (instr->type == nir_instr_type_intrinsic &&
                   nir_instr_as_intrinsic(instr)->intrinsic ==
                      nir_intrinsic_load_deref) {
                  nir_deref_instr *d =
                     nir_src_as_deref(nir_instr_as_intrinsic(instr)->src[0]);
                  nir_variable *v = nir_deref_instr_get_variable(d);

                  if (v && v->data.mode == nir_var_shader_in &&
                      v->data.location >= 0 && v->data.location < 128)
                     ((v->data.precision == GLSL_PRECISION_MEDIUM ||
                       v->data.precision == GLSL_PRECISION_LOW)
                         ? mp
                         : hp)[v->data.location] = true;
                  continue;
               }
               if (instr->type != nir_instr_type_intrinsic ||
                   (nir_instr_as_intrinsic(instr)->intrinsic !=
                       nir_intrinsic_load_interpolated_input &&
                    nir_instr_as_intrinsic(instr)->intrinsic !=
                       nir_intrinsic_load_input))
                  continue;
               nir_io_semantics sem =
                  nir_intrinsic_io_semantics(nir_instr_as_intrinsic(instr));
               if (sem.location < 128)
                  (sem.medium_precision ? mp : hp)[sem.location] = true;
            }
         }
      }
      /* (a set is F16 when every varying in it is) */
      bool set16[SGX_MAX_SETS] = {0}, not16[SGX_MAX_SETS] = {0};
      /* (nof16iter: every varying iterated in F32, F16 math kept) */
      if (c->p16 && !sgx_debug(c, SGX_DBG_NO_F16_ITER)) {
         nir_foreach_shader_in_variable (var, s) {
            char names[8][64];
            unsigned l = var->data.location, n = var_names(var, names, 8);
            if (l < VARYING_SLOT_VAR0 || l >= 128)
               continue;
            for (unsigned i = 0; i < n; i++) {
               const struct sgx_varying *vy = find_varying(c->vary, names[i]);
               if (!vy)
                  continue;
               bool ok = mp[l] && !hp[l] && n == 1 && vy->rows == 1;
               for (unsigned r = 0; r < vy->rows && vy->set + r < SGX_MAX_SETS;
                    r++)
                  (ok ? set16 : not16)[vy->set + r] = true;
            }
         }
         for (unsigned set = 0; set < SGX_MAX_SETS; set++)
            set16[set] &= !not16[set];
      }
      /* gl_FragCoord first (list A iterator 14, F32, as the DDK orders it),
       * then the sets in order; gl_PointCoord is set 0 */
      int pbase[SGX_MAX_SETS];
      unsigned pa = 0;
      bool fragcoord = false, pointcoord = false;
      nir_foreach_shader_in_variable (var, s) {
         fragcoord |= var->data.location == VARYING_SLOT_POS;
         pointcoord |= var->data.location == VARYING_SLOT_PNTC;
      }
      if (fragcoord) {
         struct sgx_iter *it = &c->iters[c->n_iters++];
         it->set = SGX_ITER_FRAGCOORD - 1;
         it->comps = 4;
         it->pa = pa;
         for (unsigned j = 0; j < 4; j++)
            c->in_reg[VARYING_SLOT_POS * 4 + j] = pa + j;
         pa += 4;
      }
      for (unsigned set = 0; set < SGX_MAX_SETS; set++) {
         pbase[set] = -1;
         if (set == 0 && pointcoord && c->vary->point_size) {
            struct sgx_iter *it = &c->iters[c->n_iters++];
            it->set = 0;
            it->comps = 2;
            it->pa = pa;
            it->f16 = c->p16;
            it->pointcoord = true;
            for (unsigned j = 0; j < 2; j++)
               c->in_reg[VARYING_SLOT_PNTC * 4 + j] =
                  it->f16 ? (int)(SGX_IN_HALF | (2 * pa + j)) : (int)(pa + j);
            pa += it->f16 ? 1 : 2;
            continue;
         }
         /* one iteration per set, of every varying in it */
         unsigned comps = 0;
         for (unsigned v = 0; v < c->vary->n; v++) {
            const struct sgx_varying *vy = &c->vary->v[v];
            if (set >= vy->set && set < vy->set + vy->rows)
               comps = MAX2(comps, vy->comp + vy->comps);
         }
         if (!comps)
            continue;
         struct sgx_iter *it = &c->iters[c->n_iters++];
         it->set = set;
         it->comps = MIN2(comps, 4);
         it->pa = pa;
         it->f16 = set16[set];
         /* F16: in_reg holds SGX_IN_HALF | the half index */
         pbase[set] = it->f16 ? (int)(SGX_IN_HALF | 2 * pa) : (int)pa;
         pa += it->f16 ? (it->comps + 1) / 2 : it->comps;
      }
      c->n_pa_inputs = pa;
      nir_foreach_shader_in_variable (var, s)
         if (var->data.location >= VARYING_SLOT_VAR0)
            map_varying(c->in_reg, var, c->vary, pbase);
   }

   /* uniforms: constant numbers */
   unsigned next = ALIGN_POT(first_free(o, SGX_LAYOUT_UNIFORM), 4);
   unsigned next_sampler = first_free(o, SGX_LAYOUT_SAMPLER);
   nir_foreach_uniform_variable (var, s) {
      if (glsl_type_is_sampler(glsl_without_array(var->type))) {
         /* the USP's sampler number: glslcompiler counts the samplers a
          * shader uses (corpus c3 tx_nd_s1: s1 alone is sampler 0) */
         int u = place(o, SGX_LAYOUT_SAMPLER, var->name);
         var->data.binding = u >= 0 ? (unsigned)u : next_sampler++;
         continue;
      }
      int k = place(o, SGX_LAYOUT_UNIFORM, var->name);
      if (k < 0) {
         /* an array or matrix pinned element by element: its base */
         char e0[80];
         snprintf(e0, sizeof e0, "%s[0]", var->name);
         k = place(o, SGX_LAYOUT_UNIFORM, e0);
      }
      if (k < 0) {
         k = next;
         next += ALIGN_POT(type_size_comps(var->type, false), 4);
      }
      var->data.driver_location = k;
   }
}

/* ior source s is x & mask with a constant mask: the iand and x's index */
static nir_alu_instr *
masked(nir_alu_instr *ior, unsigned s, uint32_t mask, unsigned *xi)
{
   nir_alu_instr *a = nir_src_as_alu(ior->src[s].src);
   if (!a || a->op != nir_op_iand)
      return NULL;
   for (unsigned j = 0; j < 2; j++) {
      nir_alu_src *m = &a->src[j];
      if (!nir_src_is_const(m->src))
         continue;
      bool all = true;
      for (unsigned k = 0; k < a->def.num_components; k++)
         all &= nir_src_comp_as_uint(m->src, m->swizzle[k]) == mask;
      if (all) {
         *xi = 1 - j;
         return a;
      }
   }
   return NULL;
}

/* the float x of ior source s = x & mask, in the ior's components */
static nir_def *
masked_x(nir_builder *b, nir_alu_instr *ior, unsigned s, nir_alu_instr *a,
         unsigned xi)
{
   unsigned sw[NIR_MAX_VEC_COMPONENTS];
   for (unsigned k = 0; k < ior->def.num_components; k++)
      sw[k] = a->src[xi].swizzle[ior->src[s].swizzle[k]];
   return nir_swizzle(b, a->src[xi].src.ssa, sw, ior->def.num_components);
}

/* copysign as the GLSL frontend builds it for a driver with integers
 * ((a & 0x7fffffff) | (b & 0x80000000), atan's result; 0x7fff / 0x8000 in
 * mediump): the float form
 * nir_copysign uses without them */
static bool
copysign_instr(nir_builder *b, nir_alu_instr *alu, void *data)
{
   unsigned bits = alu->def.bit_size;
   if (alu->op != nir_op_ior || (bits != 32 && bits != 16))
      return false;
   uint32_t sign = 1u << (bits - 1);
   for (unsigned s = 0; s < 2; s++) {
      unsigned mi, si;
      nir_alu_instr *m = masked(alu, s, sign - 1, &mi),
                    *g = masked(alu, 1 - s, sign, &si);
      if (!m || !g)
         continue;
      b->cursor = nir_before_instr(&alu->instr);
      nir_def *abs = nir_fabs(b, masked_x(b, alu, s, m, mi));
      nir_def *neg = nir_flt_imm(b, masked_x(b, alu, 1 - s, g, si), 0.0);
      nir_def_replace(&alu->def, nir_bcsel(b, neg, nir_fneg(b, abs), abs));
      return true;
   }
   return false;
}

void
sgx_lower(struct sgx_ctx *c)
{
   nir_shader *s = c->nir;

   s->options = &options;
   NIR_PASS(_, s, nir_shader_alu_pass, copysign_instr,
            nir_metadata_control_flow, NULL);
   if (c->p16 && s->info.stage == MESA_SHADER_FRAGMENT &&
       (c->opts->fs_mediump || !sgx_debug(c, SGX_DBG_NO_MEDIUMP)))
      NIR_PASS(_, s, lower_mediump, c->opts->fs_mediump);
   assign_io(c);
   NIR_PASS(_, s, nir_lower_io_vars_to_temporaries,
            nir_shader_get_entrypoint(s), nir_var_shader_out);
   NIR_PASS(_, s, nir_lower_global_vars_to_local);
   NIR_PASS(_, s, nir_split_var_copies);
   NIR_PASS(_, s, nir_lower_var_copies);
   NIR_PASS(_, s, nir_lower_vars_to_ssa);
   if (!sgx_debug(c, SGX_DBG_UNROLL_EXITS))
      NIR_PASS(_, s, keep_early_exit_loops);
   optimize(s);
   if (s->info.stage == MESA_SHADER_FRAGMENT &&
       nir_shader_intrinsics_pass(s, colour_sat_instr,
                                  nir_metadata_control_flow, NULL))
      optimize(s);
   /* indexing of temporaries that unrolling didn't remove (a loop bounded by
    * a uniform): if / else trees over the elements */
   bool ind = false;
   NIR_PASS(ind, s, nir_lower_indirect_derefs_to_if_else_trees,
            nir_var_function_temp, UINT32_MAX);
   if (ind) {
      NIR_PASS(_, s, nir_lower_vars_to_ssa);
      optimize(s);
   }
   /* (Mesa's GLSL linker hands over I/O already lowered: keep the
    * variables, they name the varyings) */
   NIR_PASS(_, s, nir_remove_dead_variables, nir_var_function_temp, NULL);
   NIR_PASS(_, s, nir_lower_flrp, 32, false);
   NIR_PASS(_, s, lower_tex_derefs, c);
   if (s->info.stage == MESA_SHADER_FRAGMENT && !c->opts->front_sa)
      NIR_PASS(_, s, nir_shader_intrinsics_pass, front_true_instr,
               nir_metadata_control_flow, NULL);

   NIR_PASS(_, s, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            type_size_vec4, 0);
   NIR_PASS(_, s, lower_uniforms, c);
   if (c->p16 && s->info.stage == MESA_SHADER_FRAGMENT) {
      /* a mediump read of a texel: the texture's result in F16 (the
       * finalizer's unpack, pck.f16.u8), not f32 then converted */
      nir_opt_16bit_tex_image_options t16 = {
         .rounding_mode = nir_rounding_mode_undef,
         .opt_tex_dest_types = nir_type_float,
      };
      NIR_PASS(_, s, nir_opt_16bit_tex_image, &t16);
      if (!sgx_debug(c, SGX_DBG_NO_NARROW16))
         NIR_PASS(_, s, narrow16);
      NIR_PASS(_, s, uniform_formats, c);
   }
   NIR_PASS(_, s, nir_opt_dce);
   NIR_PASS(_, s, lower_sincos);
   optimize(s);

   optimize_late(s);

   if (c->p16 && !sgx_debug(c, SGX_DBG_NO_MINMAX32)) {
      NIR_PASS(_, s, nir_shader_alu_pass, minmax32_instr,
               nir_metadata_control_flow, NULL);
      NIR_PASS(_, s, nir_opt_dce);
   }
   /* all() / any() of vector compares (dEQP's isOk): scalar compares and
    * iand / ior, which the bool lowering takes */
   NIR_PASS(_, s, nir_lower_alu_to_scalar, vec_cmp_instr, NULL);
   NIR_PASS(_, s, nir_shader_alu_pass, widen_cmp16_instr,
            nir_metadata_control_flow, NULL);
   NIR_PASS(_, s, nir_lower_bit_size, int16_to_32, NULL);
   NIR_PASS(_, s, nir_opt_constant_folding);
   NIR_PASS(_, s, nir_opt_dce);
   NIR_PASS(_, s, nir_shader_alu_pass, int16_conv_instr,
            nir_metadata_control_flow, NULL);
   NIR_PASS(_, s, nir_shader_alu_pass, i2f16_instr, nir_metadata_control_flow,
            NULL);
   NIR_PASS(_, s, nir_opt_dce);
   NIR_PASS(_, s, nir_lower_int_to_float);
   NIR_PASS(_, s, nir_opt_copy_prop);
   /* (a 16-bit bcsel would become fcsel, an f32-only opcode: b32csel takes
    * any size, on the condition as a float) */
   NIR_PASS(_, s, nir_shader_alu_pass, csel16_instr, nir_metadata_control_flow,
            NULL);
   NIR_PASS(_, s, nir_lower_bool_to_float, true);
   NIR_PASS(_, s, nir_opt_copy_prop);
   NIR_PASS(_, s, nir_opt_constant_folding);
   NIR_PASS(_, s, nir_opt_cse);
   NIR_PASS(_, s, nir_opt_dce);

   NIR_PASS(_, s, nir_convert_from_ssa, true, false);
   /* 16-bit vectors stay vecN: the backend packs F16 halves itself */
   NIR_PASS(_, s, nir_lower_vec_to_regs, vec_to_regs_32, NULL);
   NIR_PASS(_, s, nir_opt_dce);
   NIR_PASS(_, s, nir_legacy_trivialize, true);
   nir_index_ssa_defs(nir_shader_get_entrypoint(s));
}
