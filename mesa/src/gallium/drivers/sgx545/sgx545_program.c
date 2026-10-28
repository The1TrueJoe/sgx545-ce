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
#include "util/os_time.h"
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
   nir_def *v = nir_load_uniform(b, in->def.num_components, in->def.bit_size, nir_imm_int(b, 0),
                                 .base = (nir_intrinsic_base(in) + nir_src_as_uint(in->src[0])) * 4,
                                 .range = nir_intrinsic_range(in) * 4,
                                 .dest_type = nir_intrinsic_dest_type(in));
   nir_def_replace(&in->def, v);
   return true;
}

/*
 * varying -> set location - VAR0, the same on both sides.  Entries are named
 * the way the compiler looks varyings up (sgx_nir.c var_names): st names a
 * packed varying "packed:a,b", one name per varying it holds.
 */
/* Mesa's fixed-function shaders (st's ffvertex and its fragment programs;
 * ES1 through gl4es) pass colours, fog and texture coordinates in the legacy
 * varying slots, which the compiler doesn't assign: move them to fixed VAR
 * slots with fixed names, the same in both stages so that stages compiled
 * apart agree. GLSL ES programs only use VAR slots, so the two never meet. */
static void
remap_legacy_varyings(nir_shader *nir)
{
   static const struct { gl_varying_slot slot; unsigned var; } map[] = {
      { VARYING_SLOT_COL0, 0 }, { VARYING_SLOT_COL1, 1 }, { VARYING_SLOT_FOGC, 2 },
      { VARYING_SLOT_TEX0, 3 }, { VARYING_SLOT_TEX1, 4 }, { VARYING_SLOT_TEX2, 5 },
      { VARYING_SLOT_TEX3, 6 }, { VARYING_SLOT_TEX4, 7 }, { VARYING_SLOT_TEX5, 8 },
      { VARYING_SLOT_TEX6, 9 },
   };
   nir_variable_mode mode = nir->info.stage == MESA_SHADER_VERTEX ? nir_var_shader_out : nir_var_shader_in;
   bool changed = false;

   nir_foreach_variable_with_modes(var, nir, mode) {
      for (unsigned i = 0; i < ARRAY_SIZE(map); i++) {
         char name[32];

         if (var->data.location != (int)map[i].slot)
            continue;
         var->data.location = VARYING_SLOT_VAR0 + map[i].var;
         snprintf(name, sizeof(name), "sgx545_legacy%u", map[i].var);
         var->name = ralloc_strdup(nir, name);
         changed = true;
         break;
      }
   }
   if (changed)
      nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
}

static void
assign_varyings(nir_shader *nir, struct sgx_varyings *vary)
{
   nir_variable_mode mode = nir->info.stage == MESA_SHADER_VERTEX ? nir_var_shader_out : nir_var_shader_in;
   static bool warned;

   memset(vary, 0, sizeof(*vary));
   nir_foreach_variable_with_modes(var, nir, mode) {
      const struct glsl_type *t = var->type;
      unsigned rows, comps, set, frac = var->data.location_frac;
      struct sgx_varying *v;
      char name[32];

      if (var->data.location < VARYING_SLOT_VAR0 || vary->n == ARRAY_SIZE(vary->v))
         continue;
      rows = glsl_count_vec4_slots(t, false, false);
      t = glsl_without_array(t);
      comps = glsl_type_is_matrix(t) ? glsl_get_vector_elements(glsl_get_column_type(t))
                                     : glsl_get_vector_elements(t);
      set = var->data.location - VARYING_SLOT_VAR0;
      /* The compiler finds a varying's set by name.  st's packed slots are
       * named after what they hold ("packed:a,b"), and a varying split over
       * two slots names both: Minecraft's terrain (two vec3 texture
       * coordinates and the fog) had its second slot written over the first.
       * Each variable is named after its slot instead, the same in both
       * stages. */
      snprintf(name, sizeof(name), frac ? "sgx545_v%u_%u" : "sgx545_v%u", set, frac);
      var->name = ralloc_strdup(nir, name);
      if (frac && !warned) {
         /* the compiler puts component j of a variable at its set's j */
         fprintf(stderr, "sgx545: a varying at component %u of its slot: its components land at 0\n", frac);
         warned = true;
      }
      v = &vary->v[vary->n++];
      snprintf(v->name, sizeof(v->name), "%s", name);
      v->set = set;
      v->rows = rows;
      v->comps = comps;
      v->precision = var->data.precision;
      for (unsigned r = 0; r < rows && set + r < SGX_MAX_SETS; r++)
         vary->set_comps[set + r] = MAX2(vary->set_comps[set + r], frac + comps);
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
   struct { unsigned iterator, comps, format, sampler; bool sample, proj; } iters[16];
   const char *why;             /* what read_usp refused */
   /* dependent samples (block 5 without b0): where the smp goes in the flat
    * program (before instruction `at`), its template, sampler and raw texel
    * register */
   struct { unsigned at, sampler, raw; uint64_t smp; } deps[SGX545_MAX_DEP_SAMPLES];
   unsigned n_deps;
   unsigned n_insts;            /* instructions in the flat program, counted from the blocks */
   unsigned sa_slots;           /* SA registers the program uses from its base on (header 0x7e) */
   unsigned n_pa;               /* primary attribute registers (header 0x44) */
   unsigned branch_at[64];      /* flat indices of the branches (blocks 3) */
   unsigned n_branches;
};

/* the block stream: true when every texture sample is non-dependent (the
 * PDS samples into pa; dependent ones need an smp the driver doesn't write yet) */
/* The block stream (ISA.md "USP blocks"): code (2: n instructions), branch
 * (3: one instruction), label (4), sample (5) and its unpack (6), end (7).
 * Counts the flat program's instructions as it goes, so that each dependent
 * sample knows where its smp belongs. */
static bool
blocks_supported(const uint8_t *u, size_t size, size_t off, struct usp_info *info)
{
   while (off + 4 <= size) {
      uint32_t type = rd32(u, off);

      off += 4;
      switch (type) {
      case 2: {
         unsigned n = rd16(u, off);

         info->n_insts += n;
         off += 4 + 16 + 2 * n + 8 * n;
         break;
      }
      case 3:
         if (info->n_branches == ARRAY_SIZE(info->branch_at))
            return info->why = "too many branches", false;
         info->branch_at[info->n_branches++] = info->n_insts++;
         off += 10;
         break;
      case 4: off += 2; break;
      case 5:
         if (!(rd16(u, off) & 1)) {
            /* dependent: the template at +48, the raw texel's register at
             * +32 (type +30: 1 = r), the sampler at +26 */
            if (info->n_deps == SGX545_MAX_DEP_SAMPLES || rd16(u, off + 30) != 1)
               return info->why = "a dependent texture sample (block 5)", false;
            info->deps[info->n_deps].at = info->n_insts;
            info->deps[info->n_deps].sampler = rd16(u, off + 26);
            info->deps[info->n_deps].raw = rd16(u, off + 32);
            info->deps[info->n_deps].smp = (uint64_t)rd32(u, off + 52) << 32 | rd32(u, off + 48);
            info->n_deps++;
         }
         off += 56;
         break;
      case 6: off += 74; break;
      case 9: return info->why = "block 9", false;
      case 7: return true;
      default: return info->why = "an unknown block", false;
      }
   }
   return info->why = "a block stream without its end", false;
}

/* The flat program with each dependent sample's smp and its wdf put in
 * (ISA.md "The texture-sample sequence"): the template with the raw texel's
 * temporary as destination (l21-27, h7 clear: r) and the sampler's 4 state
 * words, in SA from `sa` on, as the texture state (src1 l7-13, bank l30-31 =
 * 3: sa; h17 clear); then `wdf` on the template's DRC before the compiler's
 * unpack reads the texel. Branch offsets are moved with the code. NULL on
 * allocation failure; *n is the new length. */
static uint64_t *
insert_samples(const uint64_t *code, unsigned count, const struct usp_info *info, const unsigned *sa,
               unsigned *n)
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
         uint32_t lo = (uint32_t)info->deps[j].smp, hi = info->deps[j].smp >> 32;

         lo = (lo & ~(0x7fu << 21 | 0x7fu << 7 | 0xc0000000u)) | info->deps[j].raw << 21 | sa[j] << 7 | 0xc0000000u;
         hi &= ~(0x80u | 1u << 17);
         w[k++] = (uint64_t)hi << 32 | lo;
         w[k++] = (uint64_t)(0xf9200000u | (hi & 1)) << 32;   /* wdf drc */
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
      w[map[i]] = (uint64_t)hi << 32 | (lo & ~0x3ffffu) | ((uint32_t)off & 0x3ffffu);
   }
   free(map);
   *n = k;
   return w;
}

static bool
read_usp(const uint8_t *u, size_t size, struct sgx545_shader *so, struct usp_info *info)
{
   size_t off = 0x84;
   unsigned n_a, n_c, n_d, n_sec;
   uint32_t n_b;

   if (size < 0x84 || rd32(u, 0) != 0x55535020 || rd32(u, 4) != 0x23)
      return info->why = "not a USP", false;
   info->temps = rd16(u, 0x46);
   info->n_pa = rd16(u, 0x44);
   info->sa_slots = rd16(u, 0x7e);
   info->color_type = rd16(u, 0x66);
   info->color_reg = rd16(u, 0x68);
   n_a = rd16(u, 0x4e);
   n_c = rd16(u, 0x50);
   n_b = rd32(u, 0x52);
   n_d = rd16(u, 0x56);
   n_sec = rd16(u, 0x7c);
   off += 4 * rd32(u, 0x28);                       /* workaround ids */
   for (unsigned i = 0; i < n_a && off + 12 <= size; i++, off += 12) {
      unsigned flags = rd16(u, off);

      if (info->n_iters == ARRAY_SIZE(info->iters) || (flags & ~0xdu))
         return info->why = "an iteration list A entry", false;
      /* a plain iteration has flags 4 or 0xc; a sample entry 0, or 1 projected */
      info->iters[info->n_iters].sample = !(flags & 4);
      info->iters[info->n_iters].proj = flags & 1;
      info->iters[info->n_iters].sampler = rd16(u, off + 2);
      info->iters[info->n_iters].iterator = rd16(u, off + 4);
      info->iters[info->n_iters].comps = rd16(u, off + 6);
      info->iters[info->n_iters].format = rd16(u, off + 8);
      info->n_iters++;
   }
   for (uint32_t i = 0; i < n_b && off < size; i++)   /* list B: constants left in memory */
      return info->why = "constants in memory (list B)", false;
   for (unsigned i = 0; i < n_c && off + 8 <= size; i++) {
      unsigned kind = rd16(u, off);
      struct sgx545_const *k = &so->consts[so->n_consts];
      /* kind 1 f32, 2 F16 half (k, slot, shift); 3 a C10 channel, a 10-byte
       * record (k, 0, slot, shift) */
      unsigned at = kind == 3 ? off + 2 : off;

      if (kind < 1 || kind > 3 || so->n_consts == SGX545_MAX_CONSTS)
         return info->why = "a list C constant kind or too many", false;
      k->fmt = kind;
      k->k = rd16(u, off + 2);
      k->slot = rd16(u, at + 4);
      k->shift = rd16(u, at + 6);
      so->const_dwords = MAX2(so->const_dwords, k->slot + 1u);
      so->n_consts++;
      off += kind == 3 ? 10 : 8;
   }
   if (n_d || n_sec || off > size)
      return info->why = n_d ? "list D" : n_sec ? "a secondary program" : "a short USP", false;
   /* the vertex input mask, then the output mask (both in u32s) */
   if (rd32(u, 0x10) == 1)
      off += 4 * ((rd16(u, 0x80) + 31) / 32);
   off += 4 * ((rd16(u, 0x82) + 31) / 32);
   return blocks_supported(u, size, off, info);
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
   remap_legacy_varyings(nir);
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
      fprintf(stderr, "sgx545: %s shader: USP features not handled yet (%s)\n", vertex ? "vertex" : "fragment",
              info.why ? info.why : "colour output");
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
         /* TSP: 3 bits per set (1, 2, 3, or 5 for 4 components) (STATE.md
          * section 3).  TSP word 1 stays 0, as the DDK's for a sampled set:
          * with a set's bit in it, the TPU samples at a constant coordinate */
         so->prog.tsp_sizes |= (n == 4 ? 5 : n) << (3 * s);

      }
      so->prog.out_dwords = dwords;
      so->prog.const_dwords = so->const_dwords;
   } else {
      /* dependent samples: their state words go in SA after the constants */
      if (info.n_deps) {
         unsigned sa[SGX545_MAX_DEP_SAMPLES], n;

         if (info.n_insts != out.n_code ||
             MAX2(so->const_dwords, info.sa_slots) + 4 * info.n_deps > SGX545_MAX_CONSTS) {
            fprintf(stderr, "sgx545: USP blocks hold %u instructions, the program %u: not drawn\n",
                    info.n_insts, out.n_code);
            simple_mtx_unlock(&screen->lock);
            sgx_shader_free(&out);
            ralloc_free(nir);
            return false;
         }
         /* after every SA register the program uses: its constants (list C)
          * and the slots the compiler keeps for itself (header 0x7e) */
         if (info.sa_slots > so->const_dwords)
            SGX545_DBG(NIR, "fragment program: %u SA slots, %u in list C\n", info.sa_slots, so->const_dwords);
         so->const_dwords = MAX2(so->const_dwords, info.sa_slots);
         for (unsigned j = 0; j < info.n_deps; j++) {
            so->dep_sa[j] = 9 + so->const_dwords + 4 * j;
            so->dep_sampler[j] = info.deps[j].sampler;
            sa[j] = so->dep_sa[j];
         }
         so->n_deps = info.n_deps;
         so->const_dwords += 4 * info.n_deps;
         so->code = insert_samples(out.code, out.n_code, &info, sa, &n);
         so->n_code = so->code ? n : 0;
      } else {
         so->code = malloc(out.n_code * sizeof(*so->code));
         if (so->code) {
            memcpy(so->code, out.code, out.n_code * sizeof(*so->code));
            so->n_code = out.n_code;
         }
      }
      /* a program that discards: its feedback nop is the last instruction
       * (the compiler's layout); the depth plane goes in after the iterated
       * inputs, by one more PDS iteration */
      so->discard_pa = -1;
      if (orig->info.fs.uses_discard) {
         if (!so->n_code || so->code[so->n_code - 1] != (uint64_t)0xf8000140u << 32) {
            fprintf(stderr, "sgx545: fragment shader with discard: no feedback point, not drawn\n");
            free(so->code);
            so->code = NULL;
         }
      }
      so->color_reg = info.color_reg;
      so->temps = info.temps;
      so->prog.temps = info.temps;
      so->prog.pa_dwords = info.n_pa;
      so->prog.sec_dwords = so->const_dwords ? 9 + so->const_dwords : 0;
      /* One DOUTI per list A entry, in PA order (docs/PDS.md "DOUTI"): a
       * varying's iterator is its set + 1.  The writer's other kinds 0xb-0xf
       * (the colour placeholder in a shader without varyings) are dropped: the
       * DDK iterates the colour for them (source 0x20), which hangs the GPU
       * when the vertex program outputs no colour, and ours never do. */
      int last_plain = -1, last_sample = -1;

      for (unsigned i = 0; i < info.n_iters && i < ARRAY_SIZE(so->prog.douti); i++) {
         unsigned it = info.iters[i].iterator;
         uint32_t w;

         if (info.iters[i].sample) {
            /* sample half: coordinate set + 1, 2-D (0x200) or projected
             * (0x400) (docs/PDS.md).  A sample of the set the previous plain
             * DOUTI iterates is merged into it, as the DDK's generator does
             * (w0 & 0x400007ff); a separate one samples at coordinate 0
             * (EA3).  Otherwise a pure sample: 0x12000 and 0x6000000. */
            uint32_t half = it | (info.iters[i].proj ? 0x400u : 0x200u);
            unsigned prev = so->prog.n_douti - 1;

            if (so->prog.n_douti && !so->prog.douti_tex[prev] && (so->prog.douti[prev] & 0x7ff) == 0 &&
                ((so->prog.douti[prev] >> 11) & 0x3f) == it) {
               so->prog.douti[prev] |= half;
               so->prog.douti_tex[prev] = info.iters[i].sampler + 1;
               last_sample = prev;
            } else {
               last_sample = so->prog.n_douti;
               so->prog.douti_tex[so->prog.n_douti] = info.iters[i].sampler + 1;
               so->prog.douti[so->prog.n_douti++] = half | 0x12000u | 0x6000000u;
            }
            continue;
         }
         if (it >= 0xb)
            continue;   /* the vertex programs output no colour: nothing to iterate */
         else
            w = it << 11 | (info.iters[i].comps - 1) << 22 | 1u << 24 | 0x6000000u |
                (info.iters[i].format == 2 ? 1u << 28 : 0);
         last_plain = so->prog.n_douti;
         so->prog.douti[so->prog.n_douti++] = w;
      }
      if (so->code && orig->info.fs.uses_discard) {
         /* the triangle's depth plane (source 0x22, 3 components) after the
          * inputs: pa<n> on, n = the registers the iterations above fill (an
          * F32 component 1, an F16 pair 1, a sampled texel 1) */
         unsigned pa = 0;

         for (unsigned i = 0; i < so->prog.n_douti; i++) {
            uint32_t w = so->prog.douti[i];

            if (w & (1u << 24))
               pa += w & (1u << 28) ? (((w >> 22) & 3) + 2) / 2 : ((w >> 22) & 3) + 1;
            if (w & 0x600)
               pa++;
         }
         if (so->prog.n_douti == ARRAY_SIZE(so->prog.douti)) {
            free(so->code);
            so->code = NULL;
         } else {
            last_plain = so->prog.n_douti;
            so->prog.douti[so->prog.n_douti++] = 0x22u << 11 | 2u << 22 | 1u << 24 | 0x6000000u;
            so->discard_pa = pa;
            so->prog.pa_dwords = MAX2(so->prog.pa_dwords, pa + 3);
            /* the visibility test's words in sa3, sa4 come with the secondary DMA */
            so->prog.sec_dwords = 9 + so->const_dwords;
         }
      }
      so->prog.code = so->code ? sgxws_upload_fs(ctx->hw, so->code, so->n_code, so->color_reg, NULL,
                                                 so->discard_pa, so->temps) : 0;
      if (last_plain >= 0)
         so->prog.douti[last_plain] |= 1u << 29;   /* the last plain iteration */
      if (last_sample >= 0)
         so->prog.douti[last_sample] |= 1u << 30;  /* the last sample */
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
   so->discard_pa = -1;
   if (cso->type == PIPE_SHADER_IR_NIR)
      nir = cso->ir.nir;
   else
      nir = tgsi_to_nir(cso->tokens, pctx->screen, false);
   so->nir = nir;

   /* SGX545_NO_COMPILER=vs / fs / 1: the fixed programs for that stage / both */
   const char *no = getenv("SGX545_NO_COMPILER");
   bool fixed = no && (!strcmp(no, "1") ||
                       !strcmp(no, nir->info.stage == MESA_SHADER_VERTEX ? "vs" : "fs"));

   if (!fixed && !nir->info.io_lowered) {
      uint64_t t = os_time_get_nano();
      bool ok = compile_shader(ctx, so, nir);

      ctx->stats.compile_ns += os_time_get_nano() - t;
      if (ok)
         return so;
   }

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
   free(so->code);
   FREE(so);
}

/* the compiled fragment shader's program with its colour blended (b), or
 * without (NULL); 0 when there is none */
uint32_t
sgx545_fs_program(struct sgx545_context *ctx, struct sgx545_shader *so, const struct sgxws_blend *b)
{
   struct sgx545_screen *screen = sgx545_screen(ctx->base.screen);
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
   va = sgxws_upload_fs(ctx->hw, so->code, so->n_code, so->color_reg, b, so->discard_pa, so->temps);
   simple_mtx_unlock(&screen->lock);
   /* the program heap is never freed: past the table's size, the oldest
    * variant's slot is reused and its program uploaded again when needed */
   if (va) {
      unsigned i = so->n_variants < ARRAY_SIZE(so->variants) ? so->n_variants++ : 0;

      so->variants[i].key = *b;
      so->variants[i].code = va;
   }
   return va;
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
