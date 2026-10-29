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

static int lookup(const struct sgx_named *t, unsigned n, const char *name);

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
   .float_mul_add32 = nir_float_muladd_support_has_fmad | nir_float_muladd_support_fuse,
   .no_integers = true,
   .max_unroll_iterations = 8,   /* (libusc unrolls nothing; a 16-trip loop with a break became 125 instructions) */
   .lower_int64_options = ~0,
   .lower_doubles_options = ~0,
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
 * (FUN_00029ff0, ISA.md "Varyings to sets"): by type mat4, mat2, vec4,
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
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            switch (in->intrinsic) {
            case nir_intrinsic_store_deref:
            case nir_intrinsic_load_deref:
            case nir_intrinsic_interp_deref_at_centroid:
               if ((in->intrinsic == nir_intrinsic_store_deref) == store &&
                   nir_deref_instr_get_variable(nir_src_as_deref(in->src[0])) == var)
                  return true;
               break;
            case nir_intrinsic_copy_deref:
               if (nir_deref_instr_get_variable(nir_src_as_deref(in->src[store ? 0 : 1])) == var)
                  return true;
               break;
            case nir_intrinsic_store_output:
            case nir_intrinsic_load_input:
            case nir_intrinsic_load_interpolated_input: {
               if ((in->intrinsic == nir_intrinsic_store_output) != store)
                  break;
               int loc = nir_intrinsic_io_semantics(in).location;
               if (loc >= var->data.location && loc < var->data.location + (int)slots)
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
   nir_foreach_variable_with_modes(var, nir, mode) {
      /* (before linking, user varyings have no location yet) */
      bool builtin = !strncmp(var->name, "gl_", 3) ||
                     (var->data.location >= 0 && var->data.location < VARYING_SLOT_VAR0);
      if (vertex && builtin && (var->data.location == VARYING_SLOT_PSIZ || !strcmp(var->name, "gl_PointSize")) &&
          var_used(nir, var, true))
         out->point_size = true;
      /* the point-sprite coordinate is iterated from set 0, the set a vertex
       * shader that writes gl_PointSize reserves (DDK capture: gl_PointCoord
       * = list A iterator 1) */
      if (!vertex && builtin && (var->data.location == VARYING_SLOT_PNTC || !strcmp(var->name, "gl_PointCoord")) &&
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
         unsigned la = glsl_type_is_array(a->type) ? glsl_get_length(a->type) : 1;
         unsigned lb = glsl_type_is_array(b->type) ? glsl_get_length(b->type) : 1;
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
      unsigned len = glsl_type_is_array(var->type) ? glsl_get_length(var->type) : 1;
      unsigned cols = glsl_get_matrix_columns(e);
      struct sgx_varying *v = &out->v[out->n++];
      snprintf(v->name, sizeof v->name, "%s", var->name);
      v->rows = len * cols;
      v->comps = glsl_get_vector_elements(e);
      v->precision = var->data.precision;
      v->set = set;
      /* past ten sets glslcompiler packs rows into free components of
       * sets of the same precision; not done here */
      for (unsigned r = 0; r < v->rows && set < SGX_MAX_SETS; r++)
         out->set_comps[set++] = v->comps;
   }
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
   double c = alu->op == nir_op_fsin ? 0.5 : 0.75;
   nir_def *u = nir_ffract(b, nir_fmad(b, x, nir_imm_float(b, 0.5 / M_PI), nir_imm_float(b, c)));
   nir_def *t = nir_fmad(b, u, nir_imm_float(b, 4.0), nir_imm_float(b, -2.0));
   nir_def_replace(&alu->def, nir_fsin_agx(b, t));
   return true;
}

static bool
lower_sincos(nir_shader *s)
{
   return nir_shader_alu_pass(s, lower_sincos_instr, nir_metadata_control_flow, NULL);
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
   int pinned = lookup(o->samplers, o->n_samplers, var->name);
   if (pinned >= 0)
      return pinned;
   unsigned next = 0;
   for (unsigned i = 0; i < o->n_samplers; i++)
      next = MAX2(next, (unsigned)o->samplers[i].value + 1);
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
      nir_variable *var = nir_deref_instr_get_variable(nir_src_as_deref(tex->src[i].src));
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
lower_tex_derefs(struct sgx_ctx *c)
{
   struct tex_ctx t = { .c = c };
   return nir_shader_instructions_pass(c->nir, lower_tex_derefs_instr, nir_metadata_control_flow, &t);
}

/* gl_FrontFacing as the constant true (see sgx_compile_options.front_p0) */
static bool
front_true_instr(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
   if ((in->intrinsic != nir_intrinsic_load_input && in->intrinsic != nir_intrinsic_load_front_face) ||
       (in->intrinsic == nir_intrinsic_load_input &&
        nir_intrinsic_io_semantics(in).location != VARYING_SLOT_FACE))
      return false;
   b->cursor = nir_before_instr(&in->instr);
   nir_def_replace(&in->def, nir_imm_intN_t(b, ~0ull, in->def.bit_size));
   return true;
}

/* ------------------------------------------------------------------------
 * float16 (mediump, from Mesa's precision lowering): comparisons of 16-bit
 * values become comparisons of their f2f32 (the backend reads the F16
 * halves directly as format-select operands), so booleans stay 32-bit */
static bool
widen_cmp16_instr(nir_builder *b, nir_alu_instr *alu, void *data)
{
   if (!nir_alu_instr_is_comparison(alu) || nir_src_bit_size(alu->src[0].src) != 16)
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
   if (instr->type != nir_instr_type_alu)
      return 0;
   const nir_alu_instr *alu = nir_instr_as_alu(instr);
   const nir_op_info *info = &nir_op_infos[alu->op];
   /* moves, vectors and selects carry data of any type */
   if (alu->op == nir_op_mov || nir_op_is_vec(alu->op) || alu->op == nir_op_bcsel)
      return 0;
   bool int16 = alu->def.bit_size == 16 && nir_alu_type_get_base_type(info->output_type) != nir_type_float;
   for (unsigned i = 0; i < info->num_inputs; i++)
      int16 |= nir_src_bit_size(alu->src[i].src) == 16 &&
               nir_alu_type_get_base_type(info->input_types[i]) != nir_type_float;
   return int16 ? 32 : 0;
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
   nir_def *dyn = NULL;          /* a dynamic offset in components (one level) */
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
         snprintf(name + l, sizeof name - l, ".%s", glsl_get_struct_elem_name(t, p->strct.index));
         int pinned = lookup(u->c->opts->uniforms, u->c->opts->n_uniforms, name);
         if (pinned >= 0)
            k = pinned;
         t = glsl_get_struct_field(t, p->strct.index);
      } else if (p->deref_type == nir_deref_type_array && nir_src_is_const(p->arr.index)) {
         const struct glsl_type *e = glsl_type_is_matrix(t) ? glsl_get_column_type(t) : glsl_get_array_element(t);
         k += nir_src_as_uint(p->arr.index) * slots4(e);
         size_t l = strlen(name);
         snprintf(name + l, sizeof name - l, "[%u]", (unsigned)nir_src_as_uint(p->arr.index));
         int pinned = lookup(u->c->opts->uniforms, u->c->opts->n_uniforms, name);
         if (pinned >= 0)
            k = pinned;
         t = e;
      } else if (p->deref_type == nir_deref_type_array && !dyn) {
         /* a[i]: the offset is computed at run time; the whole array is
          * laid out (f32, contiguous) and read through an index register */
         const struct glsl_type *e = glsl_type_is_matrix(t) ? glsl_get_column_type(t) : glsl_get_array_element(t);
         unsigned len = glsl_type_is_matrix(t) ? glsl_get_matrix_columns(t) : glsl_get_length(t);
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
      for (unsigned j = 0; j < dyn_range && k + j < ARRAY_SIZE(u->c->const_fmt); j++)
         u->c->const_fmt[k + j] = 1;
      nir_def *v = nir_load_uniform(b, n, 32, dyn, .base = k, .range = dyn_range,
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
      fprintf(stderr, "uniform %s: constant %u, %u x %u bits, precision %u\n", name, k, n, in->def.bit_size, prec);
   if (fmt == 1 && u->c->p16 && prec == GLSL_PRECISION_LOW && !(k & 3)) {
      /* every use an f2f16, and what those are read for at most xyz */
      bool all16 = true;
      unsigned read = 0;
      nir_foreach_use_including_if(use, &in->def) {
         nir_instr *ui = nir_src_is_if(use) ? NULL : nir_src_use_instr(use);
         if (!ui || ui->type != nir_instr_type_alu ||
             (nir_instr_as_alu(ui)->op != nir_op_f2f16 && nir_instr_as_alu(ui)->op != nir_op_f2fmp)) {
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
   for (unsigned j = 0; j < n && k + j < ARRAY_SIZE(u->c->const_fmt); j++)
      u->c->const_fmt[k + j] = fmt == 3 && j == 3 ? 1 : fmt;
   if (fmt == 2 && prec == GLSL_PRECISION_LOW && n == 4 && !(k & 3) && k + 3 < ARRAY_SIZE(u->c->const_lowp))
      u->c->const_lowp[k] = 1;
   nir_def *v = nir_load_uniform(b, n, in->def.bit_size, nir_imm_int(b, 0), .base = k, .range = n,
                                 .dest_type = in->def.bit_size == 16 ? nir_type_float16 : nir_type_float32);
   nir_def_replace(&in->def, v);
   return true;
}

static bool
lower_uniforms(struct sgx_ctx *c)
{
   struct unif_ctx u = { c, false };
   bool p = nir_shader_intrinsics_pass(c->nir, lower_uniform_instr, nir_metadata_control_flow, &u);
   if (u.failed)
      sgx_fail(c, "indirectly indexed uniform (not supported yet)");
   return p;
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

static int
lookup(const struct sgx_named *t, unsigned n, const char *name)
{
   for (unsigned i = 0; i < n; i++)
      if (!strcmp(t[i].name, name))
         return t[i].value;
   return -1;
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
      NIR_PASS(progress, s, nir_opt_peephole_select,
               &(nir_opt_peephole_select_options){ .limit = 200, .indirect_load_ok = true,
                                                   .expensive_alu_ok = true, .discard_ok = true });
      NIR_PASS(progress, s, nir_opt_algebraic);
      NIR_PASS(progress, s, nir_opt_constant_folding);
      NIR_PASS(progress, s, nir_opt_undef);
      NIR_PASS(progress, s, nir_opt_loop_unroll);
      NIR_PASS(progress, s, nir_opt_if, nir_opt_if_optimize_phi_true_false);
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

/* per component of a varying variable, the register: base[set] + comp */
static void
map_varying(int *tab, nir_variable *var, const struct sgx_varyings *vary, const int *set_base)
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
               tab[at] = set_base[vy->set + r] + j;
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
      unsigned next_attr = 0;
      for (unsigned i = 0; i < o->n_attribs; i++)
         next_attr = MAX2(next_attr, o->attribs[i].value + 1);
      nir_foreach_shader_in_variable(var, s) {
         int a = lookup(o->attribs, o->n_attribs, var->name);
         unsigned rows = type_size_vec4(var->type, false);
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
      nir_foreach_shader_out_variable(var, s)
         if (var->data.location >= VARYING_SLOT_VAR0)
            map_varying(c->out_reg, var, c->vary, obase);
   } else {
      /* in F16 mode, a varying every load of which is mediump is iterated as
       * F16 (list A format 2: two components per register, as the DDK does) */
      bool hp[128] = { 0 }, mp[128] = { 0 };
      nir_foreach_function_impl(impl, s) {
         nir_foreach_block(b, impl) {
            nir_foreach_instr(instr, b) {
               /* (sgxc's inputs are interpolated loads; the gallium driver's
                * are still derefs here, before nir_lower_io: their
                * variable's precision says) */
               if (instr->type == nir_instr_type_intrinsic &&
                   nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_load_deref) {
                  nir_deref_instr *d = nir_src_as_deref(nir_instr_as_intrinsic(instr)->src[0]);
                  nir_variable *v = nir_deref_instr_get_variable(d);

                  if (v && v->data.mode == nir_var_shader_in && v->data.location >= 0 &&
                      v->data.location < 128)
                     ((v->data.precision == GLSL_PRECISION_MEDIUM || v->data.precision == GLSL_PRECISION_LOW)
                         ? mp : hp)[v->data.location] = true;
                  continue;
               }
               if (instr->type != nir_instr_type_intrinsic ||
                   (nir_instr_as_intrinsic(instr)->intrinsic != nir_intrinsic_load_interpolated_input &&
                    nir_instr_as_intrinsic(instr)->intrinsic != nir_intrinsic_load_input))
                  continue;
               nir_io_semantics sem = nir_intrinsic_io_semantics(nir_instr_as_intrinsic(instr));
               if (sem.location < 128)
                  (sem.medium_precision ? mp : hp)[sem.location] = true;
            }
         }
      }
      bool set16[SGX_MAX_SETS] = { 0 };
      if (c->p16) {
         nir_foreach_shader_in_variable(var, s) {
            char names[8][64];
            unsigned l = var->data.location;
            if (l < VARYING_SLOT_VAR0 || l >= 128 || !mp[l] || hp[l] || var_names(var, names, 8) != 1)
               continue;
            const struct sgx_varying *vy = find_varying(c->vary, names[0]);
            if (vy && vy->rows == 1)
               set16[vy->set] = true;
         }
      }
      /* gl_FragCoord first (list A iterator 14, F32, as the DDK orders it),
       * then the sets in order; gl_PointCoord is set 0 */
      int pbase[SGX_MAX_SETS];
      unsigned pa = 0;
      bool fragcoord = false, pointcoord = false;
      nir_foreach_shader_in_variable(var, s) {
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
               c->in_reg[VARYING_SLOT_PNTC * 4 + j] = it->f16 ? (int)(SGX_IN_HALF | (2 * pa + j)) : (int)(pa + j);
            pa += it->f16 ? 1 : 2;
            continue;
         }
         for (unsigned v = 0; v < c->vary->n; v++) {
            const struct sgx_varying *vy = &c->vary->v[v];
            if (set < vy->set || set >= vy->set + vy->rows)
               continue;
            struct sgx_iter *it = &c->iters[c->n_iters++];
            it->set = set;
            it->comps = vy->comps;
            it->pa = pa;
            it->f16 = set16[set];
            /* F16: in_reg holds SGX_IN_HALF | the half index */
            pbase[set] = it->f16 ? (int)(SGX_IN_HALF | 2 * pa) : (int)pa;
            pa += it->f16 ? (vy->comps + 1) / 2 : vy->comps;
         }
      }
      c->n_pa_inputs = pa;
      nir_foreach_shader_in_variable(var, s)
         if (var->data.location >= VARYING_SLOT_VAR0)
            map_varying(c->in_reg, var, c->vary, pbase);
   }

   /* uniforms: constant numbers */
   unsigned next = 0;
   for (unsigned i = 0; i < o->n_uniforms; i++)
      next = MAX2(next, (unsigned)o->uniforms[i].value + 4);
   next = ALIGN_POT(next, 4);
   unsigned next_sampler = 0;
   for (unsigned i = 0; i < o->n_samplers; i++)
      next_sampler = MAX2(next_sampler, (unsigned)o->samplers[i].value + 1);
   nir_foreach_uniform_variable(var, s) {
      if (glsl_type_is_sampler(glsl_without_array(var->type))) {
         /* the USP's sampler number: glslcompiler counts the samplers a
          * shader uses (corpus c3 tx_nd_s1: s1 alone is sampler 0) */
         int u = lookup(o->samplers, o->n_samplers, var->name);
         var->data.binding = u >= 0 ? (unsigned)u : next_sampler++;
         continue;
      }
      int k = lookup(o->uniforms, o->n_uniforms, var->name);
      if (k < 0) {
         /* an array or matrix pinned element by element: its base */
         char e0[80];
         snprintf(e0, sizeof e0, "%s[0]", var->name);
         k = lookup(o->uniforms, o->n_uniforms, e0);
      }
      if (k < 0) {
         k = next;
         next += ALIGN_POT(type_size_comps(var->type, false), 4);
      }
      var->data.driver_location = k;
   }
}

void
sgx_lower(struct sgx_ctx *c)
{
   nir_shader *s = c->nir;

   s->options = &options;
   assign_io(c);
   NIR_PASS(_, s, nir_lower_io_vars_to_temporaries, nir_shader_get_entrypoint(s), nir_var_shader_out);
   NIR_PASS(_, s, nir_lower_global_vars_to_local);
   NIR_PASS(_, s, nir_split_var_copies);
   NIR_PASS(_, s, nir_lower_var_copies);
   NIR_PASS(_, s, nir_lower_vars_to_ssa);
   optimize(s);
   /* (Mesa's GLSL linker hands over I/O already lowered: keep the
    * variables, they name the varyings) */
   NIR_PASS(_, s, nir_remove_dead_variables, nir_var_function_temp, NULL);
   NIR_PASS(_, s, nir_lower_flrp, 32, false);
   lower_tex_derefs(c);
   if (s->info.stage == MESA_SHADER_FRAGMENT && !c->opts->front_p0)
      NIR_PASS(_, s, nir_shader_intrinsics_pass, front_true_instr, nir_metadata_control_flow, NULL);

   NIR_PASS(_, s, nir_lower_io, nir_var_shader_in | nir_var_shader_out, type_size_vec4, 0);
   lower_uniforms(c);
   if (c->p16 && s->info.stage == MESA_SHADER_FRAGMENT) {
      /* a mediump read of a texel: the texture's result in F16 (the
       * finalizer's unpack, pck.f16.u8), not f32 then converted */
      nir_opt_16bit_tex_image_options t16 = {
         .rounding_mode = nir_rounding_mode_undef,
         .opt_tex_dest_types = nir_type_float,
      };
      NIR_PASS(_, s, nir_opt_16bit_tex_image, &t16);
   }
   NIR_PASS(_, s, nir_opt_dce);
   NIR_PASS(_, s, lower_sincos);
   optimize(s);

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

   NIR_PASS(_, s, nir_shader_alu_pass, widen_cmp16_instr, nir_metadata_control_flow, NULL);
   NIR_PASS(_, s, nir_lower_bit_size, int16_to_32, NULL);
   NIR_PASS(_, s, nir_opt_constant_folding);
   NIR_PASS(_, s, nir_opt_dce);
   if (getenv("SGX_PRINT_PRE_I2F"))
      nir_print_shader(s, stderr);
   NIR_PASS(_, s, nir_lower_int_to_float);
   NIR_PASS(_, s, nir_opt_copy_prop);
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
