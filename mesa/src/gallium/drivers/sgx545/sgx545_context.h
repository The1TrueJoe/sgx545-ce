/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

#ifndef SGX545_CONTEXT_H
#define SGX545_CONTEXT_H

#include "pipe/p_context.h"
#include "util/u_blitter.h"
#include "pipe/p_state.h"
#include "util/slab.h"

#include "sgx545_frame.h"

struct sgx545_resource;
struct nir_shader;

/* what a shader turned out to be (sgx545_program.c) */
enum sgx545_shader_kind {
   SGX545_SHADER_UNSUPPORTED,
   SGX545_SHADER_COMPILED,      /* by mesa/src/sgx545/compiler */
   SGX545_SHADER_VS_POSITION,   /* gl_Position = attribute 0 */
   SGX545_SHADER_FS_UNIFORM,    /* gl_FragColor = a vec4 uniform */
   SGX545_SHADER_FS_CONSTANT,   /* gl_FragColor = a constant */
};

/* a constant in secondary attributes: sa[base + slot] = constant buffer 0 float k */
struct sgx545_const {
   uint16_t k, slot, fmt, shift;   /* fmt: 1 f32, 2 F16 half at shift, 3 C10 channel at shift */
};

#define SGX545_MAX_CONSTS 256

#define SGX545_MAX_DEP_SAMPLES 8

struct sgx545_shader {
   struct nir_shader *nir;
   enum sgx545_shader_kind kind;
   unsigned ubo_offset;         /* FS_UNIFORM: byte offset of the vec4 in constant buffer 0 */
   float color[4];              /* FS_CONSTANT */
   /* COMPILED: */
   struct sgxws_prog prog;
   unsigned n_consts, const_dwords;
   struct sgx545_const consts[SGX545_MAX_CONSTS];
   unsigned n_attribs;          /* vertex: attribute slots, 4 dwords each */
   /* fragment: the shader's code, kept for the blend variants (the colour
    * epilogue is the blend, sgxws_blend) */
   uint64_t *code;              /* with the dependent samples' smp put in */
   unsigned n_code, color_reg, temps;
   int discard_pa;              /* discards: the depth plane's first PA register; -1 doesn't */
   /* dependent samples: the SA register of each one's 4 texture state words,
    * and its sampler */
   unsigned dep_sa[SGX545_MAX_DEP_SAMPLES], dep_sampler[SGX545_MAX_DEP_SAMPLES], n_deps;
   struct {
      struct sgxws_blend key;
      uint32_t code;
   } variants[8];
   unsigned n_variants;
};

struct sgx545_vertex_elements {
   unsigned num_elements;
   struct pipe_vertex_element elements[PIPE_MAX_ATTRIBS];
};

struct sgx545_rasterizer {
   struct pipe_rasterizer_state base;
};

struct sgx545_dsa {
   struct pipe_depth_stencil_alpha_state base;
};

struct sgx545_blend {
   struct pipe_blend_state base;
   bool enable;                 /* render target 0 blends, in a way sop2 can */
   struct sgxws_blend hw[2];    /* by whether the colour buffer has alpha */
};

/* one render target's TA/3D structures, by size (sgxws_rt) */
#define SGX545_MAX_RTS 8

struct sgx545_context {
   struct pipe_context base;

   struct sgxws_ctx *hw;
   struct sgxws_prog vs_xy, vs_xyz, fs_uniform;
   struct sgxws_rt *rts[SGX545_MAX_RTS];
   uint32_t rt_va[SGX545_MAX_RTS];      /* the target each was made for */
   unsigned num_rts;
   struct sgx545_resource *recording;   /* the target of the frame being recorded */

   struct pipe_framebuffer_state framebuffer;
   struct pipe_viewport_state viewport;
   struct sgx545_rasterizer *rasterizer;
   struct sgx545_dsa *dsa;
   struct sgx545_blend *blend;
   struct sgx545_vertex_elements *velems;
   struct sgx545_shader *vs, *fs;
   struct pipe_vertex_buffer vertex_buffers[PIPE_MAX_ATTRIBS];
   unsigned num_vertex_buffers;
   struct pipe_constant_buffer constants[MESA_SHADER_STAGES];
   struct pipe_sampler_state *samplers[PIPE_MAX_SAMPLERS];          /* fragment */
   struct pipe_sampler_view *views[PIPE_MAX_SHADER_SAMPLER_VIEWS];  /* fragment */

   /* SGX545_STATS=1: where the time goes, printed every 2 s */
   struct {
      bool on;
      uint64_t t0, draw_ns, flush_ns, tex_ns, compile_ns;
      unsigned frames, draws, verts, flushes, tex_writes, tex_flushes;
   } stats;

   struct blitter_context *blitter;     /* GPU blits: a quad through our own shaders */

   struct slab_child_pool transfer_pool;
   uint32_t *staging;               /* vertices gathered for one draw */
   size_t staging_size;
   uint16_t *idx_staging;           /* its 16-bit indices */
   unsigned idx_staging_size;
   bool warned_shader, warned_prim, warned_blend, warned_varyings;
};

static inline struct sgx545_context *
sgx545_context(struct pipe_context *pctx)
{
   return (struct sgx545_context *)pctx;
}

struct pipe_context *sgx545_context_create(struct pipe_screen *pscreen, void *priv, unsigned flags);

void sgx545_state_init(struct sgx545_context *ctx);
void sgx545_draw_init(struct sgx545_context *ctx);
void sgx545_program_init(struct sgx545_context *ctx);

void sgx545_stats_report(struct sgx545_context *ctx);

/* submit the frame being recorded (its render runs on while the CPU goes on) */
void sgx545_flush_frame(struct sgx545_context *ctx);
/* submit it and wait for every render submitted */
void sgx545_finish(struct sgx545_context *ctx);
/* before the CPU touches res: finish any frame that renders into it */
void sgx545_flush_resource_writes(struct sgx545_context *ctx, struct sgx545_resource *res);

#endif
