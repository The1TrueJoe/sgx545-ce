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

} /* namespace */
