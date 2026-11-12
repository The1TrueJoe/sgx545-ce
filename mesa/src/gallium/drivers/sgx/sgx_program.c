/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * Shaders.  The NIR -> USSE backend (src/sgx/compiler) compiles each one
 * to code plus a struct sgx_shader that says what it needs (docs/ISA.md
 * "What a compiler needs"); this file finalises and uploads the programs and
 * keeps what a draw needs: the constants' places in secondary attributes,
 * the attribute slots, the vertex outputs, the fragment iterators.
 *
 * Varyings get texture coordinate sets in the order of their slots, the same
 * on both sides when the two stages pass the same slots; a vertex shader that
 * writes others is compiled again to its fragment shader's sets
 * (sgx_vs_for).
 *
 * Before the compiler (SGX_DEBUG=novs,nofs still selects this), a shader was
 * matched against the fixed programs the winsys carries (sgx_fixed.c):
 * gl_Position = attribute 0, gl_FragColor = one vec4 uniform or constant.
 */

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "nir/tgsi_to_nir.h"
#include "util/format/u_format.h"
#include "util/os_time.h"
#include "util/u_memory.h"

#include "sgx_compiler.h"
#include "sgx_context.h"
#include "sgx_core.h"
#include "sgx_csb.h"
#include "sgx_program.h"
#include "sgx_screen.h"
#include "sgx_texture.h"

const nir_shader_compiler_options *
sgx_get_nir_options(void)
{
   /* the compiler's own, so the frontend builds what the compiler (and sgxc)
    * expect */
   return sgx_nir_options();
}

static unsigned
type_size(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

/* the one store_output of a shader, or NULL */
static nir_intrinsic_instr *
single_output(nir_shader *nir)
{
   nir_intrinsic_instr *out = NULL;

   nir_foreach_function_impl (impl, nir) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

            switch (intr->intrinsic) {
            case nir_intrinsic_store_output:
               if (out)
                  return NULL;
               out = intr;
               break;
            case nir_intrinsic_load_input:
            case nir_intrinsic_load_ubo:
            case nir_intrinsic_load_ubo_vec4:
            case nir_intrinsic_load_uniform:
            case nir_intrinsic_load_const_buf_base_addr_lvp:
               break;
            default:
               return NULL;
            }
         }
      }
   }
   return out;
}

/* is component c of the stored value channel `comp` of a load intrinsic of this
 * kind? */
static nir_intrinsic_instr *
source_load(nir_intrinsic_instr *store, unsigned c, unsigned *comp)
{
   nir_scalar s = nir_scalar_chase_movs(nir_get_scalar(store->src[0].ssa, c));

   if (!nir_scalar_is_intrinsic(s))
      return NULL;
   *comp = s.comp;
   return nir_def_as_intrinsic(s.def);
}

static bool
source_const(nir_intrinsic_instr *store, unsigned c, float *v)
{
   nir_scalar s = nir_scalar_chase_movs(nir_get_scalar(store->src[0].ssa, c));

   if (!nir_scalar_is_const(s))
      return false;
   *v = nir_scalar_as_float(s);
   return true;
}

static void
match_vs(struct sgx_pipe_shader *so)
{
   nir_intrinsic_instr *st = single_output(so->nir), *load = NULL;

   if (!st || nir_intrinsic_io_semantics(st).location != VARYING_SLOT_POS ||
       nir_intrinsic_write_mask(st) != 0xf ||
       nir_intrinsic_component(st) != 0 || st->src[0].ssa->num_components != 4)
      return;
   for (unsigned c = 0; c < 4; c++) {
      unsigned comp;
      nir_intrinsic_instr *l = source_load(st, c, &comp);

      if (!l || l->intrinsic != nir_intrinsic_load_input ||
          nir_intrinsic_base(l) != 0 || !nir_src_is_const(l->src[0]) ||
          nir_src_as_uint(l->src[0]) != 0 ||
          nir_intrinsic_component(l) + comp != c || (load && l != load))
         return;
      load = l;
   }
   so->kind = SGX_SHADER_VS_POSITION;
}

static void
match_fs(struct sgx_pipe_shader *so)
{
   nir_intrinsic_instr *st = single_output(so->nir), *load = NULL;
   unsigned loc;
   float k[4];
   bool constant = true;

   if (!st)
      return;
   /* (the fixed programs write a float colour: an integer one is compiled,
    * packed for its target) */
   nir_alu_type ot = nir_alu_type_get_base_type(nir_intrinsic_src_type(st));
   if (ot == nir_type_int || ot == nir_type_uint)
      return;
   loc = nir_intrinsic_io_semantics(st).location;
   if ((loc != FRAG_RESULT_COLOR && loc != FRAG_RESULT_DATA0) ||
       nir_intrinsic_write_mask(st) != 0xf ||
       st->src[0].ssa->num_components != 4)
      return;
   for (unsigned c = 0; c < 4; c++)
      constant &= source_const(st, c, &k[c]);
   if (constant) {
      memcpy(so->color, k, sizeof(k));
      so->kind = SGX_SHADER_FS_CONSTANT;
      return;
   }
   for (unsigned c = 0; c < 4; c++) {
      unsigned comp;
      nir_intrinsic_instr *l = source_load(st, c, &comp);

      if (!l || comp != c || (load && l != load))
         return;
      if (l->intrinsic == nir_intrinsic_load_ubo) {
         if (!nir_src_is_const(l->src[0]) || nir_src_as_uint(l->src[0]) != 0 ||
             !nir_src_is_const(l->src[1]))
            return;
      } else if (l->intrinsic == nir_intrinsic_load_uniform) {
         if (!nir_src_is_const(l->src[0]))
            return;
      } else {
         return;
      }
      load = l;
   }
   /* load_ubo: a byte offset into constant buffer 0; load_uniform: vec4 slots */
   if (load->intrinsic == nir_intrinsic_load_ubo)
      so->ubo_offset = nir_src_as_uint(load->src[1]);
   else
      so->ubo_offset =
         (nir_intrinsic_base(load) + nir_src_as_uint(load->src[0])) * 16;
   so->kind = SGX_SHADER_FS_UNIFORM;
}

/* ---------------------------------------------------------------- compiled
 * shaders */

/* st hands uniforms over as load_uniform in vec4 slots; the compiler numbers
 * constants in components (constant k = constant buffer 0 float k), a
 * dynamic offset (a[i]) too */
static bool
rescale_uniform(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
   if (in->intrinsic != nir_intrinsic_load_uniform)
      return false;
   b->cursor = nir_before_instr(&in->instr);
   if (!nir_src_is_const(in->src[0])) {
      nir_def *v = nir_load_uniform(b, in->def.num_components, in->def.bit_size,
                                    nir_imul_imm(b, in->src[0].ssa, 4),
                                    .base = nir_intrinsic_base(in) * 4,
                                    .range = nir_intrinsic_range(in) * 4,
                                    .dest_type = nir_intrinsic_dest_type(in));
      nir_def_replace(&in->def, v);
      return true;
   }
   nir_def *v = nir_load_uniform(
      b, in->def.num_components, in->def.bit_size, nir_imm_int(b, 0),
      .base = (nir_intrinsic_base(in) + nir_src_as_uint(in->src[0])) * 4,
      .range = nir_intrinsic_range(in) * 4,
      .dest_type = nir_intrinsic_dest_type(in));
   nir_def_replace(&in->def, v);
   return true;
}

/* Uses of the integer def by those of f, a float the draw fills: an i2f32 of
 * it takes f itself, anything else f2i32(f).  The integer is whole and well
 * within F32's 24 bits (a size, an instance number), so the conversions are
 * exact. */
static void
rewrite_int_as_float(nir_builder *b, nir_def *def, nir_def *f)
{
   nir_def *i = NULL;

   nir_foreach_use_including_if_safe (use, def) {
      nir_instr *u = nir_src_is_if(use) ? NULL : nir_src_use_instr(use);

      if (u && u->type == nir_instr_type_alu &&
          nir_instr_as_alu(u)->op == nir_op_i2f32) {
         nir_alu_instr *alu = nir_instr_as_alu(u);
         unsigned swz[NIR_MAX_VEC_COMPONENTS];

         for (unsigned c = 0; c < alu->def.num_components; c++)
            swz[c] = alu->src[0].swizzle[c];
         b->cursor = nir_after_instr(u);
         nir_def_rewrite_uses(&alu->def,
                              nir_swizzle(b, f, swz, alu->def.num_components));
      } else {
         if (!i) {
            b->cursor = nir_after_instr(nir_def_instr(f));
            i = nir_f2i32(b, f);
         }
         nir_src_rewrite(use, i);
      }
   }
}

/* textureSize of a program compiled with native integers: the base level's
 * size from driver constants (SGX_K_SIZE + 3u: width, height, depth or
 * layers, raw integers), shifted down to the level asked for; an array's
 * layer count isn't */
static bool
lower_txs_int(nir_builder *b, nir_tex_instr *tex)
{
   int li = nir_tex_instr_src_index(tex, nir_tex_src_lod);
   unsigned n = tex->def.num_components;

   if (tex->texture_index >= SGX_K_SIZE_UNITS || n > 3)
      return false;
   b->cursor = nir_before_instr(&tex->instr);
   nir_def *size = nir_load_uniform(
      b, n, 32, nir_imm_int(b, 0), .base = SGX_K_SIZE + 3 * tex->texture_index,
      .range = n, .dest_type = nir_type_int32);

   if (li >= 0 && !(nir_src_is_const(tex->src[li].src) &&
                    nir_src_as_uint(tex->src[li].src) == 0)) {
      nir_def *lod = tex->src[li].src.ssa;
      nir_def *c[3];
      unsigned mips = n - (tex->is_array ? 1 : 0);

      for (unsigned i = 0; i < n; i++) {
         c[i] = nir_channel(b, size, i);
         if (i < mips)
            c[i] = nir_imax(b, nir_ushr(b, c[i], lod), nir_imm_int(b, 1));
      }
      size = nir_vec(b, c, n);
   }
   nir_def_replace(&tex->def, size);
   return true;
}

/* textureSize (level 0) of a fragment program, which st's lowering of
 * rectangle lookups asks for (coords / textureSize): the size from driver
 * constants (SGX_K_TEXSIZE) the draw fills, as floats, where the program
 * converts it to float anyway.  With native integers (data), lower_txs_int. */
static bool
lower_txs(nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type != nir_instr_type_tex)
      return false;
   nir_tex_instr *tex = nir_instr_as_tex(instr);
   int li = nir_tex_instr_src_index(tex, nir_tex_src_lod);

   if (tex->op == nir_texop_txs && *(const bool *)data)
      return lower_txs_int(b, tex);
   /* the view's level count, from the draw (SGX_K_LEVELS) */
   if (tex->op == nir_texop_query_levels && *(const bool *)data &&
       tex->texture_index < SGX_K_SIZE_UNITS) {
      b->cursor = nir_before_instr(instr);
      nir_def_replace(&tex->def,
                      nir_load_uniform(b, 1, 32, nir_imm_int(b, 0),
                                       .base = SGX_K_LEVELS + tex->texture_index,
                                       .range = 1, .dest_type = nir_type_int32));
      return true;
   }
   if (tex->op != nir_texop_txs || tex->def.num_components > 2 ||
       tex->texture_index >= SGX_K_TEXSIZE_UNITS ||
       (li >= 0 && (!nir_src_is_const(tex->src[li].src) ||
                    nir_src_as_uint(tex->src[li].src))))
      return false;
   b->cursor = nir_before_instr(instr);
   nir_def *f = nir_load_uniform(
      b, tex->def.num_components, 32, nir_imm_int(b, 0),
      .base = SGX_K_TEXSIZE + 2 * tex->texture_index,
      .range = tex->def.num_components, .dest_type = nir_type_float32);

   rewrite_int_as_float(b, &tex->def, f);
   nir_instr_remove(instr);
   return true;
}

/* gl_InstanceID of a vertex program: an instanced draw is one draw per
 * instance (sgx_draw_vbo), each with its instance number in a driver
 * constant: SGX_K_INSTANCE as a float, or with native integers (data)
 * SGX_K_INSTANCE_INT as an integer */
static bool
lower_instance_id(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
   if (in->intrinsic != nir_intrinsic_load_instance_id)
      return false;
   b->cursor = nir_before_instr(&in->instr);
   if (*(const bool *)data) {
      nir_def_replace(&in->def,
                      nir_load_uniform(b, 1, 32, nir_imm_int(b, 0),
                                       .base = SGX_K_INSTANCE_INT, .range = 1,
                                       .dest_type = nir_type_int32));
      return true;
   }
   nir_def *f =
      nir_load_uniform(b, 1, 32, nir_imm_int(b, 0), .base = SGX_K_INSTANCE,
                       .range = 1, .dest_type = nir_type_float32);

   rewrite_int_as_float(b, &in->def, f);
   nir_instr_remove(&in->instr);
   return true;
}

/*
 * varying -> set location - VAR0, the same on both sides.  Entries are named
 * the way the compiler looks varyings up (sgx_nir.c var_names): st names a
 * packed varying "packed:a,b", one name per varying it holds.
 */
/* Desktop GL's built-in varyings (colours, back colours, fog, texture
 * coordinates; also st's fixed-function shaders and GLES1 translation layers)
 * are in the legacy slots, which the compiler doesn't assign: move them to VAR
 * slots the program's own varyings leave free, each built-in its own whether
 * this stage has it or not, so that two stages with the same varyings (a linked
 * program's) place them alike; named after the built-in, so that a vertex
 * shader compiled to its fragment shader's sets finds them anyway. st's
 * two-sided colour reads the back colours in the fragment shader. */
#define SGX_LEGACY_NAME "sgx_legacy"

static void
remap_legacy_varyings(nir_shader *nir)
{
   static const gl_varying_slot map[] = {
      VARYING_SLOT_COL0, VARYING_SLOT_COL1, VARYING_SLOT_BFC0,
      VARYING_SLOT_BFC1, VARYING_SLOT_FOGC, VARYING_SLOT_TEX0,
      VARYING_SLOT_TEX1, VARYING_SLOT_TEX2, VARYING_SLOT_TEX3,
      VARYING_SLOT_TEX4, VARYING_SLOT_TEX5, VARYING_SLOT_TEX6,
      VARYING_SLOT_TEX7,
   };
   nir_variable_mode mode = nir->info.stage == MESA_SHADER_VERTEX
                               ? nir_var_shader_out
                               : nir_var_shader_in;
   int slot[ARRAY_SIZE(map)];
   uint32_t used = 0; /* VAR slots of the program's own varyings */
   bool changed = false;

   nir_foreach_variable_with_modes (var, nir, mode) {
      if (var->data.location >= VARYING_SLOT_VAR0 &&
          var->data.location < VARYING_SLOT_VAR0 + 32) {
         for (unsigned r = 0;
              r < glsl_count_vec4_slots(var->type, false, false); r++)
            used |=
               BITFIELD_BIT((var->data.location - VARYING_SLOT_VAR0 + r) & 31);
      }
   }
   for (unsigned i = 0, s = 0; i < ARRAY_SIZE(map); i++) {
      while (s < 32 && (used & BITFIELD_BIT(s)))
         s++;
      slot[i] = s < 32 ? (int)s++ : -1;
   }
   nir_foreach_variable_with_modes (var, nir, mode) {
      for (unsigned i = 0; i < ARRAY_SIZE(map); i++) {
         char name[32];

         if (var->data.location != (int)map[i])
            continue;
         if (slot[i] < 0) {
            fprintf(stderr, "sgx: no VAR slot left for a built-in varying\n");
            break;
         }
         var->data.location = VARYING_SLOT_VAR0 + slot[i];
         snprintf(name, sizeof(name), SGX_LEGACY_NAME "%u", i);
         var->name = ralloc_strdup(nir, name);
         changed = true;
         break;
      }
   }
   if (changed)
      nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
}

static int
cmp_varying(const void *a, const void *b)
{
   const nir_variable *x = *(nir_variable *const *)a,
                      *y = *(nir_variable *const *)b;

   if (x->data.location != y->data.location)
      return x->data.location - y->data.location;
   return (int)x->data.location_frac - (int)y->data.location_frac;
}

/* The varyings' sets.  sprite: a fragment shader that reads gl_PointCoord
 * (found here), or a vertex shader to go with one: the ISP writes the
 * coordinate into set 0 (STATE.md "Lines and points"), so everything else
 * moves up one set. */
static void
assign_varyings(nir_shader *nir, struct sgx_varyings *vary, bool sprite)
{
   nir_variable_mode mode = nir->info.stage == MESA_SHADER_VERTEX
                               ? nir_var_shader_out
                               : nir_var_shader_in;
   nir_variable *vars[ARRAY_SIZE(vary->v)];
   unsigned n = 0;
   uint64_t rows_used = 0; /* by location - VAR0 */
   bool psize = false;
   static bool warned;

   memset(vary, 0, sizeof(*vary));
   nir_foreach_variable_with_modes (var, nir, mode) {
      if (nir->info.stage == MESA_SHADER_FRAGMENT &&
          var->data.location == VARYING_SLOT_PNTC)
         sprite = true;
      /* the point size is the output dword after the sets */
      if (nir->info.stage == MESA_SHADER_VERTEX &&
          var->data.location == VARYING_SLOT_PSIZ)
         psize = true;
      if (var->data.location < VARYING_SLOT_VAR0 || n == ARRAY_SIZE(vars))
         continue;
      vars[n++] = var;
      for (unsigned r = 0; r < glsl_count_vec4_slots(var->type, false, false);
           r++)
         rows_used |=
            BITFIELD64_BIT((var->data.location - VARYING_SLOT_VAR0 + r) & 63);
   }
   /* a set per slot in slot order (after gl_PointCoord's set 0), each
    * variable at its component in the slot */
   qsort(vars, n, sizeof(vars[0]), cmp_varying);
   for (unsigned i = 0; i < n; i++) {
      nir_variable *var = vars[i];
      const struct glsl_type *t = var->type;
      unsigned slot = var->data.location - VARYING_SLOT_VAR0,
               frac = var->data.location_frac;
      unsigned rows = glsl_count_vec4_slots(t, false, false), comps, set;
      struct sgx_varying *v = &vary->v[vary->n++];
      char name[32];

      t = glsl_without_array(t);
      comps = glsl_type_is_matrix(t)
                 ? glsl_get_vector_elements(glsl_get_column_type(t))
                 : glsl_get_vector_elements(t);
      /* a built-in the two stages may declare apart (gl_FogFragCoord a float
       * against fog's vec4): the whole slot, under its own name */
      bool legacy = var->name && !strncmp(var->name, SGX_LEGACY_NAME,
                                          strlen(SGX_LEGACY_NAME));
      if (legacy) {
         frac = 0;
         comps = 4;
      }
      set =
         (sprite ? 1 : 0) + util_bitcount64(rows_used & BITFIELD64_MASK(slot));
      /* The compiler finds a varying's set by name.  st's packed slots are
       * named after what they hold ("packed:a,b"), and a varying split over
       * two slots names both: two vec3 texture coordinates and the fog had
       * the second slot written over the first.
       * Each variable is named after its slot instead, the same in both
       * stages. */
      if (legacy)
         snprintf(name, sizeof(name), "%s", var->name);
      else
         snprintf(name, sizeof(name), frac ? "sgx_v%u_%u" : "sgx_v%u", slot,
                  frac);
      var->name = ralloc_strdup(nir, name);
      snprintf(v->name, sizeof(v->name), "%s", name);
      v->set = set;
      v->comp = frac;
      v->rows = rows;
      v->comps = comps;
      v->precision = var->data.precision;
      for (unsigned r = 0; r < rows && set + r < SGX_MAX_SETS; r++)
         vary->set_comps[set + r] =
            MAX2(vary->set_comps[set + r], frac + comps);
   }
   /* noperspective: the compiler's own w varying (SGX_NOPERSPECTIVE_W, one
    * highp component), in the set after the slots', the same in both stages
    * (the vertex program writes v * w and w, the fragment one divides) */
   bool noperspective = false;
   for (unsigned i = 0; i < n; i++)
      noperspective |=
         vars[i]->data.interpolation == INTERP_MODE_NOPERSPECTIVE;
   if (noperspective && vary->n < ARRAY_SIZE(vary->v)) {
      struct sgx_varying *v = &vary->v[vary->n++];
      unsigned set = (sprite ? 1 : 0) + util_bitcount64(rows_used);

      snprintf(v->name, sizeof(v->name), "%s", SGX_NOPERSPECTIVE_W);
      v->set = set;
      v->comp = 0;
      v->rows = 1;
      v->comps = 1;
      v->precision = GLSL_PRECISION_HIGH;
      if (set < SGX_MAX_SETS)
         vary->set_comps[set] = MAX2(vary->set_comps[set], 1);
   }
   /* A flat primary colour (glShadeModel(GL_FLAT): st makes it a flat
    * input) goes to set 0 as the TA's vertex colour, the one source the PDS
    * iterates flat; the sets before it move up one.  Not with a sprite's
    * coordinate in set 0. */
   for (unsigned i = 0; nir->info.stage == MESA_SHADER_FRAGMENT && !sprite &&
                        i < n && i < vary->n;
        i++) {
      struct sgx_varying *c = &vary->v[i];

      if (strcmp(c->name, SGX_LEGACY_NAME "0") ||
          vars[i]->data.interpolation != INTERP_MODE_FLAT || c->rows != 1)
         continue;
      for (unsigned j = 0; j < vary->n; j++) {
         if (j != i && vary->v[j].set < c->set)
            vary->v[j].set++;
      }
      c->set = 0;
      memset(vary->set_comps, 0, sizeof(vary->set_comps));
      for (unsigned j = 0; j < vary->n; j++) {
         const struct sgx_varying *v = &vary->v[j];

         for (unsigned r = 0; r < v->rows && v->set + r < SGX_MAX_SETS; r++)
            vary->set_comps[v->set + r] =
               MAX2(vary->set_comps[v->set + r], v->comp + v->comps);
      }
      vary->colour = true;
      break;
   }
   /* past ten sets: rows into the free components of earlier sets */
   vary->point_size = sprite;
   if (sprite)
      vary->set_comps[0] = 2;
   if (!sgx_pack_varyings(vary) && !warned) {
      fprintf(stderr, "sgx: varyings past ten sets of four components\n");
      warned = true;
   }
   vary->point_size = sprite || psize;
}

/* the same sets for the same varyings (the stages' precisions may differ) */
static bool
same_varyings(const struct sgx_varyings *a, const struct sgx_varyings *b)
{
   if (a->n != b->n || a->colour != b->colour ||
       memcmp(a->set_comps, b->set_comps, sizeof(a->set_comps)))
      return false;
   for (unsigned i = 0; i < a->n; i++) {
      const struct sgx_varying *x = &a->v[i], *y = &b->v[i];

      if (strcmp(x->name, y->name) || x->set != y->set || x->comp != y->comp ||
          x->rows != y->rows || x->comps != y->comps)
         return false;
   }
   return true;
}

#define SGX_FRAGCOORD_ITER 14

/* what the draw needs from the compiled program besides its code: the
 * dependent samples (where each smp goes in the flat program, its template,
 * sampler and raw texel register) and the branches whose offsets move when
 * the smps go in */
struct prog_info {
   const char *why; /* what read_program refused */
   struct {
      unsigned at, sampler, raw;
      unsigned phase; /* split: at indexes code1 */
      bool f16;       /* the texel returns F16, two registers from raw */
      bool fetch;     /* a texelFetch: state of its own (sgx_draw.c) */
      unsigned plane; /* an RG32 / RGBA32 unit's plane (channel) */
      uint64_t smp;
   } deps[SGX_MAX_DEP_SAMPLES];
   unsigned n_deps;
   unsigned branch_at[64]; /* flat indices of the branches */
   unsigned n_branches;
};

/* The flat program with each dependent sample's smp and its wdf put in
 * (ISA.md "The texture-sample sequence"): the template with the raw texel's
 * temporary as destination (l21-27, h7 clear: r) and the sampler's 4 state
 * words, in SA from `sa` on, as the texture state (src1 l7-13, bank l30-31 =
 * 3: sa; h17 clear); then `wdf` on the template's DRC before the compiler's
 * unpack reads the texel. Branch offsets are moved with the code. NULL on
 * allocation failure; *n is the new length. */
static uint64_t *
insert_samples(const uint64_t *code, unsigned count,
               const struct prog_info *info, const unsigned *sa, unsigned *n)
{
   unsigned total = count + 2 * info->n_deps, j = 0, k = 0;
   uint64_t *w = malloc(total * sizeof(*w));
   unsigned *map = malloc((count + 1) * sizeof(*map));

   if (!w || !map) {
      free(w);
      free(map);
      return NULL;
   }
   for (unsigned i = 0; i <= count; i++) {
      while (j < info->n_deps && info->deps[j].at == i) {
         uint32_t lo = (uint32_t)info->deps[j].smp,
                  hi = info->deps[j].smp >> 32;

         lo = (lo & ~(0x7fu << 21 | 0x7fu << 7 | 0xc0000000u)) |
              info->deps[j].raw << 21 | sa[j] << 7 | 0xc0000000u;
         hi &= ~(0x80u | 1u << 17);
         w[k++] = (uint64_t)hi << 32 | lo;
         w[k++] = (uint64_t)(0xf9200000u | (hi & 1)) << 32; /* wdf drc */
         j++;
      }
      map[i] = k;
      if (i < count)
         w[k++] = code[i];
   }
   /* the branches' relative offsets (l0-17, signed) */
   for (unsigned b = 0; b < info->n_branches; b++) {
      unsigned i = info->branch_at[b];
      uint32_t lo = (uint32_t)code[i], hi = code[i] >> 32;
      int off = (int)(lo << 14) >> 14, t = (int)i + off;

      if (i >= count || t < 0 || t > (int)count)
         continue;
      off = (int)map[t] - (int)map[i];
      w[map[i]] =
         (uint64_t)hi << 32 | (lo & ~0x3ffffu) | ((uint32_t)off & 0x3ffffu);
   }
   free(map);
   *n = k;
   return w;
}

static_assert(SGX_MAX_CONST_MAP == SGX_MAX_CONST_MAP,
              "the driver holds the compiler's whole constant map");

static bool
read_program(const struct sgx_shader *out, struct sgx_pipe_shader *so,
             struct prog_info *info)
{
   /* list C, decoded: kind 1 f32, 2 F16 half, 3 a C10 channel; ours
    * (opts.driver_consts): 0x10 an F16 half of the C10 value, 0x11 a literal,
    * 0x12 a vec4 packed as the BGRA8 colour */
   for (unsigned i = 0; i < out->n_consts; i++) {
      const struct sgx_const_map *m = &out->consts[i];
      struct sgx_const *k = &so->consts[so->n_consts];

      if (so->n_consts == SGX_MAX_CONST_MAP)
         return info->why = "too many constants", false;
      k->fmt = m->fmt;
      k->k = m->k;
      k->value = m->value;
      k->slot = m->slot;
      k->shift = m->shift;
      k->mem = m->mem;
      if (k->slot != SGX_SLOT_NONE)
         so->const_dwords = MAX2(so->const_dwords, k->slot + 1u);
      so->n_consts++;
   }
   if (out->mem_dwords > SGX_MAX_CONST_MAP + 1)
      return info->why = "constant memory block too large", false;
   so->mem_dwords = out->mem_dwords;
   so->n_mem_bases = MIN2(out->n_mem_bases, ARRAY_SIZE(so->mem_base_slot));
   for (unsigned j = 0; j < so->n_mem_bases; j++) {
      so->mem_base_slot[j] = out->mem_base_slot[j];
      so->const_dwords = MAX2(so->const_dwords, out->mem_base_slot[j] + 1u);
   }
   for (unsigned i = 0; i < out->n_samples; i++) {
      const struct sgx_sample_info *x = &out->samples[i];

      if (!x->dependent)
         continue;
      if (info->n_deps == SGX_MAX_DEP_SAMPLES)
         return info->why = "too many dependent texture samples", false;
      info->deps[info->n_deps].at = x->at;
      info->deps[info->n_deps].sampler = x->sampler;
      info->deps[info->n_deps].raw = x->reg;
      info->deps[info->n_deps].smp = x->smp;
      info->deps[info->n_deps].phase = x->phase;
      info->deps[info->n_deps].f16 = x->f16;
      info->deps[info->n_deps].fetch = x->fetch;
      info->deps[info->n_deps].plane = x->plane;
      info->n_deps++;
   }
   for (unsigned i = 0; i < out->n_blocks; i++) {
      if (out->blocks[i].kind != SGX_BLOCK_BRANCH)
         continue;
      if (info->n_branches == ARRAY_SIZE(info->branch_at))
         return info->why = "too many branches", false;
      info->branch_at[info->n_branches++] = out->blocks[i].at;
   }
   return true;
}

/* the compiler's layout: vertex attributes where st put them
 * (driver_location); uniforms and samplers are the compiler's */
static int
layout_place(void *data, enum sgx_layout_kind kind, const char *name)
{
   nir_shader *nir = data;

   if (kind != SGX_LAYOUT_ATTRIB)
      return -1;
   nir_foreach_shader_in_variable (var, nir) {
      if (var->name && !strcmp(var->name, name))
         return var->data.driver_location;
   }
   return -1;
}

static unsigned
layout_first_free(void *data, enum sgx_layout_kind kind)
{
   nir_shader *nir = data;
   unsigned next = 0;

   if (kind == SGX_LAYOUT_ATTRIB) {
      nir_foreach_shader_in_variable (var, nir) {
         if (var->name)
            next = MAX2(next, var->data.driver_location +
                                 glsl_count_attribute_slots(var->type, true));
      }
   }
   return next;
}

/*
 * Linear filtering of an sRGB texture, in the shader: the TPU filters the
 * encoded texels and the decode comes after, where GL decodes first.  The four
 * texels are sampled at their centres (where the TPU's filter returns the
 * texel itself), decoded (nir_lower_tex's lower_srgb, after this) and mixed
 * here.  Only for textures sampled at one level (sgx_fs_for): the level a
 * minified lookup takes isn't known to the shader.  data: the units.
 */
static bool
lower_srgb_filter(nir_builder *b, nir_instr *instr, void *data)
{
   uint32_t units = *(const uint32_t *)data;

   if (instr->type != nir_instr_type_tex)
      return false;
   nir_tex_instr *tex = nir_instr_as_tex(instr);
   int ci = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   bool rect = tex->sampler_dim == GLSL_SAMPLER_DIM_RECT;

   if (tex->op != nir_texop_tex || tex->texture_index >= SGX_K_TEXSIZE_UNITS ||
       !(units & (1u << tex->texture_index)) || tex->is_shadow ||
       tex->is_array || ci < 0 || tex->coord_components != 2 ||
       (tex->sampler_dim != GLSL_SAMPLER_DIM_2D && !rect) ||
       nir_tex_instr_src_index(tex, nir_tex_src_projector) >= 0 ||
       nir_tex_instr_src_index(tex, nir_tex_src_bias) >= 0 ||
       nir_tex_instr_src_index(tex, nir_tex_src_offset) >= 0)
      return false;
   b->cursor = nir_before_instr(instr);
   /* in texels: a rectangle texture's coordinates already are */
   nir_def *sz =
      rect ? NULL
           : nir_load_uniform(b, 2, 32, nir_imm_int(b, 0),
                              .base = SGX_K_TEXSIZE + 2 * tex->texture_index,
                              .range = 2, .dest_type = nir_type_float32);
   nir_def *coord = tex->src[ci].src.ssa;
   nir_def *u = nir_fadd_imm(b, sz ? nir_fmul(b, coord, sz) : coord, -0.5);
   nir_def *i0 = nir_ffloor(b, u);
   nir_def *f = nir_fsub(b, u, i0);
   nir_def *t[4];

   for (unsigned k = 0; k < 4; k++) {
      nir_tex_instr *tap = nir_instr_as_tex(nir_instr_clone(b->shader, instr));
      nir_def *c =
         nir_fadd(b, i0, nir_imm_vec2(b, (k & 1) + 0.5f, (k >> 1) + 0.5f));

      nir_builder_instr_insert(b, &tap->instr);
      nir_src_rewrite(&tap->src[ci].src, sz ? nir_fdiv(b, c, sz) : c);
      t[k] = &tap->def;
   }
   unsigned n = tex->def.num_components;
   nir_def *fx = nir_replicate(b, nir_channel(b, f, 0), n);
   nir_def *fy = nir_replicate(b, nir_channel(b, f, 1), n);
   nir_def *r =
      nir_flrp(b, nir_flrp(b, t[0], t[1], fx), nir_flrp(b, t[2], t[3], fx), fy);

   nir_def_replace(&tex->def, r);
   return true;
}

/* the units a fragment shader takes shadow samples of */
static uint32_t
shadow_units(nir_shader *nir)
{
   uint32_t units = 0;

   nir_foreach_function_impl (impl, nir) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_tex ||
                !nir_instr_as_tex(instr)->is_shadow)
               continue;
            nir_tex_instr *tex = nir_instr_as_tex(instr);
            int d = nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);
            nir_variable *var = d >= 0 ? nir_deref_instr_get_variable(
                                            nir_src_as_deref(tex->src[d].src))
                                       : NULL;
            unsigned unit = var ? var->data.binding : tex->texture_index;

            if (unit < 32)
               units |= 1u << unit;
         }
      }
   }
   return units;
}

/* A shadow sample: the depth sampled as a value, compared in the shader
 * (the TPU doesn't filter the F32 depth, so a compare of the nearest texel)
 * with the sampler's function (compare: PIPE_FUNC + 1 by unit, 0 or NULL
 * LEQUAL, GL's default) and swizzled as the view says (swz, sgx_fs_for). */
static void
lower_shadow(nir_shader *nir, const uint8_t *compare, const uint16_t *swz)
{
   enum compare_func func[PIPE_MAX_SAMPLERS];
   nir_lower_tex_shadow_swizzle sw[PIPE_MAX_SAMPLERS];

   for (unsigned u = 0; u < PIPE_MAX_SAMPLERS; u++) {
      func[u] = compare && compare[u] ? (enum compare_func)(compare[u] - 1)
                                      : COMPARE_FUNC_LEQUAL;
      sw[u] = (nir_lower_tex_shadow_swizzle){PIPE_SWIZZLE_X, PIPE_SWIZZLE_Y,
                                             PIPE_SWIZZLE_Z, PIPE_SWIZZLE_W};
      if (swz && swz[u])
         sw[u] = (nir_lower_tex_shadow_swizzle){
            swz[u] & 7, swz[u] >> 3 & 7, swz[u] >> 6 & 7, swz[u] >> 9 & 7};
   }
   /* (the reference clamped to [0, 1], as for the fixed-point depths) */
   NIR_PASS(_, nir, nir_lower_tex_shadow, PIPE_MAX_SAMPLERS, func, sw, true);
}

/* fs_vary: a vertex shader compiled to a fragment shader's sets; srgb: the
 * samplers whose texels a fragment shader decodes from sRGB, srgb_filter
 * those of them it filters itself (lower_srgb_filter) */
/* An integer colour buffer's word, which the PBE stores unchanged
 * (sgx_resource_target; the compiler's rt_packed): output 0's channels as
 * the colour word's B, G, R, A bytes for a target of 8 bits a channel, their
 * low bytes (PBE format 1 stores them R, G, B, A); for R32 the value
 * itself; for plane c of an RG32 or RGBA32 one, channel c (kind: the
 * variant's rt_int). */
static nir_def *
pack_word(nir_builder *b, nir_def *v, unsigned wm, unsigned kind)
{
   unsigned n = v->num_components;

   if (kind >= 2) {
      /* R32: the value; a plane of RG32 / RGBA32 (kind 3 + c): channel c */
      unsigned c = kind == 2 ? 0 : kind - 3;
      return c < n ? nir_channel(b, v, c) : nir_imm_int(b, 0);
   }
   static const unsigned shift[4] = {16, 8, 0, 24}; /* r, g, b, a */
   nir_def *word = nir_imm_int(b, 0);

   for (unsigned c = 0; c < n; c++) {
      if (!(wm >> c & 1))
         continue;
      word = nir_ior(b, word,
                     nir_ishl_imm(b, nir_iand_imm(b, nir_channel(b, v, c), 0xff),
                                  shift[c]));
   }
   return word;
}

/* The program as the compiler takes it still has its outputs as variables
 * (sgx_lower lowers I/O itself): output 0 is brought down to one store of
 * the variable at the end, and that store becomes one of the packed word,
 * the variable a uint (the compiler's rt_packed: one word, mask 1). */
static bool
pack_int_outputs(nir_shader *nir, unsigned kind)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_variable *out = NULL;

   nir_foreach_shader_out_variable (var, nir) {
      if (var->data.location == FRAG_RESULT_COLOR ||
          var->data.location == FRAG_RESULT_DATA0)
         out = var;
   }
   if (!out || !glsl_type_is_32bit(glsl_without_array(out->type)))
      return false;
   NIR_PASS(_, nir, nir_lower_io_vars_to_temporaries, impl,
            nir_var_shader_out);
   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_lower_var_copies);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);

   nir_builder b = nir_builder_create(impl);
   bool progress = false;

   nir_foreach_block (block, impl) {
      nir_foreach_instr_safe (instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *st = nir_instr_as_intrinsic(instr);

         if (st->intrinsic != nir_intrinsic_store_deref ||
             nir_deref_instr_get_variable(nir_src_as_deref(st->src[0])) != out)
            continue;
         b.cursor = nir_before_instr(instr);
         nir_def *word = pack_word(&b, st->src[1].ssa,
                                   nir_intrinsic_write_mask(st), kind);
         nir_instr_remove(instr);
         out->type = glsl_uint_type();
         nir_store_deref(&b, nir_build_deref_var(&b, out), word, 0x1);
         progress = true;
      }
   }
   if (progress) {
      out->type = glsl_uint_type();
      NIR_PASS(_, nir, nir_opt_dce);
   }
   return progress;
}

static bool
compile_shader(struct sgx_context *ctx, struct sgx_pipe_shader *so,
               nir_shader *orig, const struct sgx_varyings *fs_vary,
               unsigned srgb, uint32_t srgb_filter, const uint16_t *swz,
               uint32_t not_bgra, const uint8_t *vtx_fmt, bool points,
               uint32_t depth, const uint8_t *compare)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);
   nir_shader *nir = nir_shader_clone(NULL, orig);
   struct sgx_compile_options opts = {0};
   struct sgx_varyings vary;
   struct sgx_shader out;
   struct prog_info info = {0};
   bool vertex = nir->info.stage == MESA_SHADER_VERTEX;

   opts.verbose = (sgx_drv_debug & SGX_DEBUG_NIR) != 0;
   /* The point size (st gives every vertex program one) is an output only
    * for points: elsewhere it costs a dword a vertex in the parameter buffer
    * and rounds the vertex up to the next four (the DDK's triangles carry
    * none) */
   if (vertex && !points) {
      bool demoted = false;

      nir_foreach_shader_out_variable (var, nir) {
         if (var->data.location == VARYING_SLOT_PSIZ) {
            var->data.mode = nir_var_shader_temp;
            demoted = true;
         }
      }
      if (demoted) {
         nir_fixup_deref_modes(nir);
         NIR_PASS(_, nir, nir_lower_vars_to_ssa);
         NIR_PASS(_, nir, nir_opt_dce);
         NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_shader_temp, NULL);
      }
   }
   opts.f16 = ctx->base.screen->shader_caps[nir->info.stage].fp16;
   /* list C kinds 0x10 and 0x11 (fill_consts): lowp colours pre-converted to
    * F16 and literals in SA, where libusc converts in a secondary program */
   opts.driver_consts = true;
   /* GLSL 1.30's integers, once the screen has them: the cap is the
    * screen's, so GLSL ES 1.00 programs come with integers then too */
   opts.native_integers = so->native_integers =
      screen->base.shader_caps[nir->info.stage].integers;
   if (vtx_fmt) {
      memcpy(opts.vtx_fmt, vtx_fmt, sizeof(opts.vtx_fmt));
      memcpy(so->vtx_fmt, vtx_fmt, sizeof(so->vtx_fmt));
   }
   /* st's fixed-function fragment programs only owe the framebuffer's
    * precision: mediump (texture coordinates stay F32); dEQP-GLES2 mustpass
    * and piglit unchanged with it and F16 samples (EA1, 2026-11-10) */
   /* the units sampling 8888 texels get them B, G, R, A ordered
    * (sgx_tex_format): all of them unless the variant says otherwise */
   /* a unit sampled for a compare holds a depth texture */
   uint32_t shadow =
      nir->info.stage == MESA_SHADER_FRAGMENT ? shadow_units(nir) : 0;
   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      opts.tex_bgra = ~not_bgra & ~(depth | shadow);
      opts.tex_f32 = depth | shadow;
      /* RGBA32 and RG32: a read of each channel's R32 plane (each read's
       * plane in its sample info, sgx_draw.c adds it to the address) */
      opts.tex_r32x4 = so->r32x4;
      opts.tex_r32x2 = so->r32x2;
      /* a coordinate only samples read: the PDS samples from the set
       * itself (a pure-sample DOUTI, source 0x24, below) */
      opts.pds_samples_alone = screen->core->pds_samples;
   }
   /* texels a mediump read takes come back F16 (the sample's T3, below) */
   opts.f16_samples = opts.f16 && nir->info.stage == MESA_SHADER_FRAGMENT &&
                      screen->core->f16_sample_return &&
                      !(sgx_drv_debug & SGX_DEBUG_NO_F16_SAMPLES);
   /* and those of dependent samples (sgx_draw.c sets their T3) */
   opts.dep_f16_samples =
      opts.f16_samples && !(sgx_drv_debug & SGX_DEBUG_NO_DEP_F16_SAMPLES);
   opts.fs_mediump = opts.f16 && nir->info.stage == MESA_SHADER_FRAGMENT &&
                     nir->info.name && !strcmp(nir->info.name, "ff-fs") &&
                     !(sgx_drv_debug & SGX_DEBUG_NO_FF_MEDIUMP);
   NIR_PASS(_, nir, nir_shader_intrinsics_pass, rescale_uniform,
            nir_metadata_control_flow, NULL);
   {
      /* a 1D texture is a 2D one a texel high (sgx_resource): its samples
       * become 2D ones on that row; a rectangle texture's coordinates are
       * divided by its size (lower_txs), which st's fixed-function programs
       * leave to the driver (TFP's rectangle binding: xfwm4's compositor) */
      nir_lower_tex_options tex = {.lower_1d = true,
                                   .lower_srgb = srgb,
                                   .lower_rect = true};

      /* a sampler view's swizzle, as the texel comes out (sgx_fs_for); a
       * shadow sample's goes on its result (lower_shadow) */
      if (shadow)
         lower_shadow(nir, compare, swz);
      for (unsigned u = 0; swz && u < PIPE_MAX_SAMPLERS; u++) {
         if (!swz[u] || (shadow >> u & 1))
            continue;
         tex.swizzle_result |= 1u << u;
         for (unsigned c = 0; c < 4; c++)
            tex.swizzles[u][c] = swz[u] >> (3 * c) & 7;
      }
      if (srgb_filter)
         NIR_PASS(_, nir, nir_shader_instructions_pass, lower_srgb_filter,
                  nir_metadata_control_flow, &srgb_filter);
      NIR_PASS(_, nir, nir_lower_tex, &tex);
   }
   /* textureGrad, the Offset variants and texelFetch as smp can do them;
    * the sizes they ask for are lower_txs's */
   if (opts.native_integers)
      NIR_PASS(_, nir, sgx_lower_tex, &opts);
   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS(_, nir, nir_shader_instructions_pass, lower_txs,
               nir_metadata_control_flow, &opts.native_integers);
      NIR_PASS(_, nir, nir_opt_dce);
   } else {
      NIR_PASS(_, nir, nir_shader_intrinsics_pass, lower_instance_id,
               nir_metadata_control_flow, &opts.native_integers);
      NIR_PASS(_, nir, nir_opt_dce);
   }
   remap_legacy_varyings(nir);
   /* (names the variables the way the list does) */
   assign_varyings(nir, &vary, false);
   bool sprite = fs_vary && fs_vary->point_size;
   if (fs_vary) {
      bool psize = vary.point_size;

      vary = *fs_vary;
      vary.point_size = sprite || psize;
   }
   so->reads_pointcoord =
      nir->info.stage == MESA_SHADER_FRAGMENT && vary.point_size;
   if (!so->vary)
      so->vary = MALLOC_STRUCT(sgx_varyings);
   if (so->vary)
      *so->vary = fs_vary ? *fs_vary : vary;
   struct sgx_layout layout = {layout_place, layout_first_free, nir};

   /* gl_FrontFacing against the winding gallium calls front, sgx_draw.c writes
    * it */
   opts.front_sa = SGX_SA_FRONT;

   opts.layout = &layout;
   if (vertex) {
      /* a matrix attribute arrives as a variable per column, all with the
       * matrix's name: name the later columns apart so each one gets its own
       * slot */
      nir_foreach_shader_in_variable (var, nir) {
         nir_foreach_shader_in_variable (prev, nir) {
            if (prev == var)
               break;
            if (var->name && prev->name && !strcmp(var->name, prev->name)) {
               var->name = ralloc_asprintf(var, "%s#%d", var->name,
                                           var->data.driver_location);
               break;
            }
         }
      }
      nir_foreach_shader_in_variable (var, nir) {
         if (var->name)
            so->n_attribs = MAX2(
               so->n_attribs, var->data.driver_location +
                                 glsl_count_attribute_slots(var->type, true));
      }
   }
   if (sgx_drv_debug & SGX_DEBUG_NIR)
      nir_print_shader(nir, stderr);
   /* a discarding fragment program in two phases, as the DDK runs its
    * alpha-tested ones: the test before the visibility feedback, the colour
    * after it for the pixels that pass (out.code1); the compiler decides */
   opts.split_phases = !vertex && !(sgx_drv_debug & SGX_DEBUG_NO_PHASE_SPLIT);
   /* an integer colour buffer: output 0 packed for it, as one word */
   if (!vertex && so->rt_int) {
      unsigned kind = so->rt_int;

      if (pack_int_outputs(nir, kind))
         opts.rt_packed = 1;
      if (sgx_drv_debug & SGX_DEBUG_NIR) {
         fprintf(stderr, "sgx: after pack_int_outputs (kind %u):\n", kind);
         nir_print_shader(nir, stderr);
      }
   }
   if (sgx_compile(nir, &opts, &vary, &out)) {
      fprintf(stderr, "sgx: %s shader did not compile:\n%s\n",
              vertex ? "vertex" : "fragment", out.log ? out.log : "");
      sgx_shader_free(&out);
      ralloc_free(nir);
      return false;
   }
   if (sgx_dump_code_dir()) {
      static unsigned seq;
      char path[512];
      FILE *fp;

      /* "hi lo" hex per instruction, as sgxc's .code (dis.py -x reads the
       * pairs) */
      snprintf(path, sizeof(path), "%s/%s-%u.code", sgx_dump_code_dir(),
               vertex ? "vs" : "fs", seq);
      if ((fp = fopen(path, "w"))) {
         for (unsigned i = 0; i < out.n_code; i++)
            fprintf(fp, "%08x %08x\n", (unsigned)(out.code[i] >> 32),
                    (unsigned)out.code[i]);
         fclose(fp);
      }
      /* a split program's phase 1, as sgxc -P names it */
      snprintf(path, sizeof(path), "%s/fs-%u.code1", sgx_dump_code_dir(), seq);
      if (out.n_code1 && (fp = fopen(path, "w"))) {
         for (unsigned i = 0; i < out.n_code1; i++)
            fprintf(fp, "%08x %08x\n", (unsigned)(out.code1[i] >> 32),
                    (unsigned)out.code1[i]);
         fclose(fp);
      }
      seq++;
   }
   /* phase 1 is straight-line (the compiler splits only such programs): no
    * branch offsets to move there */
   for (unsigned i = 0; i < out.n_blocks1; i++) {
      if (out.blocks1[i].kind == SGX_BLOCK_BRANCH) {
         info.why = "a branch in phase 1";
         out.n_code1 = 0;
         break;
      }
   }
   if (info.why || !read_program(&out, so, &info)) {
      fprintf(stderr, "sgx: %s shader: not handled yet (%s)\n",
              vertex ? "vertex" : "fragment", info.why);
      sgx_shader_free(&out);
      ralloc_free(nir);
      return false;
   }

   {
      /* An instruction names an SA register in 7 bits: a program's constants
       * (from sa9, sa10 in a vertex program) and a fragment program's
       * texture state (after them) end at sa127.  Past it the numbers wrap,
       * onto registers the program never loaded, and the 3D locks up (piglit
       * glsl-uniform-interstage-limits).  The compiler moves constants that
       * don't fit to memory (mem_dwords), so this only catches a program
       * whose SA use still runs past (a guard, not drawn). */
      unsigned end = (vertex ? 10 : 9) + MAX2(so->const_dwords, out.sa_slots) +
                     (vertex ? 0 : 4 * info.n_deps);

      if (end > 128) {
         fprintf(
            stderr,
            "sgx: %s shader needs SA registers up to sa%u (sa127 is the last): not drawn\n",
            vertex ? "vertex" : "fragment", end - 1);
         sgx_shader_free(&out);
         ralloc_free(nir);
         return false;
      }
   }

   uint32_t old_code = so->prog.code;

   memset(&so->prog, 0, sizeof(so->prog));
   simple_mtx_lock(&screen->lock);
   sgxws_code_free(ctx->hw, old_code);
   if (vertex) {
      unsigned dwords = 4, tsp[SGX_MAX_SETS] = {0};

      so->prog.vertex = true;
      so->prog.code =
         sgxws_upload_vs(ctx->hw, out.code, out.n_code, out.n_temps);
      so->prog.in_dwords = so->prog.attr_dwords = 4 * MAX2(so->n_attribs, 1);
      for (unsigned s = 0; s < SGX_MAX_SETS; s++) {
         unsigned n = vary.set_comps[s];

         if (!n)
            continue;
         dwords += n;
         /* TSP word 0: 1, 2, 3, or 5 for 4 components (STATE.md section 3) */
         tsp[s] = n == 4 ? 5 : n;
      }
      STATIC_ASSERT(SGX_MAX_SETS == 10);
      sgx_pack(&so->prog.tsp_sizes, TA_STATE_TSP0, t0)
      {
         t0.set0 = tsp[0];
         t0.set1 = tsp[1];
         t0.set2 = tsp[2];
         t0.set3 = tsp[3];
         t0.set4 = tsp[4];
         t0.set5 = tsp[5];
         t0.set6 = tsp[6];
         t0.set7 = tsp[7];
         t0.set8 = tsp[8];
         t0.set9 = tsp[9];
      }
      so->prog.point_size = vary.point_size;
      so->prog.colour = vary.colour;
      /* TSP word 1 stays 0, as the DDK's for a sampled set: with a set's bit
       * in it, the TPU samples at a constant coordinate; the DDK's for a
       * sprite's coordinate set (`sprite`) is 1 */
      sgx_pack(&so->prog.tsp_prec, TA_STATE_TSP1, t1) t1.f16 = sprite ? 1 : 0;
      /* user clip planes 0..n_clip-1 (st's clip distances), their results
       * after the point size */
      so->prog.clip_planes = BITFIELD_MASK(MIN2(out.n_clip, 6));
      so->prog.out_dwords = dwords + (vary.point_size ? 1 : 0) + out.n_clip;
      so->prog.const_dwords = so->const_dwords;
   } else {
      /* dependent samples: their state words go in SA after the constants */
      if (info.n_deps) {
         unsigned sa[SGX_MAX_DEP_SAMPLES], n;

         if (MAX2(so->const_dwords, out.sa_slots) + 4 * info.n_deps >
             SGX_MAX_CONSTS) {
            fprintf(stderr,
                    "sgx: %u SA slots and %u dependent samples: not drawn\n",
                    out.sa_slots, info.n_deps);
            simple_mtx_unlock(&screen->lock);
            sgx_shader_free(&out);
            ralloc_free(nir);
            return false;
         }
         /* after every SA register the program uses: its constants (list C)
          * and the slots the compiler keeps for itself */
         if (out.sa_slots > so->const_dwords)
            SGX_DBG(NIR, "fragment program: %u SA slots, %u in list C\n",
                    out.sa_slots, so->const_dwords);
         so->const_dwords = MAX2(so->const_dwords, out.sa_slots);
         for (unsigned j = 0; j < info.n_deps; j++) {
            so->dep_sa[j] = 9 + so->const_dwords + 4 * j;
            so->dep_sampler[j] = info.deps[j].sampler;
            so->dep_f16[j] = info.deps[j].f16;
            so->dep_fetch[j] = info.deps[j].fetch;
            so->dep_plane[j] = info.deps[j].plane;
            sa[j] = so->dep_sa[j];
         }
         so->n_deps = info.n_deps;
         so->const_dwords += 4 * info.n_deps;
         if (out.n_code1) {
            /* each phase's dependent samples into its own block */
            struct prog_info i0 = info, i1 = info;
            unsigned sa0[SGX_MAX_DEP_SAMPLES], sa1[SGX_MAX_DEP_SAMPLES], n1;

            i0.n_deps = i1.n_deps = 0;
            i1.n_branches = 0;
            for (unsigned j = 0; j < info.n_deps; j++) {
               struct prog_info *pi = info.deps[j].phase ? &i1 : &i0;
               unsigned *ps = info.deps[j].phase ? sa1 : sa0;

               ps[pi->n_deps] = sa[j];
               pi->deps[pi->n_deps++] = info.deps[j];
            }
            so->code = insert_samples(out.code, out.n_code, &i0, sa0, &n);
            so->code1 = insert_samples(out.code1, out.n_code1, &i1, sa1, &n1);
            if (!so->code || !so->code1) {
               free(so->code);
               free(so->code1);
               so->code = so->code1 = NULL;
            }
            so->n_code1 = so->code1 ? n1 : 0;
         } else {
            so->code = insert_samples(out.code, out.n_code, &info, sa, &n);
         }
         so->n_code = so->code ? n : 0;
      } else if (out.n_code1) {
         /* phase 0's code as the program's (it ends at the feedback nop, so
          * the discard handling below applies), phase 1's after the feedback */
         so->code = malloc(out.n_code * sizeof(*so->code));
         so->code1 = malloc(out.n_code1 * sizeof(*so->code1));
         if (so->code && so->code1) {
            memcpy(so->code, out.code, out.n_code * sizeof(*so->code));
            memcpy(so->code1, out.code1, out.n_code1 * sizeof(*so->code1));
            so->n_code = out.n_code;
            so->n_code1 = out.n_code1;
         } else {
            free(so->code);
            free(so->code1);
            so->code = so->code1 = NULL;
         }
      } else {
         so->code = malloc(MAX2(out.n_code, 1) * sizeof(*so->code));
         if (so->code) {
            memcpy(so->code, out.code, out.n_code * sizeof(*so->code));
            so->n_code = out.n_code;
         }
      }
      /* a program that discards: its feedback nop is the last instruction
       * (the compiler's layout); the depth plane goes in after the iterated
       * inputs, by one more PDS iteration. The compiled program's discards,
       * not the shader's: a sampler view's swizzle can fold one away (an RGB
       * texture's alpha is 1) */
      so->discard_pa = -1;
      so->depth_pa = -1;
      if (out.discards) {
         if (!so->n_code || so->code[so->n_code - 1] != (uint64_t)0xf8000140u
                                                           << 32) {
            fprintf(
               stderr,
               "sgx: fragment shader with discard: no feedback point, not drawn\n");
            free(so->code);
            free(so->code1);
            so->code = so->code1 = NULL;
         }
      }
      /* gl_FragDepth waits in pa<depth_pa> at the feedback nop for a depthf
       * there (ISA.md, test H10) */
      if (out.writes_depth && so->code && so->n_code &&
          so->code[so->n_code - 1] == (uint64_t)0xf8000140u << 32) {
         so->depth_pa = out.depth_pa;
      } else if (out.writes_depth && !ctx->warned_frag_depth) {
         fprintf(stderr, "sgx: gl_FragDepth with no feedback point: the "
                         "triangle's depth is used\n");
         ctx->warned_frag_depth = true;
      }
      so->uses_front_face = out.uses_front_face;
      so->color_reg = out.colour_pa;
      so->temps = out.n_temps;
      so->prog.temps = out.n_temps;
      so->prog.pa_dwords = out.n_pa;
      so->prog.sec_dwords = so->const_dwords ? 9 + so->const_dwords : 0;
      /* One DOUTI per iteration, in PA order (docs/PDS.md "DOUTI"): a
       * varying's iterator is its set + 1, or, with the vertex colour in set
       * 0, the set itself (the PDS numbers the sets after the colour from 1:
       * seen for a sample, an iteration assumed alike).
       * The DDK's driver also iterates a
       * placeholder colour (source 0x20) in a shader without varyings; that
       * hangs the GPU when the vertex program outputs no colour, and ours
       * never do, so there is none. */
      int last_plain = -1, last_sample = -1;

      for (unsigned i = 0; i < out.n_iters && i < ARRAY_SIZE(so->prog.douti);
           i++) {
         unsigned it = out.iters[i].set + 1;
         unsigned tc = out.iters[i].set + (vary.colour ? 0 : 1);

         if (out.iters[i].sample) {
            /* sample half: coordinate set + 1, 2-D or projected (docs/PDS.md).
             * The sample half is independent of the iterate half, so a sample
             * is merged into the previous DOUTI when that one is a plain
             * iteration of a coordinate set, of this set or another, as the
             * DDK's generator does: its texel lands after that iteration, where
             * a DOUTI of its own would put it.  Not into gl_FragCoord's or a
             * point coordinate's (its w1 bits are the sample half's), nor a
             * flat one's (b25-26 2 may take the sample's coordinate too).
             * Otherwise a pure sample (source 0x24). */
            bool proj = out.iters[i].proj;
            unsigned prev = so->prog.n_douti - 1;
            struct sgx_pds_douti *d =
               so->prog.n_douti ? &so->prog.douti[prev] : NULL;
            bool other_set = !(sgx_drv_debug & SGX_DEBUG_NO_PDS_MERGE);

            if (d && !so->prog.douti_tex[prev] && !d->sample_set && !d->dim2 &&
                !d->proj && d->iterate && !d->w1 && d->b25_26 == 3 &&
                (d->src == tc ||
                 (other_set && d->src >= 1 && d->src <= SGX_MAX_SETS))) {
               d->sample_set = tc;
               d->dim2 = !proj;
               d->proj = proj;
               so->prog.douti_tex[prev] = out.iters[i].sampler + 1;
               last_sample = prev;
            } else {
               last_sample = so->prog.n_douti;
               so->prog.douti_tex[so->prog.n_douti] = out.iters[i].sampler + 1;
               so->prog.douti[so->prog.n_douti++] = (struct sgx_pds_douti){
                  .sample_set = tc,
                  .dim2 = !proj,
                  .proj = proj,
                  .src = 0x24,
                  .b25_26 = 3,
               };
            }
            /* returned F16, two registers (opts.f16_samples): the DDK's T3
             * for a sample its program does math on, as a whole word */
            if (out.iters[i].f16) {
               so->prog.douti_t3_clr[last_sample] = ~0u;
               so->prog.douti_t3_set[last_sample] = 0x01028800u;
            }
            continue;
         }
         /* gl_FragCoord (the compiler's set 13) is source 0: x and y at pixel
          * centres in the target's memory order, z, 1/w (docs/PDS.md "DOUTI") */
         if (it == SGX_FRAGCOORD_ITER)
            tc = 0;
         /* set 0 as the vertex colour: source 0x20, four components into
          * one register as BGRA8 */
         bool colour =
            vary.colour && out.iters[i].set == 0 && !out.iters[i].pointcoord;

         last_plain = so->prog.n_douti;
         so->prog.douti[so->prog.n_douti++] = (struct sgx_pds_douti){
            .src = colour ? 0x20 : tc,
            .comps = colour ? 4 : out.iters[i].comps,
            .iterate = true,
            /* 2: the provoking (last) vertex's value, 3 interpolated; only
             * the colour source iterates flat */
            .b25_26 = out.iters[i].flat ? 2 : 3,
            .prec1 = out.iters[i].f16,
            .w1 = out.iters[i].pointcoord ? 0x10 : 0,
         };
      }
      if (so->code && out.discards) {
         /* the triangle's depth plane (source 0x22, 3 components) after the
          * inputs: pa<n> on, n = the registers the iterations above fill (an
          * F32 component 1, an F16 pair 1, a sampled texel or the vertex
          * colour 1) */
         unsigned pa = 0;

         for (unsigned i = 0; i < so->prog.n_douti; i++) {
            const struct sgx_pds_douti *d = &so->prog.douti[i];

            if (d->iterate)
               pa += d->src == 0x20 ? 1
                     : d->prec1     ? (d->comps + 1) / 2
                                    : d->comps;
            if (d->dim2 || d->proj)
               pa++;
         }
         /* (an F16 sample fills two) */
         for (unsigned i = 0; i < out.n_iters; i++)
            pa = MAX2(pa, (unsigned)out.iters[i].pa + out.iters[i].regs);
         if (so->prog.n_douti == ARRAY_SIZE(so->prog.douti)) {
            free(so->code);
            free(so->code1);
            so->code = so->code1 = NULL;
         } else {
            last_plain = so->prog.n_douti;
            so->prog.douti[so->prog.n_douti++] = (struct sgx_pds_douti){
               .src = 0x22,
               .comps = 3,
               .iterate = true,
               .b25_26 = 3,
            };
            so->discard_pa = pa;
            so->prog.pa_dwords = MAX2(so->prog.pa_dwords, pa + 3);
            /* the visibility test's words in sa3, sa4 come with the secondary
             * DMA */
            so->prog.sec_dwords = 9 + so->const_dwords;
         }
      }
      if (so->code && so->depth_pa >= 0) {
         /* depthf reads its depth, and the depth words in sa3, sa4 */
         so->prog.pa_dwords = MAX2(so->prog.pa_dwords, so->depth_pa + 1);
         so->prog.sec_dwords = 9 + so->const_dwords;
      }
      struct sgxws_fs_feedback fb = {so->discard_pa, so->depth_pa};
      so->prog.code =
         !so->code ? 0
         : so->code1 && so->discard_pa >= 0
            ? sgxws_upload_fs_split(ctx->hw, so->code, so->n_code, so->code1,
                                    so->n_code1, so->color_reg, NULL, &fb,
                                    so->temps)
            : sgxws_upload_fs(ctx->hw, so->code, so->n_code, so->color_reg,
                              NULL, &fb, so->temps);
      if (last_plain >= 0)
         so->prog.douti[last_plain].last_iter = true;
      if (last_sample >= 0)
         so->prog.douti[last_sample].last_sample = true;
   }
   simple_mtx_unlock(&screen->lock);
   if (sgx_drv_debug & SGX_DEBUG_NIR)
      fprintf(
         stderr,
         "sgx: compiled %s shader: %u instructions, %u temps, %u constants, %u iterators\n",
         vertex ? "vertex" : "fragment", out.n_code, out.n_temps, so->n_consts,
         out.n_iters);
   so->kind = so->prog.code ? SGX_SHADER_COMPILED : SGX_SHADER_UNSUPPORTED;
   sgx_shader_free(&out);
   ralloc_free(nir);
   return so->kind == SGX_SHADER_COMPILED;
}

static void *
sgx_create_shader_state(struct pipe_context *pctx,
                        const struct pipe_shader_state *cso)
{
   struct sgx_context *ctx = sgx_context(pctx);
   struct sgx_pipe_shader *so = CALLOC_STRUCT(sgx_pipe_shader);
   nir_shader *nir;

   if (!so)
      return NULL;
   so->discard_pa = -1;
   so->depth_pa = -1;
   if (cso->type == PIPE_SHADER_IR_NIR)
      nir = cso->ir.nir;
   else
      nir = tgsi_to_nir(cso->tokens, pctx->screen, false);
   so->nir = nir;
   nir_foreach_shader_out_variable (var, nir)
      so->writes_psize |= var->data.location == VARYING_SLOT_PSIZ;

   if (!nir->info.io_lowered) {
      uint64_t t = os_time_get_nano();
      bool ok = compile_shader(ctx, so, nir, NULL, 0, 0, NULL, 0, NULL, false,
                               0, NULL);

      ctx->stats.compile_ns += os_time_get_nano() - t;
      if (ok)
         return so;
   }

   if (!nir->info.io_lowered)
      NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
               type_size, 0);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_dce);

   if (nir->info.stage == MESA_SHADER_VERTEX)
      match_vs(so);
   else if (nir->info.stage == MESA_SHADER_FRAGMENT)
      match_fs(so);

   if ((sgx_drv_debug & SGX_DEBUG_NIR) || (!so->kind && !ctx->warned_shader)) {
      if (!so->kind)
         fprintf(stderr,
                 "sgx: no program for this %s shader yet (drawn as nothing):\n",
                 mesa_shader_stage_name(nir->info.stage));
      else
         fprintf(stderr, "sgx: %s shader -> fixed program %d\n",
                 mesa_shader_stage_name(nir->info.stage), so->kind);
      if (!so->kind || (sgx_drv_debug & SGX_DEBUG_NIR))
         nir_print_shader(nir, stderr);
      if (!so->kind)
         ctx->warned_shader = !(sgx_drv_debug & SGX_DEBUG_NIR);
   }
   return so;
}

static void
sgx_bind_vs_state(struct pipe_context *pctx, void *hwcso)
{
   sgx_context(pctx)->vs = hwcso;
}

static void
sgx_bind_fs_state(struct pipe_context *pctx, void *hwcso)
{
   sgx_context(pctx)->fs = hwcso;
}

/* a shader's programs (its blend variants too) go: the winsys frees their
 * code once no render runs it */
static void
free_programs(struct sgx_context *ctx, struct sgx_pipe_shader *so)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);

   if (!ctx->hw)
      return;
   simple_mtx_lock(&screen->lock);
   sgxws_code_free(ctx->hw, so->prog.code);
   for (unsigned i = 0; i < so->n_variants; i++)
      sgxws_code_free(ctx->hw, so->variants[i].code);
   simple_mtx_unlock(&screen->lock);
}

/* a variant (sgx_vs_for, sgx_fs_for) */
static void
destroy_variant(struct sgx_context *ctx, struct sgx_pipe_shader *v)
{
   if (!v)
      return;
   free_programs(ctx, v);
   free(v->code);
   free(v->code1);
   FREE(v->vary);
   FREE(v);
}

static void
sgx_delete_shader_state(struct pipe_context *pctx, void *hwcso)
{
   struct sgx_context *ctx = sgx_context(pctx);
   struct sgx_pipe_shader *so = hwcso;

   for (unsigned i = 0; i < ARRAY_SIZE(so->variants_vs); i++)
      destroy_variant(ctx, so->variants_vs[i]);
   for (unsigned i = 0; i < ARRAY_SIZE(so->variants_srgb); i++)
      destroy_variant(ctx, so->variants_srgb[i]);
   ralloc_free(so->nir);
   so->nir = NULL;
   destroy_variant(ctx, so);
}

struct sgx_pipe_shader *
sgx_vs_for(struct sgx_context *ctx, struct sgx_pipe_shader *vs,
           const struct sgx_pipe_shader *fs, const uint8_t *vtx_fmt,
           bool points)
{
   static const uint8_t f32x4[16];
   const struct sgx_varyings *vary = fs && fs->vary ? fs->vary : vs->vary;
   struct sgx_pipe_shader **slot;

   if (!vtx_fmt)
      vtx_fmt = f32x4;
   points = points && vs->writes_psize;
   if ((!vs->vary || !vary || same_varyings(vs->vary, vary)) &&
       !memcmp(vtx_fmt, f32x4, sizeof(f32x4)) && !points)
      return vs;
   for (unsigned i = 0; i < ARRAY_SIZE(vs->variants_vs); i++) {
      struct sgx_pipe_shader *v = vs->variants_vs[i];

      if (v && v->points == points &&
          (!vary || (same_varyings(v->vary, vary) &&
                     v->vary->point_size == vary->point_size)) &&
          !memcmp(v->vtx_fmt, vtx_fmt, sizeof(v->vtx_fmt)))
         return v;
   }
   if (!vs->nir)
      return NULL;
   /* the oldest goes */
   slot = &vs->variants_vs[vs->next_variant_vs++ % ARRAY_SIZE(vs->variants_vs)];
   destroy_variant(ctx, *slot);
   if (!(*slot = CALLOC_STRUCT(sgx_pipe_shader)))
      return NULL;
   (*slot)->discard_pa = -1;
   (*slot)->depth_pa = -1;
   (*slot)->points = points;
   if (!compile_shader(ctx, *slot, vs->nir, fs ? fs->vary : NULL, 0, 0, NULL, 0,
                       vtx_fmt, points, 0, NULL) ||
       !(*slot)->prog.code) {
      FREE((*slot)->vary);
      FREE(*slot);
      *slot = NULL;
   }
   return *slot;
}

/* the texture units a fragment shader samples (all, when it isn't known) */
uint32_t
sgx_fs_textures_used(const struct sgx_pipe_shader *fs)
{
   uint32_t mask = 0;
   unsigned unit;

   if (fs->kind != SGX_SHADER_COMPILED || !fs->nir)
      return ~0u;
   BITSET_FOREACH_SET (unit, fs->nir->info.textures_used, PIPE_MAX_SAMPLERS)
      mask |= 1u << unit;
   return mask;
}

struct sgx_pipe_shader *
sgx_fs_for(struct sgx_context *ctx, struct sgx_pipe_shader *fs)
{
   struct sgx_pipe_shader **slot;
   unsigned srgb = 0, unit;
   uint32_t srgb_filter = 0;
   uint16_t swz[PIPE_MAX_SAMPLERS] = {0};
   uint32_t not_bgra = 0, depth = 0;
   uint8_t compare[PIPE_MAX_SAMPLERS] = {0};
   bool any_swz = false;
   uint8_t rt_int = 0;
   uint32_t r32x4 = 0, r32x2 = 0;

   if (fs->kind != SGX_SHADER_COMPILED || !fs->nir)
      return fs;
   /* an integer colour buffer takes its own packing (pack_int_outputs) */
   if (ctx->framebuffer.nr_cbufs && ctx->framebuffer.cbufs[0].texture &&
       util_format_is_pure_integer(ctx->framebuffer.cbufs[0].format))
      rt_int = sgx_tex_raw32(ctx->framebuffer.cbufs[0].format) > 1
                  ? 3 + ctx->rt_plane /* a plane: channel rt_plane */
               : sgx_tex_raw32(ctx->framebuffer.cbufs[0].format) ? 2
                                                                 : 1;
   BITSET_FOREACH_SET (unit, fs->nir->info.textures_used, PIPE_MAX_SAMPLERS) {
      const struct pipe_sampler_view *v = ctx->views[unit];

      if (!v)
         continue;
      if (util_format_is_srgb(v->format)) {
         const struct pipe_sampler_state *ss = ctx->samplers[unit];

         srgb |= 1u << unit;
         /* filtered at one level: decoded before it's filtered */
         if (ss &&
             (ss->mag_img_filter == PIPE_TEX_FILTER_LINEAR ||
              ss->min_img_filter == PIPE_TEX_FILTER_LINEAR) &&
             (ss->min_mip_filter == PIPE_TEX_MIPFILTER_NONE ||
              v->u.tex.first_level == v->u.tex.last_level))
            srgb_filter |= 1u << unit;
      }
      if (!sgx_tex_bgra(v->format))
         not_bgra |= 1u << unit;
      /* one F32 word a texel (tex_f32): depth, and R32 (raw); RG32 and
       * RGBA32 a read a plane (tex_r32x2 / x4) */
      if (sgx_tex_raw32(v->format) == 1)
         depth |= 1u << unit;
      if (sgx_tex_raw32(v->format) == 4)
         r32x4 |= 1u << unit;
      else if (sgx_tex_raw32(v->format) == 2)
         r32x2 |= 1u << unit;
      if (sgx_tex_depth(v->format)) {
         const struct pipe_sampler_state *ss = ctx->samplers[unit];

         depth |= 1u << unit;
         if (ss && ss->compare_mode == PIPE_TEX_COMPARE_R_TO_TEXTURE)
            compare[unit] = ss->compare_func + 1;
      }
      /* XYZW: none, 0 */
      if (v->swizzle_r != PIPE_SWIZZLE_X || v->swizzle_g != PIPE_SWIZZLE_Y ||
          v->swizzle_b != PIPE_SWIZZLE_Z || v->swizzle_a != PIPE_SWIZZLE_W) {
         /* b15 marks one: XXXX packs to 0 */
         swz[unit] = 0x8000 | v->swizzle_r | v->swizzle_g << 3 |
                     v->swizzle_b << 6 | v->swizzle_a << 9;
         any_swz = true;
      }
   }
   if (!srgb && !any_swz && !not_bgra && !rt_int && !r32x4 && !r32x2)
      return fs;
   for (unsigned i = 0; i < ARRAY_SIZE(fs->variants_srgb); i++) {
      if (fs->variants_srgb[i] && fs->variants_srgb[i]->srgb == srgb &&
          fs->variants_srgb[i]->srgb_filter == srgb_filter &&
          fs->variants_srgb[i]->not_bgra == not_bgra &&
          fs->variants_srgb[i]->depth == depth &&
          fs->variants_srgb[i]->rt_int == rt_int &&
          fs->variants_srgb[i]->r32x4 == r32x4 &&
          fs->variants_srgb[i]->r32x2 == r32x2 &&
          !memcmp(fs->variants_srgb[i]->compare, compare, sizeof(compare)) &&
          !memcmp(fs->variants_srgb[i]->swz, swz, sizeof(swz)))
         return fs->variants_srgb[i];
   }
   /* the oldest goes */
   slot = &fs->variants_srgb[fs->next_variant_srgb++ %
                             ARRAY_SIZE(fs->variants_srgb)];
   destroy_variant(ctx, *slot);
   if (!(*slot = CALLOC_STRUCT(sgx_pipe_shader)))
      return NULL;
   (*slot)->discard_pa = -1;
   (*slot)->depth_pa = -1;
   (*slot)->srgb = srgb;
   (*slot)->srgb_filter = srgb_filter;
   memcpy((*slot)->swz, swz, sizeof(swz));
   (*slot)->not_bgra = not_bgra;
   (*slot)->depth = depth;
   (*slot)->rt_int = rt_int;
   (*slot)->r32x4 = r32x4;
   (*slot)->r32x2 = r32x2;
   memcpy((*slot)->compare, compare, sizeof(compare));
   if (!compile_shader(ctx, *slot, fs->nir, NULL, srgb, srgb_filter, swz,
                       not_bgra, NULL, false, depth, compare) ||
       !(*slot)->code) {
      FREE((*slot)->vary);
      FREE(*slot);
      *slot = NULL;
   }
   return *slot;
}

/* the compiled fragment shader's program with its colour blended (b), or
 * without (NULL); 0 when there is none */

uint32_t
sgx_fs_program(struct sgx_context *ctx, struct sgx_pipe_shader *so,
               const struct sgxws_blend *b)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);
   uint32_t va;

   if (!b)
      return so->prog.code;
   for (unsigned i = 0; i < so->n_variants; i++) {
      if (!memcmp(&so->variants[i].key, b, sizeof(*b)))
         return so->variants[i].code;
   }
   if (!so->code)
      return 0;
   simple_mtx_lock(&screen->lock);
   struct sgxws_fs_feedback fb = {so->discard_pa, so->depth_pa};
   va = so->code1 && so->discard_pa >= 0
           ? sgxws_upload_fs_split(ctx->hw, so->code, so->n_code, so->code1,
                                   so->n_code1, so->color_reg, b, &fb,
                                   so->temps)
           : sgxws_upload_fs(ctx->hw, so->code, so->n_code, so->color_reg, b,
                             &fb, so->temps);
   /* past the table's size, the slots are reused in turn, their programs
    * freed (dEQP's fragment_ops.interaction draws one shader with dozens of
    * blends) */
   if (va) {
      unsigned i = so->n_variants < ARRAY_SIZE(so->variants)
                      ? so->n_variants++
                      : so->next_variant++ % ARRAY_SIZE(so->variants);

      sgxws_code_free(ctx->hw, so->variants[i].code); /* 0: a new slot */
      so->variants[i].key = *b;
      so->variants[i].code = va;
   }
   simple_mtx_unlock(&screen->lock);
   return va;
}

void
sgx_program_init(struct sgx_context *ctx)
{
   ctx->base.create_vs_state = sgx_create_shader_state;
   ctx->base.bind_vs_state = sgx_bind_vs_state;
   ctx->base.delete_vs_state = sgx_delete_shader_state;
   ctx->base.create_fs_state = sgx_create_shader_state;
   ctx->base.bind_fs_state = sgx_bind_fs_state;
   ctx->base.delete_fs_state = sgx_delete_shader_state;
}
