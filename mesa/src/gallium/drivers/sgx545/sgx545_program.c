/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

/*
 * Shaders.  The NIR -> USSE backend (src/sgx545/compiler) compiles each one
 * to a USP, as libusc did for the DDK, and this file is the rest of what the
 * DDK's GL driver did with it (docs/ISA.md "The USP container", "What a
 * compiler needs"): read the USP's lists, finalise and upload the programs,
 * and keep what a draw needs: the constants' places in secondary attributes,
 * the attribute slots, the vertex outputs, the fragment iterators.
 *
 * Varyings get texture coordinate set (location - VARYING_SLOT_VAR0) on both
 * sides, so a vertex and a fragment shader compiled apart still agree.
 *
 * Before the compiler (SGX545_NO_COMPILER=1 still selects this), a shader was
 * matched against the fixed programs the winsys carries (tools/gen/mkusse.py):
 * gl_Position = attribute 0, gl_FragColor = one vec4 uniform or constant.
 */

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "nir/tgsi_to_nir.h"
#include "util/u_memory.h"

#include "sgx545_context.h"
#include "sgx545_screen.h"
#include "sgx545_program.h"
#include "sgx_compiler.h"

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

/* ---------------------------------------------------------------- compiled shaders */

/* st hands uniforms over as load_uniform in vec4 slots; the compiler numbers
 * constants in components (constant k = constant buffer 0 float k) */
static bool
rescale_uniform(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
   if (in->intrinsic != nir_intrinsic_load_uniform || !nir_src_is_const(in->src[0]))
      return false;
   b->cursor = nir_before_instr(&in->instr);
   nir_def *v = nir_load_uniform(b, in->def.num_components, in->def.bit_size, nir_imm_int(b, 0),
                                 .base = (nir_intrinsic_base(in) + nir_src_as_uint(in->src[0])) * 4,
                                 .range = nir_intrinsic_range(in) * 4,
                                 .dest_type = nir_intrinsic_dest_type(in));
   nir_def_replace(&in->def, v);
   return true;
}

/* varying -> set location - VAR0, the same on both sides */
static void
assign_varyings(nir_shader *nir, struct sgx_varyings *vary)
{
   nir_variable_mode mode = nir->info.stage == MESA_SHADER_VERTEX ? nir_var_shader_out : nir_var_shader_in;

   memset(vary, 0, sizeof(*vary));
   nir_foreach_variable_with_modes(var, nir, mode) {
      const struct glsl_type *t = var->type;
      unsigned rows, comps;
      struct sgx_varying *v;

      if (var->data.location < VARYING_SLOT_VAR0 || vary->n == ARRAY_SIZE(vary->v))
         continue;
      rows = glsl_count_vec4_slots(t, false, false);
      t = glsl_without_array(t);
      comps = glsl_type_is_matrix(t) ? glsl_get_vector_elements(glsl_get_column_type(t))
                                     : glsl_get_vector_elements(t);
      v = &vary->v[vary->n++];
      snprintf(v->name, sizeof(v->name), "%s", var->name ? var->name : "");
      v->set = var->data.location - VARYING_SLOT_VAR0;
      v->rows = rows;
      v->comps = comps;
      v->precision = var->data.precision;
      for (unsigned r = 0; r < rows && v->set + r < SGX_MAX_SETS; r++)
         vary->set_comps[v->set + r] = comps;
   }
}

static unsigned
rd16(const uint8_t *p, size_t off)
{
   return p[off] | p[off + 1] << 8;
}

static uint32_t
rd32(const uint8_t *p, size_t off)
{
   return p[off] | p[off + 1] << 8 | p[off + 2] << 16 | (uint32_t)p[off + 3] << 24;
}

/* what the draw needs from a USP (docs/ISA.md "The USP container") */
struct usp_info {
   unsigned temps, color_type, color_reg;
   unsigned n_iters;
   struct { unsigned iterator, comps, format; } iters[16];
};

static bool
read_usp(const uint8_t *u, size_t size, struct sgx545_shader *so, struct usp_info *info)
{
   size_t off = 0x84;
   unsigned n_a, n_c, n_d, n_sec;
   uint32_t n_b;

   if (size < 0x84 || rd32(u, 0) != 0x55535020 || rd32(u, 4) != 0x23)
      return false;
   info->temps = rd16(u, 0x46);
   info->color_type = rd16(u, 0x66);
   info->color_reg = rd16(u, 0x68);
   n_a = rd16(u, 0x4e);
   n_c = rd16(u, 0x50);
   n_b = rd32(u, 0x52);
   n_d = rd16(u, 0x56);
   n_sec = rd16(u, 0x7c);
   off += 4 * rd32(u, 0x28);                       /* workaround ids */
   for (unsigned i = 0; i < n_a && off + 12 <= size; i++, off += 12) {
      if (info->n_iters < ARRAY_SIZE(info->iters)) {
         info->iters[info->n_iters].iterator = rd16(u, off + 4);
         info->iters[info->n_iters].comps = rd16(u, off + 6);
         info->iters[info->n_iters].format = rd16(u, off + 8);
         info->n_iters++;
      }
      if (rd16(u, off) & ~0xcu)                    /* only plain iterations so far */
         return false;
   }
   for (uint32_t i = 0; i < n_b && off < size; i++)   /* list B: constants left in memory */
      return false;
   for (unsigned i = 0; i < n_c && off + 8 <= size; i++) {
      unsigned kind = rd16(u, off);
      struct sgx545_const *k = &so->consts[so->n_consts];

      if (kind != 1 || so->n_consts == SGX545_MAX_CONSTS)
         return false;                             /* F16/C10 constants: not in F32 mode */
      k->fmt = kind;
      k->k = rd16(u, off + 2);
      k->slot = rd16(u, off + 4);
      k->shift = rd16(u, off + 6);
      so->const_dwords = MAX2(so->const_dwords, k->slot + 1u);
      so->n_consts++;
      off += 8;
   }
   return n_d == 0 && n_sec == 0 && off <= size;
}

static bool
compile_shader(struct sgx545_context *ctx, struct sgx545_shader *so, nir_shader *orig)
{
   struct sgx545_screen *screen = sgx545_screen(ctx->base.screen);
   nir_shader *nir = nir_shader_clone(NULL, orig);
   struct sgx_compile_options opts = { 0 };
   struct sgx_varyings vary;
   struct sgx_shader out;
   struct usp_info info = { 0 };
   bool vertex = nir->info.stage == MESA_SHADER_VERTEX;

   opts.verbose = (sgx545_debug & SGX545_DEBUG_NIR) != 0;
   NIR_PASS(_, nir, nir_shader_intrinsics_pass, rescale_uniform, nir_metadata_control_flow, NULL);
   assign_varyings(nir, &vary);
   if (vertex) {
      nir_foreach_shader_in_variable(var, nir) {
         if (!var->name || opts.n_attribs == SGX_MAX_NAMED)
            continue;
         snprintf(opts.attribs[opts.n_attribs].name, sizeof(opts.attribs[0].name), "%s", var->name);
         opts.attribs[opts.n_attribs++].value = var->data.driver_location;
         so->n_attribs = MAX2(so->n_attribs, var->data.driver_location + 1);
      }
   }
   if (sgx_compile(nir, &opts, &vary, &out)) {
      fprintf(stderr, "sgx545: %s shader did not compile:\n%s\n", vertex ? "vertex" : "fragment",
              out.log ? out.log : "");
      sgx_shader_free(&out);
      ralloc_free(nir);
      return false;
   }
   if (getenv("SGX545_DUMP_USP")) {
      static unsigned seq;
      char path[512];
      FILE *fp;

      snprintf(path, sizeof(path), "%s/%s-%u.usp", getenv("SGX545_DUMP_USP"), vertex ? "vs" : "fs", seq++);
      if ((fp = fopen(path, "wb"))) {
         fwrite(out.usp, 1, out.usp_size, fp);
         fclose(fp);
      }
   }
   if (!read_usp(out.usp, out.usp_size, so, &info) || (!vertex && info.color_type != 3)) {
      fprintf(stderr, "sgx545: %s shader: USP features not handled yet\n", vertex ? "vertex" : "fragment");
      sgx_shader_free(&out);
      ralloc_free(nir);
      return false;
   }

   memset(&so->prog, 0, sizeof(so->prog));
   simple_mtx_lock(&screen->lock);
   if (vertex) {
      unsigned dwords = 4;

      so->prog.vertex = true;
      so->prog.code = sgxws_upload_vs(ctx->hw, out.code, out.n_code, info.temps);
      so->prog.in_dwords = so->prog.attr_dwords = 4 * MAX2(so->n_attribs, 1);
      for (unsigned s = 0; s < SGX_MAX_SETS; s++) {
         unsigned n = vary.set_comps[s];

         if (!n)
            continue;
         dwords += n;
         /* TSP: 3 bits per set (1, 2, 3, or 5 for 4 components); the sets
          * iterated at full precision (STATE.md section 3) */
         so->prog.tsp_sizes |= (n == 4 ? 5 : n) << (3 * s);
         so->prog.tsp_prec |= 1u << s;
      }
      so->prog.out_dwords = dwords;
      so->prog.const_dwords = so->const_dwords;
   } else {
      so->prog.code = sgxws_upload_fs(ctx->hw, out.code, out.n_code, info.color_reg);
      so->prog.temps = info.temps;
      so->prog.sec_dwords = so->const_dwords ? 9 + so->const_dwords : 0;
      /* One DOUTI per list A entry, in PA order (docs/PDS.md "DOUTI"): a
       * varying's iterator is its set + 1; the writer's other kinds 0xb/0xc
       * iterate the colour (source 0x20, 4 components, as the clear's), 0xd-0xf
       * source 0, as the DDK's draw-time builder does (docs/ISA.md list A). */
      for (unsigned i = 0; i < info.n_iters && i < ARRAY_SIZE(so->prog.douti); i++) {
         unsigned it = info.iters[i].iterator;
         uint32_t w;

         if ((it == 0xb || it == 0xc) && getenv("SGX545_FS_COLOR_ITER"))
            w = 0x20u << 11 | 3u << 22 | 1u << 24;
         else if (it >= 0xb && getenv("SGX545_FS_DUMMY_SRC"))
            w = atoi(getenv("SGX545_FS_DUMMY_SRC")) << 11 | 3u << 22 | 1u << 24 | 0x6000000u;
         else if (it >= 0xb)
            continue;   /* the vertex programs output no colour: nothing to iterate */
         else if (it >= 0xd)
            w = (info.iters[i].comps - 1) << 22 | 1u << 24 | 0x6000000u;
         else
            w = it << 11 | (info.iters[i].comps - 1) << 22 | 1u << 24 | 0x6000000u |
                (info.iters[i].format == 2 ? 1u << 28 : 0);
         so->prog.douti[so->prog.n_douti++] = w;
      }
      if (so->prog.n_douti)
         so->prog.douti[so->prog.n_douti - 1] |= 1u << 29;   /* the last iteration */
   }
   simple_mtx_unlock(&screen->lock);
   if (sgx545_debug & SGX545_DEBUG_NIR)
      fprintf(stderr, "sgx545: compiled %s shader: %u instructions, %u temps, %u constants, %u iterators\n",
              vertex ? "vertex" : "fragment", out.n_code, info.temps, so->n_consts, info.n_iters);
   so->kind = so->prog.code ? SGX545_SHADER_COMPILED : SGX545_SHADER_UNSUPPORTED;
   sgx_shader_free(&out);
   ralloc_free(nir);
   return so->kind == SGX545_SHADER_COMPILED;
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

   /* SGX545_NO_COMPILER=vs / fs / 1: the fixed programs for that stage / both */
   const char *no = getenv("SGX545_NO_COMPILER");
   bool fixed = no && (!strcmp(no, "1") ||
                       !strcmp(no, nir->info.stage == MESA_SHADER_VERTEX ? "vs" : "fs"));

   if (!fixed && !nir->info.io_lowered && compile_shader(ctx, so, nir))
      return so;

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
