/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

/*
 * Shaders.  Until the NIR -> USSE backend (src/sgx545/compiler) is wired in,
 * a shader is matched against the fixed programs the winsys carries
 * (re/gen/mkusse.py): a vertex shader that writes attribute 0 to
 * gl_Position, a fragment shader that writes one vec4 uniform or constant.
 * Anything else is not drawn.
 */

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "nir/tgsi_to_nir.h"
#include "util/u_memory.h"

#include "sgx545_context.h"
#include "sgx545_screen.h"
#include "sgx545_program.h"

static const nir_shader_compiler_options sgx545_nir_options = {
   .lower_fdiv = true,
   .lower_fmod = true,
   .lower_fpow = true,
   .lower_fsat = true,
   .lower_flrp32 = true,
   .lower_fdph = true,
   .lower_ftrunc = true,
   .lower_fsign = true,
   .lower_bitops = true,
   .lower_insert_byte = true,
   .lower_insert_word = true,
   .lower_extract_byte = true,
   .lower_extract_word = true,
   .lower_pack_half_2x16 = true,
   .lower_unpack_half_2x16 = true,
   .force_indirect_unrolling = nir_var_all,
   .force_indirect_unrolling_sampler = true,
   .max_unroll_iterations = 32,
   .support_indirect_inputs = 0,
   .support_indirect_outputs = 0,
};

const nir_shader_compiler_options *
sgx545_get_nir_options(void)
{
   return &sgx545_nir_options;
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

   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
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

/* is component c of the stored value channel `comp` of a load intrinsic of this kind? */
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
match_vs(struct sgx545_shader *so)
{
   nir_intrinsic_instr *st = single_output(so->nir), *load = NULL;

   if (!st || nir_intrinsic_io_semantics(st).location != VARYING_SLOT_POS ||
       nir_intrinsic_write_mask(st) != 0xf || nir_intrinsic_component(st) != 0 ||
       st->src[0].ssa->num_components != 4)
      return;
   for (unsigned c = 0; c < 4; c++) {
      unsigned comp;
      nir_intrinsic_instr *l = source_load(st, c, &comp);

      if (!l || l->intrinsic != nir_intrinsic_load_input || nir_intrinsic_base(l) != 0 ||
          !nir_src_is_const(l->src[0]) || nir_src_as_uint(l->src[0]) != 0 ||
          nir_intrinsic_component(l) + comp != c || (load && l != load))
         return;
      load = l;
   }
   so->kind = SGX545_SHADER_VS_POSITION;
}

static void
match_fs(struct sgx545_shader *so)
{
   nir_intrinsic_instr *st = single_output(so->nir), *load = NULL;
   unsigned loc;
   float k[4];
   bool constant = true;

   if (!st)
      return;
   loc = nir_intrinsic_io_semantics(st).location;
   if ((loc != FRAG_RESULT_COLOR && loc != FRAG_RESULT_DATA0) || nir_intrinsic_write_mask(st) != 0xf ||
       st->src[0].ssa->num_components != 4)
      return;
   for (unsigned c = 0; c < 4; c++)
      constant &= source_const(st, c, &k[c]);
   if (constant) {
      memcpy(so->color, k, sizeof(k));
      so->kind = SGX545_SHADER_FS_CONSTANT;
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
      so->ubo_offset = (nir_intrinsic_base(load) + nir_src_as_uint(load->src[0])) * 16;
   so->kind = SGX545_SHADER_FS_UNIFORM;
}

static void *
sgx545_create_shader_state(struct pipe_context *pctx, const struct pipe_shader_state *cso)
{
   struct sgx545_context *ctx = sgx545_context(pctx);
   struct sgx545_shader *so = CALLOC_STRUCT(sgx545_shader);
   nir_shader *nir;

   if (!so)
      return NULL;
   if (cso->type == PIPE_SHADER_IR_NIR)
      nir = cso->ir.nir;
   else
      nir = tgsi_to_nir(cso->tokens, pctx->screen, false);
   so->nir = nir;

   if (!nir->info.io_lowered)
      NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out, type_size, 0);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_dce);

   if (nir->info.stage == MESA_SHADER_VERTEX)
      match_vs(so);
   else if (nir->info.stage == MESA_SHADER_FRAGMENT)
      match_fs(so);

   if ((sgx545_debug & SGX545_DEBUG_NIR) || (!so->kind && !ctx->warned_shader)) {
      if (!so->kind)
         fprintf(stderr, "sgx545: no program for this %s shader yet (drawn as nothing):\n",
                 mesa_shader_stage_name(nir->info.stage));
      else
         fprintf(stderr, "sgx545: %s shader -> fixed program %d\n",
                 mesa_shader_stage_name(nir->info.stage), so->kind);
      if (!so->kind || (sgx545_debug & SGX545_DEBUG_NIR))
         nir_print_shader(nir, stderr);
      if (!so->kind)
         ctx->warned_shader = !(sgx545_debug & SGX545_DEBUG_NIR);
   }
   return so;
}

static void
sgx545_bind_vs_state(struct pipe_context *pctx, void *hwcso)
{
   sgx545_context(pctx)->vs = hwcso;
}

static void
sgx545_bind_fs_state(struct pipe_context *pctx, void *hwcso)
{
   sgx545_context(pctx)->fs = hwcso;
}

static void
sgx545_delete_shader_state(struct pipe_context *pctx, void *hwcso)
{
   struct sgx545_shader *so = hwcso;

   ralloc_free(so->nir);
   FREE(so);
}

void
sgx545_program_init(struct sgx545_context *ctx)
{
   ctx->base.create_vs_state = sgx545_create_shader_state;
   ctx->base.bind_vs_state = sgx545_bind_vs_state;
   ctx->base.delete_vs_state = sgx545_delete_shader_state;
   ctx->base.create_fs_state = sgx545_create_shader_state;
   ctx->base.bind_fs_state = sgx545_bind_fs_state;
   ctx->base.delete_fs_state = sgx545_delete_shader_state;
}
