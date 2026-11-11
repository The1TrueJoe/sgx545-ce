/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * End-to-end tests of the compiler: shaders built with nir_builder,
 * compiled, and the program checked through the isaspec disassembler
 * (every word decodes) and through what struct sgx_shader reports.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gtest/gtest.h>

#include "compiler/glsl_types.h"
#include "nir.h"
#include "nir_builder.h"
#include "sgx_compiler.h"
#include "sgx_fixed.h"
#include "sgx_fixed_golden.h"
#include "sgx_isa.h"

namespace {

class sgx_compile_test : public ::testing::Test {
 protected:
   sgx_compile_test()
   {
      glsl_type_singleton_init_or_ref();
   }

   ~sgx_compile_test()
   {
      if (compiled)
         sgx_shader_free(&out);
      ralloc_free(b.shader);
      glsl_type_singleton_decref();
   }

   void start(mesa_shader_stage stage)
   {
      b = nir_builder_init_simple_shader(stage, sgx_nir_options(), "test");
   }

   nir_variable *var(nir_variable_mode mode, const glsl_type *type,
                     const char *name, int location)
   {
      nir_variable *v = nir_variable_create(b.shader, mode, type, name);
      v->data.location = location;
      return v;
   }

   /* compile with the compiler's own layout; false on a compile error */
   bool compile(bool f16 = false, uint32_t tex_bgra = 0)
   {
      struct sgx_compile_options opts = {};
      struct sgx_varyings vary = {};

      opts.f16 = f16;
      opts.tex_bgra = tex_bgra;
      opts.pds_samples_alone = pds_alone;
      opts.f16_samples = f16_samples;
      opts.split_phases = split_phases;
      opts.dep_f16_samples = dep_f16;
      opts.driver_consts = true;
      sgx_assign_varyings(b.shader, b.shader->info.stage == MESA_SHADER_VERTEX,
                          &vary);
      compiled = true;
      return sgx_compile(b.shader, &opts, &vary, &out) == 0;
   }

   /* the program as text, one instruction a line */
   std::string disasm()
   {
      char *text = NULL;
      size_t len = 0;
      FILE *f = open_memstream(&text, &len);
      std::vector<uint32_t> w;
      for (unsigned i = 0; i < out.n_code; i++) {
         w.push_back((uint32_t)out.code[i]);
         w.push_back((uint32_t)(out.code[i] >> 32));
      }
      sgx_disasm(w.data(), out.n_code, f, NULL);
      fclose(f);
      std::string s(text, len);
      free(text);
      return s;
   }

   /* every word decodes to a known form with no stray bits */
   void expect_decodes()
   {
      std::string s = disasm();
      EXPECT_EQ(s.find("no match"), std::string::npos) << s;
      EXPECT_EQ(s.find("unexpected bits"), std::string::npos) << s;
      EXPECT_EQ(s.find("dontcare"), std::string::npos) << s;
   }

   nir_builder b = {};
   struct sgx_shader out = {};
   bool compiled = false;
   bool pds_alone = false;
   bool f16_samples = false;
   bool split_phases = false;
   bool dep_f16 = false;
};

TEST_F(sgx_compile_test, fragment_varying_to_colour)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *in =
      var(nir_var_shader_in, glsl_vec4_type(), "v_coords", VARYING_SLOT_VAR0);
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_store_var(&b, col, nir_load_var(&b, in), 0xf);

   ASSERT_TRUE(compile()) << out.log;
   EXPECT_GT(out.n_code, 0u);
   EXPECT_EQ(out.n_iters, 1u);
   EXPECT_EQ(out.n_flow, 0u);
   expect_decodes();
}

TEST_F(sgx_compile_test, vertex_position)
{
   start(MESA_SHADER_VERTEX);
   nir_variable *in = var(nir_var_shader_in, glsl_vec4_type(), "a_position",
                          VERT_ATTRIB_GENERIC0);
   nir_variable *pos = var(nir_var_shader_out, glsl_vec4_type(), "gl_Position",
                           VARYING_SLOT_POS);
   nir_store_var(&b, pos, nir_load_var(&b, in), 0xf);

   ASSERT_TRUE(compile()) << out.log;
   EXPECT_GT(out.n_code, 0u);
   EXPECT_GE(out.n_out_regs, 4u);
   expect_decodes();
}

/* mat4 * v from a uniform and an attribute: co-issued as efo (two result
 * lanes an instruction) */
TEST_F(sgx_compile_test, vertex_mat4_efo)
{
   start(MESA_SHADER_VERTEX);
   nir_variable *m = var(nir_var_uniform,
                         glsl_matrix_type(GLSL_TYPE_FLOAT, 4, 4), "u_mvp", -1);
   nir_variable *in = var(nir_var_shader_in, glsl_vec4_type(), "a_position",
                          VERT_ATTRIB_GENERIC0);
   nir_variable *pos = var(nir_var_shader_out, glsl_vec4_type(), "gl_Position",
                           VARYING_SLOT_POS);
   nir_deref_instr *md = nir_build_deref_var(&b, m);
   nir_def *v = nir_load_var(&b, in);
   nir_def *r = NULL;
   for (unsigned c = 0; c < 4; c++) {
      nir_def *col = nir_load_deref(&b, nir_build_deref_array_imm(&b, md, c));
      nir_def *t = nir_fmul(&b, col, nir_channel(&b, v, c));
      r = r ? nir_fadd(&b, r, t) : t;
   }
   nir_store_var(&b, pos, r, 0xf);

   ASSERT_TRUE(compile()) << out.log;
   expect_decodes();
   EXPECT_NE(disasm().find("efo"), std::string::npos) << disasm();
}

/* A loop bounded by bool ? 3 : 0 over 32-bit ints, with mediump in F16: the
 * select must stay an integer one. Demoting it to F16 turned the bound into
 * 0.0 and folded the loop away (dEQP select_iteration_count_fragment). */
TEST_F(sgx_compile_test, fragment_int_select_loop_bound)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *in =
      var(nir_var_shader_in, glsl_vec4_type(), "v_coords", VARYING_SLOT_VAR0);
   in->data.precision = GLSL_PRECISION_MEDIUM;
   nir_variable *ub = var(nir_var_uniform, glsl_bool_type(), "ub_true", -1);
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_variable *i = nir_local_variable_create(b.impl, glsl_int_type(), "i");
   nir_variable *res =
      nir_local_variable_create(b.impl, glsl_vec4_type(), "res");

   nir_store_var(&b, res, nir_load_var(&b, in), 0xf);
   nir_store_var(&b, i, nir_imm_int(&b, 0), 1);
   nir_loop *loop = nir_push_loop(&b);
   {
      nir_def *iv = nir_load_var(&b, i);
      nir_def *bound = nir_bcsel(&b, nir_load_var(&b, ub), nir_imm_int(&b, 3),
                                 nir_imm_int(&b, 0));
      nir_break_if(&b, nir_ige(&b, iv, bound));
      static const unsigned yzwx[] = {1, 2, 3, 0};
      nir_store_var(&b, res, nir_swizzle(&b, nir_load_var(&b, res), yzwx, 4),
                    0xf);
      nir_store_var(&b, i, nir_iadd_imm(&b, iv, 1), 1);
   }
   nir_pop_loop(&b, loop);
   nir_store_var(&b, col, nir_load_var(&b, res), 0xf);

   ASSERT_TRUE(compile(true)) << out.log;
   expect_decodes();
   EXPECT_GT(out.n_flow, 0u) << "the loop was folded away\n" << disasm();
}

/* gl_FragColor with only some channels written (piglit
 * glsl-fs-post-increment-01): the others are undefined, not a crash */
TEST_F(sgx_compile_test, fragment_partial_colour)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *in =
      var(nir_var_shader_in, glsl_vec4_type(), "v_coords", VARYING_SLOT_VAR0);
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_store_var(&b, col, nir_load_var(&b, in), 0x7);

   ASSERT_TRUE(compile(true)) << out.log;
   expect_decodes();
}

/* A texel as the colour: its bytes moved into BGRA order (two pck.u8.u8),
 * not unpacked to floats and packed again */
TEST_F(sgx_compile_test, fragment_texel_to_colour)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *uv =
      var(nir_var_shader_in, glsl_vec_type(2), "uv", VARYING_SLOT_VAR0);
   nir_variable *smp =
      var(nir_var_uniform,
          glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT),
          "t", -1);
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_deref_instr *sd = nir_build_deref_var(&b, smp);
   struct nir_tex_builder tb = {};
   tb.coord = nir_load_var(&b, uv);
   tb.texture_deref = sd;
   tb.sampler_deref = sd;
   tb.dim = GLSL_SAMPLER_DIM_2D;
   tb.dest_type = nir_type_float32;
   nir_store_var(&b, col, nir_build_tex_struct(&b, nir_texop_tex, tb), 0xf);

   ASSERT_TRUE(compile(true)) << out.log;
   expect_decodes();
   std::string s = disasm();
   EXPECT_EQ(out.n_code, 2u) << s;
   EXPECT_EQ(out.n_temps, 0u) << s;
   EXPECT_NE(s.find("pck.u8.u8"), std::string::npos) << s;
}

/* A uniform vec4 as the colour: one mov from a slot the driver fills with
 * the colour packed to BGRA8 (constant map format 0x12) */
TEST_F(sgx_compile_test, fragment_uniform_colour_packed)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *u = var(nir_var_uniform, glsl_vec4_type(), "color", -1);
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_store_var(&b, col, nir_load_var(&b, u), 0xf);

   ASSERT_TRUE(compile(true)) << out.log;
   expect_decodes();
   EXPECT_EQ(out.n_code, 1u) << disasm();
   bool packed = false;
   for (unsigned i = 0; i < out.n_consts; i++)
      packed |= out.consts[i].fmt == 0x12 && out.consts[i].k == 0;
   EXPECT_TRUE(packed);
}

/* for (i < 8) { if (r.x > a.x) break; r += b; }: a loop that can exit early
 * stays a loop (unrolled, its exits became a select tree holding all eight
 * partial sums: corpus loop_v, 31 temporaries) */
TEST_F(sgx_compile_test, fragment_early_exit_loop_kept)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *a =
      var(nir_var_shader_in, glsl_vec4_type(), "a", VARYING_SLOT_VAR0);
   nir_variable *bv =
      var(nir_var_shader_in, glsl_vec4_type(), "b", VARYING_SLOT_VAR1);
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_variable *i = nir_local_variable_create(b.impl, glsl_int_type(), "i");
   nir_variable *r = nir_local_variable_create(b.impl, glsl_vec4_type(), "r");

   nir_store_var(&b, r, nir_imm_vec4(&b, 0, 0, 0, 0), 0xf);
   nir_store_var(&b, i, nir_imm_int(&b, 0), 1);
   nir_loop *loop = nir_push_loop(&b);
   {
      nir_def *iv = nir_load_var(&b, i);
      nir_break_if(&b, nir_ige_imm(&b, iv, 8));
      nir_def *rv = nir_load_var(&b, r);
      nir_break_if(&b, nir_flt(&b, nir_channel(&b, nir_load_var(&b, a), 0),
                               nir_channel(&b, rv, 0)));
      nir_store_var(&b, r, nir_fadd(&b, rv, nir_load_var(&b, bv)), 0xf);
      nir_store_var(&b, i, nir_iadd_imm(&b, iv, 1), 1);
   }
   nir_pop_loop(&b, loop);
   nir_store_var(&b, col, nir_load_var(&b, r), 0xf);

   ASSERT_TRUE(compile()) << out.log;
   expect_decodes();
   EXPECT_GT(out.n_flow, 0u) << disasm();
   EXPECT_LE(out.n_temps, 8u) << disasm();
}

/* ceil(x) is one fsubflr d, c48, -x (= -floor(-x)): the floor's negated
 * result and ceil's own negations cancel */
TEST_F(sgx_compile_test, fragment_ceil_one_instruction)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *a =
      var(nir_var_shader_in, glsl_vec4_type(), "a", VARYING_SLOT_VAR0);
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_store_var(&b, col, nir_fceil(&b, nir_load_var(&b, a)), 0xf);

   ASSERT_TRUE(compile()) << out.log;
   expect_decodes();
   EXPECT_EQ(out.n_code, 3u) << disasm(); /* fsubflr, two pck */
   EXPECT_EQ(out.n_temps, 0u) << disasm();
}

/* (a < b || c < d) ? x : y: the comparisons stay test masks and are or'ed
 * bitwise, no 1.0 / 0.0 floats (glamor's gradient repeat tests) */
TEST_F(sgx_compile_test, fragment_or_of_compares_as_masks)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *a =
      var(nir_var_shader_in, glsl_vec4_type(), "a", VARYING_SLOT_VAR0);
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_def *v = nir_load_var(&b, a);
   nir_def *c1 = nir_flt(&b, nir_channel(&b, v, 0), nir_channel(&b, v, 1));
   nir_def *c2 = nir_flt(&b, nir_channel(&b, v, 2), nir_channel(&b, v, 3));
   nir_def *sel = nir_bcsel(&b, nir_ior(&b, c1, c2), v, nir_fneg(&b, v));
   nir_store_var(&b, col, sel, 0xf);

   ASSERT_TRUE(compile()) << out.log;
   expect_decodes();
   std::string s = disasm();
   EXPECT_NE(s.find(" or "), std::string::npos) << s;
   EXPECT_EQ(s.find("fmaxmin"), std::string::npos) << s;
}

/* a texel whose unit arrives BGRA-ordered (tex_bgra) is the colour as it
 * is: no instruction, the colour register is the sampled one */
TEST_F(sgx_compile_test, fragment_bgra_texel_is_colour)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *uv =
      var(nir_var_shader_in, glsl_vec_type(2), "uv", VARYING_SLOT_VAR0);
   nir_variable *smp =
      var(nir_var_uniform,
          glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT),
          "t", -1);
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_deref_instr *sd = nir_build_deref_var(&b, smp);
   struct nir_tex_builder tb = {};
   tb.coord = nir_load_var(&b, uv);
   tb.texture_deref = sd;
   tb.sampler_deref = sd;
   tb.dim = GLSL_SAMPLER_DIM_2D;
   tb.dest_type = nir_type_float32;
   nir_store_var(&b, col, nir_build_tex_struct(&b, nir_texop_tex, tb), 0xf);

   ASSERT_TRUE(compile(true, 1)) << out.log;
   EXPECT_EQ(out.n_code, 0u) << disasm();
   ASSERT_EQ(out.n_samples, 1u);
   EXPECT_EQ(out.colour_pa, (unsigned)out.samples[0].reg);
}

/* ... and read as a value, its red and blue come from bytes 2 and 0 */
TEST_F(sgx_compile_test, fragment_bgra_texel_unpacks_zyxw)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *uv =
      var(nir_var_shader_in, glsl_vec_type(2), "uv", VARYING_SLOT_VAR0);
   nir_variable *smp =
      var(nir_var_uniform,
          glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT),
          "t", -1);
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_deref_instr *sd = nir_build_deref_var(&b, smp);
   struct nir_tex_builder tb = {};
   tb.coord = nir_load_var(&b, uv);
   tb.texture_deref = sd;
   tb.sampler_deref = sd;
   tb.dim = GLSL_SAMPLER_DIM_2D;
   tb.dest_type = nir_type_float32;
   nir_def *t = nir_build_tex_struct(&b, nir_texop_tex, tb);
   nir_store_var(&b, col, nir_fmul_imm(&b, t, 0.5), 0xf);

   ASSERT_TRUE(compile(false, 1)) << out.log;
   expect_decodes();
   std::string s = disasm();
   /* channel 0 (red) unpacked from byte 2 */
   size_t at = s.find("pck.f32.u8 r0, pa");
   ASSERT_NE(at, std::string::npos) << s;
   std::string line = s.substr(at, s.find('\n', at) - at);
   EXPECT_NE(line.find(".l2,"), std::string::npos) << s;
}

/* pds_samples_alone: a varying only a sample reads has no plain iteration;
 * the sample is the first PA register (the DDK's tex fs: one PDS sample into
 * pa0, the colour) */
TEST_F(sgx_compile_test, fragment_sample_only_varying_not_iterated)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *uv =
      var(nir_var_shader_in, glsl_vec_type(2), "uv", VARYING_SLOT_VAR0);
   nir_variable *smp =
      var(nir_var_uniform,
          glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT),
          "t", -1);
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_deref_instr *sd = nir_build_deref_var(&b, smp);
   struct nir_tex_builder tb = {};
   tb.coord = nir_load_var(&b, uv);
   tb.texture_deref = sd;
   tb.sampler_deref = sd;
   tb.dim = GLSL_SAMPLER_DIM_2D;
   tb.dest_type = nir_type_float32;
   nir_store_var(&b, col, nir_build_tex_struct(&b, nir_texop_tex, tb), 0xf);

   pds_alone = true;
   ASSERT_TRUE(compile(true, 1)) << out.log;
   ASSERT_EQ(out.n_iters, 1u);
   EXPECT_TRUE(out.iters[0].sample);
   EXPECT_EQ(out.iters[0].pa, 0u);
   EXPECT_EQ(out.colour_pa, 0u);
   EXPECT_EQ(out.n_code, 0u) << disasm();
}

/* two mat4 * v back to back (position, then a texture coordinate): the
 * second chain's first efo stores the first chain's last lane (d=i1), so only
 * the last chain ends with mov oN, i1 (libusc's chained v_mvp) */
TEST_F(sgx_compile_test, vertex_efo_chains_share_the_mov)
{
   start(MESA_SHADER_VERTEX);
   nir_variable *m = var(nir_var_uniform,
                         glsl_matrix_type(GLSL_TYPE_FLOAT, 4, 4), "u_mvp", -1);
   nir_variable *t = var(nir_var_uniform,
                         glsl_matrix_type(GLSL_TYPE_FLOAT, 4, 4), "u_tex", -1);
   nir_variable *in = var(nir_var_shader_in, glsl_vec4_type(), "a_position",
                          VERT_ATTRIB_GENERIC0);
   nir_variable *tc =
      var(nir_var_shader_in, glsl_vec4_type(), "a_tc", VERT_ATTRIB_GENERIC1);
   nir_variable *pos = var(nir_var_shader_out, glsl_vec4_type(), "gl_Position",
                           VARYING_SLOT_POS);
   nir_variable *vt =
      var(nir_var_shader_out, glsl_vec4_type(), "v_tc", VARYING_SLOT_VAR0);
   nir_variable *mats[2] = {m, t}, *srcs[2] = {in, tc}, *dsts[2] = {pos, vt};
   for (unsigned q = 0; q < 2; q++) {
      nir_deref_instr *md = nir_build_deref_var(&b, mats[q]);
      nir_def *v = nir_load_var(&b, srcs[q]);
      nir_def *r = NULL;
      for (unsigned c = 0; c < 4; c++) {
         nir_def *col =
            nir_load_deref(&b, nir_build_deref_array_imm(&b, md, c));
         nir_def *x = nir_fmul(&b, col, nir_channel(&b, v, c));
         r = r ? nir_fadd(&b, r, x) : x;
      }
      nir_store_var(&b, dsts[q], r, 0xf);
   }

   ASSERT_TRUE(compile()) << out.log;
   expect_decodes();
   std::string s = disasm();
   unsigned movs = 0;
   for (size_t at = s.find(", i1"); at != std::string::npos;
        at = s.find(", i1", at + 1))
      movs += s.rfind("mov ", at) != std::string::npos &&
              s.rfind("mov ", at) > s.rfind('\n', at);
   EXPECT_EQ(movs, 1u) << s;
   EXPECT_NE(s.find("d=i1"), std::string::npos) << s;
}

/* f16_samples: a mediump texel returns F16 into two registers and is read
 * as their halves, with no unpack (the DDK's terrain program) */
TEST_F(sgx_compile_test, fragment_f16_sample_not_unpacked)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *uv =
      var(nir_var_shader_in, glsl_vec_type(2), "uv", VARYING_SLOT_VAR0);
   nir_variable *c4 =
      var(nir_var_shader_in, glsl_vec4_type(), "c", VARYING_SLOT_VAR1);
   c4->data.precision = GLSL_PRECISION_MEDIUM;
   nir_variable *smp =
      var(nir_var_uniform,
          glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT),
          "t", -1);
   smp->data.precision = GLSL_PRECISION_MEDIUM;
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_deref_instr *sd = nir_build_deref_var(&b, smp);
   struct nir_tex_builder tb = {};
   tb.coord = nir_load_var(&b, uv);
   tb.texture_deref = sd;
   tb.sampler_deref = sd;
   tb.dim = GLSL_SAMPLER_DIM_2D;
   tb.dest_type = nir_type_float32;
   nir_def *t = nir_build_tex_struct(&b, nir_texop_tex, tb);
   nir_store_var(&b, col, nir_fmul(&b, t, nir_load_var(&b, c4)), 0xf);

   f16_samples = true;
   ASSERT_TRUE(compile(true)) << out.log;
   expect_decodes();
   ASSERT_EQ(out.n_samples, 1u);
   EXPECT_TRUE(out.samples[0].f16);
   std::string s = disasm();
   EXPECT_EQ(s.find("pck.f16.u8"), std::string::npos) << s;
   EXPECT_EQ(s.find("pck.f32.u8"), std::string::npos) << s;
}

/* ... and a projected sample (TXP: xy and w of one vec4 varying): the PDS
 * projects as it samples, so the varying isn't iterated either */
TEST_F(sgx_compile_test, fragment_proj_sample_only_varying_not_iterated)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *tc =
      var(nir_var_shader_in, glsl_vec4_type(), "tc", VARYING_SLOT_VAR0);
   nir_variable *c4 =
      var(nir_var_shader_in, glsl_vec4_type(), "c", VARYING_SLOT_VAR1);
   nir_variable *smp =
      var(nir_var_uniform,
          glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT),
          "t", -1);
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_deref_instr *sd = nir_build_deref_var(&b, smp);
   nir_def *v = nir_load_var(&b, tc);
   nir_tex_instr *tx = nir_tex_instr_create(b.shader, 4);
   tx->op = nir_texop_tex;
   tx->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tx->dest_type = nir_type_float32;
   tx->coord_components = 2;
   tx->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, nir_channels(&b, v, 0x3));
   tx->src[1] =
      nir_tex_src_for_ssa(nir_tex_src_projector, nir_channel(&b, v, 3));
   tx->src[2] = nir_tex_src_for_ssa(nir_tex_src_texture_deref, &sd->def);
   tx->src[3] = nir_tex_src_for_ssa(nir_tex_src_sampler_deref, &sd->def);
   nir_def_init(&tx->instr, &tx->def, 4, 32);
   nir_builder_instr_insert(&b, &tx->instr);
   nir_def *texel = &tx->def;
   nir_store_var(&b, col, nir_fmul(&b, texel, nir_load_var(&b, c4)), 0xf);

   pds_alone = true;
   ASSERT_TRUE(compile()) << out.log;
   expect_decodes();
   unsigned plain = 0, proj = 0;
   for (unsigned i = 0; i < out.n_iters; i++) {
      plain += !out.iters[i].sample;
      proj += out.iters[i].sample && out.iters[i].proj;
   }
   EXPECT_EQ(plain, 1u); /* c only */
   EXPECT_EQ(proj, 1u);
}

/* dep_f16_samples: a dependent mediump texel whose four channels are read
 * returns F16 into two registers (the driver's T3), and isn't unpacked */
TEST_F(sgx_compile_test, fragment_dependent_f16_sample_not_unpacked)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *uv =
      var(nir_var_shader_in, glsl_vec_type(2), "uv", VARYING_SLOT_VAR0);
   nir_variable *c4 =
      var(nir_var_shader_in, glsl_vec4_type(), "c", VARYING_SLOT_VAR1);
   c4->data.precision = GLSL_PRECISION_MEDIUM;
   nir_variable *smp =
      var(nir_var_uniform,
          glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT),
          "t", -1);
   smp->data.precision = GLSL_PRECISION_MEDIUM;
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_deref_instr *sd = nir_build_deref_var(&b, smp);
   nir_def *c = nir_load_var(&b, uv);
   struct nir_tex_builder tb = {};
   tb.coord = nir_fadd_imm(&b, nir_fmul(&b, c, c), 0.1);
   tb.texture_deref = sd;
   tb.sampler_deref = sd;
   tb.dim = GLSL_SAMPLER_DIM_2D;
   tb.dest_type = nir_type_float32;
   nir_def *texel = nir_build_tex_struct(&b, nir_texop_tex, tb);
   nir_store_var(&b, col, nir_fmul(&b, texel, nir_load_var(&b, c4)), 0xf);

   f16_samples = dep_f16 = true;
   ASSERT_TRUE(compile(true)) << out.log;
   expect_decodes();
   ASSERT_EQ(out.n_samples, 1u);
   EXPECT_TRUE(out.samples[0].dependent);
   EXPECT_TRUE(out.samples[0].f16);
   EXPECT_EQ(disasm().find("pck.f16.u8"), std::string::npos) << disasm();
}

/* two plain varyings and two projected PDS-only samples: each sample comes
 * right after a plain iteration (the driver merges it into that DOUTI) */
TEST_F(sgx_compile_test, fragment_pds_samples_interleaved)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *a =
      var(nir_var_shader_in, glsl_vec4_type(), "a", VARYING_SLOT_VAR0);
   nir_variable *d =
      var(nir_var_shader_in, glsl_vec4_type(), "d", VARYING_SLOT_VAR1);
   nir_variable *tc[2] = {
      var(nir_var_shader_in, glsl_vec4_type(), "t0", VARYING_SLOT_VAR2),
      var(nir_var_shader_in, glsl_vec4_type(), "t1", VARYING_SLOT_VAR3)};
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_def *acc = nir_fmul(&b, nir_load_var(&b, a), nir_load_var(&b, d));
   for (unsigned k = 0; k < 2; k++) {
      nir_variable *smp = var(
         nir_var_uniform,
         glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT),
         k ? "s1" : "s0", -1);
      smp->data.binding = k;
      nir_deref_instr *sd = nir_build_deref_var(&b, smp);
      nir_def *v = nir_load_var(&b, tc[k]);
      nir_tex_instr *tx = nir_tex_instr_create(b.shader, 4);
      tx->op = nir_texop_tex;
      tx->sampler_dim = GLSL_SAMPLER_DIM_2D;
      tx->dest_type = nir_type_float32;
      tx->coord_components = 2;
      tx->texture_index = tx->sampler_index = k;
      tx->src[0] =
         nir_tex_src_for_ssa(nir_tex_src_coord, nir_channels(&b, v, 0x3));
      tx->src[1] =
         nir_tex_src_for_ssa(nir_tex_src_projector, nir_channel(&b, v, 3));
      tx->src[2] = nir_tex_src_for_ssa(nir_tex_src_texture_deref, &sd->def);
      tx->src[3] = nir_tex_src_for_ssa(nir_tex_src_sampler_deref, &sd->def);
      nir_def_init(&tx->instr, &tx->def, 4, 32);
      nir_builder_instr_insert(&b, &tx->instr);
      acc = nir_fmul(&b, acc, &tx->def);
   }
   nir_store_var(&b, col, acc, 0xf);

   pds_alone = true;
   ASSERT_TRUE(compile()) << out.log;
   expect_decodes();
   ASSERT_EQ(out.n_iters, 4u);
   for (unsigned i = 0; i < 4; i++)
      EXPECT_EQ((bool)out.iters[i].sample, (bool)(i & 1)) << "entry " << i;
   /* PA in list order */
   for (unsigned i = 1; i < 4; i++)
      EXPECT_EQ(out.iters[i].pa, out.iters[i - 1].pa + out.iters[i - 1].regs);
}

/* split_phases: an alpha-tested texture program becomes phase 0 (the test
 * into p1, ending in the feedback nop) and phase 1 (the colour); phase 0
 * writes no PA register (the inputs phase 1 reads) */
TEST_F(sgx_compile_test, fragment_alpha_test_split_in_two_phases)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *uv =
      var(nir_var_shader_in, glsl_vec_type(2), "uv", VARYING_SLOT_VAR0);
   nir_variable *c4 =
      var(nir_var_shader_in, glsl_vec4_type(), "c", VARYING_SLOT_VAR1);
   nir_variable *smp =
      var(nir_var_uniform,
          glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT),
          "t", -1);
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_deref_instr *sd = nir_build_deref_var(&b, smp);
   struct nir_tex_builder tb = {};
   tb.coord = nir_load_var(&b, uv);
   tb.texture_deref = sd;
   tb.sampler_deref = sd;
   tb.dim = GLSL_SAMPLER_DIM_2D;
   tb.dest_type = nir_type_float32;
   nir_def *v = nir_fmul(&b, nir_build_tex_struct(&b, nir_texop_tex, tb),
                         nir_load_var(&b, c4));
   nir_discard_if(&b, nir_flt_imm(&b, nir_channel(&b, v, 3), 0.5));
   nir_store_var(&b, col, v, 0xf);

   split_phases = true;
   ASSERT_TRUE(compile()) << out.log;
   ASSERT_GT(out.n_code1, 0u);
   ASSERT_GT(out.n_code, 0u);
   EXPECT_EQ(out.code[out.n_code - 1], (uint64_t)0xf8000140u << 32);
   EXPECT_TRUE(out.discards);
   /* phase 0: no instruction writes PA */
   for (unsigned i = 0; i + 1 < out.n_code; i++) {
      char text[256];
      sgx_disasm_one(out.code[i], NULL, text, sizeof(text));
      std::string t(text);
      size_t sp = t.find(' ');
      EXPECT_NE(t.substr(sp + 1, 2), "pa") << t;
   }
}

/* mediump arithmetic with f16: a * b + c becomes fmad16 on F16 halves */
TEST_F(sgx_compile_test, fragment_mediump_fmad16)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *a =
      var(nir_var_shader_in, glsl_vec4_type(), "a", VARYING_SLOT_VAR0);
   nir_variable *u = var(nir_var_uniform, glsl_vec4_type(), "u", -1);
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   a->data.precision = u->data.precision = col->data.precision =
      GLSL_PRECISION_MEDIUM;
   nir_def *x = nir_load_var(&b, a);
   nir_store_var(&b, col,
                 nir_ffma(&b, x, nir_load_var(&b, u), nir_fmul(&b, x, x)), 0xf);

   ASSERT_TRUE(compile(true)) << out.log;
   expect_decodes();
   EXPECT_NE(disasm().find("fmad16"), std::string::npos) << disasm();
}

/* lowp: a colour from lowp uniforms only is computed in C10 (the colour
 * classes) */
TEST_F(sgx_compile_test, fragment_lowp_colour_in_c10)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *u0 = var(nir_var_uniform, glsl_vec4_type(), "u0", -1);
   nir_variable *u1 = var(nir_var_uniform, glsl_vec4_type(), "u1", -1);
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   u0->data.precision = u1->data.precision = col->data.precision =
      GLSL_PRECISION_LOW;
   nir_store_var(&b, col,
                 nir_fmul(&b, nir_load_var(&b, u0), nir_load_var(&b, u1)), 0xf);

   ASSERT_TRUE(compile(true)) << out.log;
   expect_decodes();
   std::string s = disasm();
   EXPECT_TRUE(s.find("sop") != std::string::npos ||
               s.find(".c10") != std::string::npos)
      << s;
}

/* a sample whose coordinate is computed is dependent: reported with its smp
 * (which decodes) for the driver to insert, and not iterated by the PDS */
TEST_F(sgx_compile_test, fragment_dependent_sample)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *uv =
      var(nir_var_shader_in, glsl_vec_type(2), "uv", VARYING_SLOT_VAR0);
   nir_variable *smp =
      var(nir_var_uniform,
          glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT),
          "t", -1);
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_deref_instr *sd = nir_build_deref_var(&b, smp);
   nir_def *c = nir_load_var(&b, uv);
   struct nir_tex_builder tb = {};
   tb.coord = nir_fadd_imm(&b, nir_fmul(&b, c, c), 0.1);
   tb.texture_deref = sd;
   tb.sampler_deref = sd;
   tb.dim = GLSL_SAMPLER_DIM_2D;
   tb.dest_type = nir_type_float32;
   nir_store_var(&b, col, nir_build_tex_struct(&b, nir_texop_tex, tb), 0xf);

   ASSERT_TRUE(compile()) << out.log;
   expect_decodes();
   ASSERT_EQ(out.n_samples, 1u);
   EXPECT_TRUE(out.samples[0].dependent);
   char text[256];
   ASSERT_TRUE(sgx_disasm_one(out.samples[0].smp, NULL, text, sizeof(text)));
   EXPECT_EQ(std::string(text).find("smp"), 0u) << text;
   for (unsigned i = 0; i < out.n_iters; i++)
      EXPECT_FALSE(out.iters[i].sample) << "a PDS sample";
}

/* a discard without split_phases: one program, the test into the keep
 * predicate and the feedback nop, no phase 1 */
TEST_F(sgx_compile_test, fragment_discard_one_phase)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *in =
      var(nir_var_shader_in, glsl_vec4_type(), "v", VARYING_SLOT_VAR0);
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_def *v = nir_load_var(&b, in);
   nir_discard_if(&b, nir_flt_imm(&b, nir_channel(&b, v, 0), 0.5));
   nir_store_var(&b, col, v, 0xf);

   ASSERT_TRUE(compile()) << out.log;
   expect_decodes();
   EXPECT_TRUE(out.discards);
   EXPECT_EQ(out.n_code1, 0u);
   bool feedback = false;
   for (unsigned i = 0; i < out.n_blocks; i++)
      feedback |= out.blocks[i].kind == SGX_BLOCK_FEEDBACK;
   EXPECT_TRUE(feedback);
}

/* more constants than SA holds: the rest are loaded from memory (ld.a.d) */
TEST_F(sgx_compile_test, vertex_constants_in_memory)
{
   start(MESA_SHADER_VERTEX);
   nir_variable *arr = var(nir_var_uniform,
                           glsl_array_type(glsl_vec4_type(), 48, 0), "u", -1);
   nir_variable *pos = var(nir_var_shader_out, glsl_vec4_type(), "gl_Position",
                           VARYING_SLOT_POS);
   nir_deref_instr *ad = nir_build_deref_var(&b, arr);
   nir_def *r = NULL;
   for (unsigned i = 0; i < 48; i++) {
      nir_def *e = nir_load_deref(&b, nir_build_deref_array_imm(&b, ad, i));
      r = r ? nir_fmul(&b, r, e) : e;
   }
   nir_store_var(&b, pos, r, 0xf);

   ASSERT_TRUE(compile()) << out.log;
   expect_decodes();
   EXPECT_NE(disasm().find("ld.a.d"), std::string::npos) << disasm();
}

/* what feeds a derivative runs on helper instances too: skipinv clear */
TEST_F(sgx_compile_test, fragment_derivative_source_no_skipinv)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *in =
      var(nir_var_shader_in, glsl_vec4_type(), "v", VARYING_SLOT_VAR0);
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_def *v = nir_load_var(&b, in);
   nir_store_var(&b, col, nir_ddx(&b, nir_fmul(&b, v, v)), 0xf);

   ASSERT_TRUE(compile()) << out.log;
   expect_decodes();
   std::string s = disasm();
   size_t d = s.find("fdsx");
   ASSERT_NE(d, std::string::npos) << s;
   /* the multiply before it */
   size_t m = s.rfind("fmad", d);
   ASSERT_NE(m, std::string::npos) << s;
   std::string line = s.substr(m, s.find('\n', m) - m);
   EXPECT_EQ(line.find("skipinv"), std::string::npos) << s;
}

/* gl_FragDepth: the instruction that writes pa<depth_pa> (the clamp) */
static std::string
depth_write(const std::string &s, unsigned pa)
{
   std::string want = "pa" + std::to_string(pa) + ",";
   size_t at = 0;
   while ((at = s.find(want, at)) != std::string::npos) {
      size_t b = s.rfind('\n', at);
      b = b == std::string::npos ? 0 : b + 1;
      std::string line = s.substr(b, s.find('\n', at) - b);
      if (line.find("fminmax") != std::string::npos)
         return line;
      at++;
   }
   return "";
}

class sgx_depth_test : public sgx_compile_test {
 protected:
   /* gl_FragColor = v; gl_FragDepth = the constant k, or v[ch] when k < 0 */
   void build(float k = -1.0f, bool discard = false, unsigned ch = 2)
   {
      start(MESA_SHADER_FRAGMENT);
      nir_variable *in =
         var(nir_var_shader_in, glsl_vec4_type(), "v", VARYING_SLOT_VAR0);
      nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(),
                              "gl_FragColor", FRAG_RESULT_COLOR);
      nir_variable *z = var(nir_var_shader_out, glsl_float_type(),
                            "gl_FragDepth", FRAG_RESULT_DEPTH);
      nir_def *v = nir_load_var(&b, in);
      if (discard)
         nir_discard_if(&b, nir_flt_imm(&b, nir_channel(&b, v, 0), 0.5));
      nir_store_var(&b, col, v, 0xf);
      nir_store_var(
         &b, z, k >= 0.0f ? nir_imm_float(&b, k) : nir_channel(&b, v, ch), 0x1);
   }

   /* the depth is clamped into its own PA register, after the inputs and
    * the colour, and the program has a feedback point for the depthf */
   void expect_depth()
   {
      expect_decodes();
      EXPECT_TRUE(out.writes_depth);
      EXPECT_NE(out.depth_pa, out.colour_pa);
      EXPECT_LT(out.depth_pa, out.n_pa);
      std::string s = disasm();
      std::string w = depth_write(s, out.depth_pa);
      ASSERT_NE(w, "") << s;
      /* clamped to [0, 1]: fminmax d, x, 0, 1 */
      EXPECT_NE(w.find("c48"), std::string::npos) << w;
      EXPECT_NE(w.find("c52"), std::string::npos) << w;
      bool feedback = false;
      for (unsigned i = 0; i < out.n_blocks; i++)
         feedback |= out.blocks[i].kind == SGX_BLOCK_FEEDBACK;
      EXPECT_TRUE(feedback);
      for (unsigned i = 0; i < out.n_iters; i++)
         EXPECT_LT(out.iters[i].pa, out.depth_pa);
   }
};

TEST_F(sgx_depth_test, fragment_depth_written)
{
   build();
   ASSERT_TRUE(compile()) << out.log;
   expect_depth();
   EXPECT_FALSE(out.discards);
}

TEST_F(sgx_depth_test, fragment_depth_and_discard)
{
   build(-1.0f, true);
   ASSERT_TRUE(compile()) << out.log;
   expect_depth();
   EXPECT_TRUE(out.discards);
}

/* gl_FragDepth = v.x: the colour is packed over pa0, where v.x is, so the
 * depth is clamped out of it first */
TEST_F(sgx_depth_test, fragment_depth_read_before_colour)
{
   build(-1.0f, false, 0);
   ASSERT_TRUE(compile()) << out.log;
   expect_depth();
   int clamp = -1, colour = -1;
   std::string want = "pa" + std::to_string(out.colour_pa) + ",";
   for (unsigned i = 0; i < out.n_code; i++) {
      char text[256];
      sgx_disasm_one(out.code[i], NULL, text, sizeof(text));
      std::string t(text);
      if (t.find("fminmax pa" + std::to_string(out.depth_pa) + ",") !=
          std::string::npos)
         clamp = (int)i;
      else if (colour < 0 && t.find("pck.u8") != std::string::npos &&
               t.find(want) != std::string::npos)
         colour = (int)i;
   }
   ASSERT_GE(clamp, 0) << disasm();
   ASSERT_GE(colour, 0) << disasm();
   EXPECT_LT(clamp, colour) << disasm();
}

/* a constant depth is a register write too (the clamp folds or reads a
 * constant), not dropped */
TEST_F(sgx_depth_test, fragment_depth_constant)
{
   build(0.25f);
   ASSERT_TRUE(compile()) << out.log;
   expect_decodes();
   EXPECT_TRUE(out.writes_depth);
   EXPECT_LT(out.depth_pa, out.n_pa);
}

/* split_phases: a program that writes depth stays one phase (the depth is
 * read at the feedback point, where phase 0 would end), with the depth
 * written before the feedback nop */
TEST_F(sgx_depth_test, fragment_depth_not_split)
{
   build(-1.0f, true);
   split_phases = true;
   ASSERT_TRUE(compile()) << out.log;
   EXPECT_EQ(out.n_code1, 0u);
   EXPECT_TRUE(out.writes_depth);
   int fb = -1;
   for (unsigned i = 0; i < out.n_blocks; i++)
      if (out.blocks[i].kind == SGX_BLOCK_FEEDBACK)
         fb = (int)out.blocks[i].at;
   ASSERT_GE(fb, 0);
   EXPECT_EQ(out.code[fb], (uint64_t)0xf8000140u << 32);
   int wrote = -1;
   for (int i = 0; i < fb; i++) {
      char text[256];
      sgx_disasm_one(out.code[i], NULL, text, sizeof(text));
      std::string t(text);
      if (t.find("fminmax pa" + std::to_string(out.depth_pa) + ",") !=
          std::string::npos)
         wrote = i;
   }
   EXPECT_GE(wrote, 0) << disasm();
}

/* a derivative never writes a register it reads: the neighbours of a quad
 * don't all read before any writes */
TEST_F(sgx_compile_test, fragment_derivative_dest_not_source)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *in =
      var(nir_var_shader_in, glsl_vec4_type(), "v", VARYING_SLOT_VAR0);
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_def *v = nir_load_var(&b, in);
   nir_def *a = nir_fabs(&b, nir_fmul(&b, v, v));
   nir_def *d = nir_fadd(&b, nir_ddy(&b, a), nir_ddx(&b, a));
   nir_def *fw =
      nir_fadd(&b, nir_fabs(&b, nir_ddx(&b, v)), nir_fabs(&b, nir_ddy(&b, v)));
   nir_store_var(&b, col, nir_fmul(&b, d, fw), 0xf);

   ASSERT_TRUE(compile()) << out.log;
   expect_decodes();
   std::string s = disasm();
   unsigned n = 0;
   for (size_t at = 0; (at = s.find("fds", at)) != std::string::npos; at++) {
      std::string line = s.substr(at, s.find('\n', at) - at);
      /* fdsx d, x, x */
      char op[8], dst[16], src[16];
      ASSERT_EQ(sscanf(line.c_str(), "%7s %15[^,], %15[^,],", op, dst, src), 3)
         << line;
      EXPECT_STRNE(dst, src) << line;
      n++;
   }
   EXPECT_GE(n, 8u) << s;
}

/* derivatives of what isn't a varying: a uniform, a constant (both 0),
 * gl_FragCoord (1 a pixel), and the fine and coarse forms */
class sgx_deriv_test : public sgx_compile_test {
 protected:
   nir_variable *col = NULL;

   void begin()
   {
      start(MESA_SHADER_FRAGMENT);
      col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                FRAG_RESULT_COLOR);
   }

   /* compiles, decodes, and a constant source never reaches an fds */
   void check()
   {
      ASSERT_TRUE(compile(f16)) << out.log;
      expect_decodes();
      std::string s = disasm();
      for (size_t at = 0; (at = s.find("fds", at)) != std::string::npos; at++) {
         std::string line = s.substr(at, s.find('\n', at) - at);
         EXPECT_EQ(line.find(", c"), std::string::npos) << line;
         EXPECT_EQ(line.find("#"), std::string::npos) << line;
      }
   }

   bool f16 = false;
};

TEST_F(sgx_deriv_test, fragment_derivative_of_uniform)
{
   begin();
   nir_variable *u = var(nir_var_uniform, glsl_vec4_type(), "u", -1);
   nir_def *v = nir_load_var(&b, u);
   nir_store_var(&b, col, nir_fadd(&b, nir_ddx(&b, v), nir_ddy(&b, v)), 0xf);
   check();
   EXPECT_EQ(disasm().find("fds"), std::string::npos) << disasm();
}

TEST_F(sgx_deriv_test, fragment_derivative_of_constant)
{
   begin();
   nir_variable *in =
      var(nir_var_shader_in, glsl_vec4_type(), "v", VARYING_SLOT_VAR0);
   nir_def *k = nir_imm_vec4(&b, 0.25f, 0.5f, 0.75f, 1.0f);
   nir_store_var(&b, col, nir_fadd(&b, nir_load_var(&b, in), nir_ddy(&b, k)),
                 0xf);
   check();
}

TEST_F(sgx_deriv_test, fragment_derivative_of_fragcoord)
{
   begin();
   nir_variable *fc = var(nir_var_shader_in, glsl_vec4_type(), "gl_FragCoord",
                          VARYING_SLOT_POS);
   nir_def *p = nir_load_var(&b, fc);
   nir_def *d = nir_vec4(&b, nir_ddx(&b, nir_channel(&b, p, 0)),
                         nir_ddy(&b, nir_channel(&b, p, 1)),
                         nir_imm_float(&b, 0.0f), nir_imm_float(&b, 1.0f));
   nir_store_var(&b, col, d, 0xf);
   check();
   EXPECT_NE(disasm().find("fdsx"), std::string::npos) << disasm();
   EXPECT_NE(disasm().find("fdsy"), std::string::npos) << disasm();
}

TEST_F(sgx_deriv_test, fragment_derivative_fine_and_coarse)
{
   begin();
   nir_variable *in =
      var(nir_var_shader_in, glsl_vec4_type(), "v", VARYING_SLOT_VAR0);
   nir_def *v = nir_fmul(&b, nir_load_var(&b, in), nir_load_var(&b, in));
   nir_def *d =
      nir_fadd(&b, nir_fadd(&b, nir_ddx_fine(&b, v), nir_ddy_fine(&b, v)),
               nir_fadd(&b, nir_ddx_coarse(&b, v), nir_ddy_coarse(&b, v)));
   nir_store_var(&b, col, d, 0xf);
   check();
}

/* mediump in F16 mode: the source is format-selected, the result F16 */
TEST_F(sgx_deriv_test, fragment_derivative_mediump)
{
   begin();
   nir_variable *in =
      var(nir_var_shader_in, glsl_vec4_type(), "v", VARYING_SLOT_VAR0);
   in->data.precision = GLSL_PRECISION_MEDIUM;
   col->data.precision = GLSL_PRECISION_MEDIUM;
   nir_def *v = nir_f2f16(&b, nir_load_var(&b, in));
   nir_def *d = nir_fadd(&b, nir_ddx(&b, v), nir_ddy(&b, v));
   nir_store_var(&b, col, nir_f2f32(&b, d), 0xf);
   f16 = true;
   check();
}

/* user clip planes per pixel: the clip distances as varyings, one
 * discard on any negative enabled one. The driver moves the CLIP_DIST
 * slots to VAR slots as it does other built-ins; here they are moved by
 * hand */
static void
move_clipdist(nir_shader *s, nir_variable_mode mode)
{
   nir_foreach_variable_with_modes (v, s, mode)
      if (v->data.location == VARYING_SLOT_CLIP_DIST0 ||
          v->data.location == VARYING_SLOT_CLIP_DIST1)
         v->data.location = VARYING_SLOT_VAR0 + 8 +
                            (v->data.location - VARYING_SLOT_CLIP_DIST0);
}

TEST_F(sgx_compile_test, fragment_clip_planes_discard)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *in =
      var(nir_var_shader_in, glsl_vec4_type(), "v", VARYING_SLOT_VAR0);
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_store_var(&b, col, nir_load_var(&b, in), 0xf);
   ASSERT_TRUE(sgx_lower_clip_fs(b.shader, 0x3));
   move_clipdist(b.shader, nir_var_shader_in);

   ASSERT_TRUE(compile()) << out.log;
   expect_decodes();
   EXPECT_TRUE(out.discards);
   EXPECT_EQ(out.n_iters, 2u); /* v and clipdist_0 */
}

/* planes 0 and 5: both clip-distance vec4s */
TEST_F(sgx_compile_test, fragment_clip_planes_two_sets)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *in =
      var(nir_var_shader_in, glsl_vec4_type(), "v", VARYING_SLOT_VAR0);
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_store_var(&b, col, nir_load_var(&b, in), 0xf);
   ASSERT_TRUE(sgx_lower_clip_fs(b.shader, 0x21));
   move_clipdist(b.shader, nir_var_shader_in);

   ASSERT_TRUE(compile()) << out.log;
   expect_decodes();
   EXPECT_TRUE(out.discards);
   EXPECT_EQ(out.n_iters, 3u);
}

/* no planes: nothing changes */
TEST_F(sgx_compile_test, fragment_clip_planes_none)
{
   start(MESA_SHADER_FRAGMENT);
   EXPECT_FALSE(sgx_lower_clip_fs(b.shader, 0));
}

/* gl_ClipVertex through st's lowering (nir_lower_clip_vs): the vertex
 * shader writes the clip distances as an output set; the clip vertex
 * itself isn't an output */
TEST_F(sgx_compile_test, vertex_clip_vertex_to_clip_distances)
{
   start(MESA_SHADER_VERTEX);
   nir_variable *a =
      var(nir_var_shader_in, glsl_vec4_type(), "a", VERT_ATTRIB_GENERIC0);
   nir_variable *pos = var(nir_var_shader_out, glsl_vec4_type(), "gl_Position",
                           VARYING_SLOT_POS);
   nir_variable *cv = var(nir_var_shader_out, glsl_vec4_type(), "gl_ClipVertex",
                          VARYING_SLOT_CLIP_VERTEX);
   nir_def *p = nir_load_var(&b, a);
   nir_store_var(&b, pos, p, 0xf);
   nir_store_var(&b, cv, nir_fmul_imm(&b, p, 2.0), 0xf);
   b.shader->info.outputs_written =
      VARYING_BIT_POS | BITFIELD64_BIT(VARYING_SLOT_CLIP_VERTEX);

   /* (st's STATE_CLIPPLANE; only the uniforms' names matter here) */
   gl_state_index16 state[8][STATE_LENGTH] = {};
   for (unsigned i = 0; i < 8; i++) {
      state[i][0] = 1;
      state[i][1] = i;
   }
   ASSERT_TRUE(nir_lower_clip_vs(b.shader, 0x3, true, false, state));
   move_clipdist(b.shader, nir_var_shader_out);

   ASSERT_TRUE(compile()) << out.log;
   expect_decodes();
   unsigned sets = 0;
   for (unsigned i = 0; i < SGX_MAX_SETS; i++)
      sets += out.set_comps[i] != 0;
   EXPECT_EQ(sets, 1u) << disasm(); /* clipdist_0 */
}

/* polygon stipple as nir_lower_pstipple_fs builds it (bool1): a sample of
 * the 32x32 stipple texture at gl_FragCoord / 32, by texture index with no
 * deref, and a discard on its alpha */
TEST_F(sgx_compile_test, fragment_polygon_stipple)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *in =
      var(nir_var_shader_in, glsl_vec4_type(), "v", VARYING_SLOT_VAR0);
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_variable *fc = var(nir_var_shader_in, glsl_vec4_type(), "gl_FragCoord",
                          VARYING_SLOT_POS);
   nir_variable *st =
      var(nir_var_uniform,
          glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT),
          "stipple_tex", -1);
   st->data.binding = 1;
   st->data.explicit_binding = true;
   BITSET_SET(b.shader->info.textures_used, 1);
   BITSET_SET(b.shader->info.samplers_used, 1);

   nir_def *tc = nir_fmul(&b, nir_trim_vector(&b, nir_load_var(&b, fc), 2),
                          nir_imm_vec2(&b, 1.0 / 32.0, 1.0 / 32.0));
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 1);
   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->coord_components = 2;
   tex->dest_type = nir_type_float32;
   tex->texture_index = 1;
   tex->sampler_index = 1;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, tc);
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   nir_discard_if(&b, nir_fneu_imm(&b, nir_channel(&b, &tex->def, 3), 0.0));
   b.shader->info.fs.uses_discard = true;
   nir_store_var(&b, col, nir_load_var(&b, in), 0xf);

   ASSERT_TRUE(compile()) << out.log;
   expect_decodes();
   EXPECT_TRUE(out.discards);
   ASSERT_EQ(out.n_samples, 1u);
   EXPECT_EQ(out.samples[0].sampler, 1u);
}

/* texture() of a sampler of this dimension at coordinate c */
static nir_def *
sample_dim(nir_builder *b, nir_variable *smp, nir_def *c, bool shadow = false)
{
   nir_deref_instr *sd = nir_build_deref_var(b, smp);
   struct nir_tex_builder tb = {};
   tb.coord = c;
   tb.texture_deref = sd;
   tb.sampler_deref = sd;
   tb.dim = glsl_get_sampler_dim(smp->type);
   tb.dest_type = nir_type_float32;
   if (shadow) {
      tb.comparator = nir_channel(b, c, 2);
      tb.coord = nir_trim_vector(b, c, 2);
   }
   return nir_build_tex_struct(b, nir_texop_tex, tb);
}

/* a compare sample reaching the compiler is refused (the driver lowers it) */
TEST_F(sgx_compile_test, fragment_shadow_sampler_refused)
{
   start(MESA_SHADER_FRAGMENT);
   nir_variable *in =
      var(nir_var_shader_in, glsl_vec_type(3), "v", VARYING_SLOT_VAR0);
   nir_variable *smp =
      var(nir_var_uniform,
          glsl_sampler_type(GLSL_SAMPLER_DIM_2D, true, false, GLSL_TYPE_FLOAT),
          "t", -1);
   nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(), "gl_FragColor",
                           FRAG_RESULT_COLOR);
   nir_def *r = sample_dim(&b, smp, nir_load_var(&b, in), true);
   nir_store_var(&b, col, nir_vec4(&b, r, r, r, r), 0xf);
   EXPECT_FALSE(compile());
   EXPECT_NE(std::string(out.log ? out.log : "").find("shadow"),
             std::string::npos);
}

/* a volume sample is smp's 3D dimension, as a cube map's is: from the PDS
 * (a whole vec3 varying) and dependent */
TEST_F(sgx_compile_test, fragment_volume_sample_is_3d)
{
   for (unsigned dep = 0; dep < 2; dep++) {
      start(MESA_SHADER_FRAGMENT);
      nir_variable *in =
         var(nir_var_shader_in, glsl_vec_type(3), "v", VARYING_SLOT_VAR0);
      nir_variable *smp = var(
         nir_var_uniform,
         glsl_sampler_type(GLSL_SAMPLER_DIM_3D, false, false, GLSL_TYPE_FLOAT),
         "t", -1);
      nir_variable *col = var(nir_var_shader_out, glsl_vec4_type(),
                              "gl_FragColor", FRAG_RESULT_COLOR);
      nir_def *c = nir_load_var(&b, in);
      if (dep)
         c = nir_fmul(&b, c, c);
      nir_store_var(&b, col, sample_dim(&b, smp, c), 0xf);
      ASSERT_TRUE(compile()) << out.log;
      expect_decodes();
      ASSERT_EQ(out.n_samples, 1u);
      EXPECT_EQ(out.samples[0].dim, 3u);
      EXPECT_EQ(out.samples[0].dependent, (bool)dep);
      if (dep) {
         char text[256];
         sgx_disasm_one(out.samples[0].smp, NULL, text, sizeof(text));
         EXPECT_NE(std::string(text).find("smp.3d"), std::string::npos) << text;
      }
      sgx_shader_free(&out);
      compiled = false;
      ralloc_free(b.shader);
      b.shader = NULL;
   }
}

/* the fixed programs: every line assembles, every word decodes, and each
 * program's last instruction ends it */
TEST(sgx_fixed, programs_assemble)
{
   struct sgx_fixed_progs p;
   char err[256] = "";
   ASSERT_TRUE(sgx_fixed_progs_build(&p, err, sizeof(err))) << err;
   const struct sgx_fixed_prog *all[] = {
      &p.empty,    &p.clear_vs,       &p.vpgen_vs,   &p.copy_fs,
      &p.vs_xy,    &p.vs_xyz,         &p.fs_uniform_sec, &p.fs_uniform,
      &p.pe_ptoff, &p.pe_emit,
   };
   std::vector<const struct sgx_fixed_prog *> v(all, all + ARRAY_SIZE(all));
   for (unsigned n = 1; n <= 32; n++)
      v.push_back(&p.state_emit[n]);
   for (const struct sgx_fixed_prog *x : v) {
      ASSERT_GT(x->n, 0u) << x->name;
      for (unsigned i = 0; i < x->n; i++) {
         char text[256];
         uint64_t w = x->w[2 * i] | (uint64_t)x->w[2 * i + 1] << 32;
         EXPECT_TRUE(sgx_disasm_one(w, NULL, text, sizeof(text))) << x->name;
         if (i == x->n - 1)
            EXPECT_NE(std::string(text).find("end"), std::string::npos)
               << x->name << ": " << text;
      }
   }
   /* phas.i @label: the target is the label's instruction, to relocate */
   ASSERT_EQ(p.vs_xy.nreloc, 2u);
   EXPECT_EQ(p.vs_xy.reloc[0], 0u);
   EXPECT_EQ(p.vs_xy.reloc[1], 7u);
}

/* the words are the ones the winsys used before (sgx_fixed_golden.h) */
TEST(sgx_fixed, programs_match_golden)
{
   struct sgx_fixed_progs p;
   char err[256] = "";
   ASSERT_TRUE(sgx_fixed_progs_build(&p, err, sizeof(err))) << err;
   const struct {
      const char *name;
      const struct sgx_fixed_prog *prog;
   } named[] = {
      {"empty", &p.empty},       {"clear_vs", &p.clear_vs},
      {"vpgen_vs", &p.vpgen_vs}, {"copy_fs", &p.copy_fs},
      {"vs_xy", &p.vs_xy},       {"vs_xyz", &p.vs_xyz},
      {"fs_uniform_sec", &p.fs_uniform_sec},
      {"fs_uniform", &p.fs_uniform},
      {"pe_ptoff", &p.pe_ptoff}, {"pe_emit", &p.pe_emit},
   };
   unsigned checked = 0;
   for (const struct sgx_fixed_golden &g : sgx_fixed_golden) {
      const struct sgx_fixed_prog *x = NULL;
      unsigned k;
      if (sscanf(g.name, "state_emit_%u", &k) == 1 && k >= 1 && k <= 32)
         x = &p.state_emit[k];
      for (const auto &e : named) {
         if (!strcmp(e.name, g.name))
            x = e.prog;
      }
      ASSERT_NE(x, nullptr) << g.name;
      ASSERT_EQ(x->n, g.n) << g.name;
      for (unsigned i = 0; i < 2 * g.n; i++)
         EXPECT_EQ(x->w[i], g.w[i]) << g.name << " word " << i;
      ASSERT_EQ(x->nreloc, g.nreloc) << g.name;
      for (unsigned i = 0; i < g.nreloc; i++)
         EXPECT_EQ(x->reloc[i], g.reloc[i]) << g.name;
      checked++;
   }
   EXPECT_EQ(checked, 42u);
}

} /* namespace */
