/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

#ifndef SGX_CONTEXT_H
#define SGX_CONTEXT_H

#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "util/list.h"
#include "util/slab.h"
#include "util/u_blitter.h"

#include "sgx_compiler.h"
#include "sgxws.h"

struct sgx_resource;
struct nir_shader;
struct sgx_varyings;

/* what a shader turned out to be (sgx_program.c) */
enum sgx_shader_kind {
   SGX_SHADER_UNSUPPORTED,
   SGX_SHADER_COMPILED,    /* by mesa/src/sgx/compiler */
   SGX_SHADER_VS_POSITION, /* gl_Position = attribute 0 */
   SGX_SHADER_FS_UNIFORM,  /* gl_FragColor = a vec4 uniform */
   SGX_SHADER_FS_CONSTANT, /* gl_FragColor = a constant */
};

/* a constant in secondary attributes: sa[base + slot] = constant buffer 0 float
 * k */
struct sgx_const {
   uint16_t k, slot, fmt,
      shift;     /* fmt: 1 f32, 2 F16 half at shift, 3 C10 channel at shift,
                  * 0x10 F16 half of the C10 value, 0x11 the literal `value`;
                  * slot 0xffff: in the memory block only */
   uint16_t mem; /* word of the constant memory block, 0 none */
   uint32_t value;
};

#define SGX_MAX_CONSTS 256 /* SA dwords of constants (sa127 ends them) */

#define SGX_MAX_DEP_SAMPLES 8

/* the fragment driver block's register that sets gl_FrontFacing's sense:
 * bit 0 clear when gallium's front_ccw is set (A/B against the DDK) */
#define SGX_SA_FRONT 5

struct sgx_pipe_shader {
   struct nir_shader *nir;
   enum sgx_shader_kind kind;
   unsigned
      ubo_offset; /* FS_UNIFORM: byte offset of the vec4 in constant buffer 0 */
   float color[4]; /* FS_CONSTANT */
   /* COMPILED: */
   struct sgxws_prog prog;
   unsigned n_consts, const_dwords;
   struct sgx_const consts[SGX_MAX_CONST_MAP];
   /* constants past sa127 (the compiler's ld.a.d): a block of mem_dwords per
    * draw, its address + 512 * j in SA slot mem_base_slot[j] */
   unsigned mem_dwords, n_mem_bases;
   uint8_t mem_base_slot[8];
   unsigned n_attribs; /* vertex: attribute slots, 4 dwords each */
   /* fragment: the shader's code, kept for the blend variants (the colour
    * epilogue is the blend, sgxws_blend) */
   uint64_t *code; /* with the dependent samples' smp put in */
   unsigned n_code, color_reg, temps;
   int discard_pa; /* discards: the depth plane's first PA register; -1 doesn't */
   /* dependent samples: the SA register of each one's 4 texture state words,
    * and its sampler */
   unsigned dep_sa[SGX_MAX_DEP_SAMPLES], dep_sampler[SGX_MAX_DEP_SAMPLES],
      n_deps;
   struct {
      struct sgxws_blend key;
      uint32_t code;
   } variants[32];
   unsigned n_variants, next_variant;
   bool uses_front_face;      /* fragment: reads SGX_SA_FRONT */
   bool reads_pointcoord;     /* fragment: its varyings start at set 1,
                                 gl_PointCoord is set 0 */
   struct sgx_varyings *vary; /* compiled: the varyings' sets (a variant's: its
                                 fragment shader's) */
   /* vertex: the same compiled to the sets of a fragment shader whose
    * varyings differ from its own (sgx_vs_for) */
   struct sgx_pipe_shader *variants_vs[16];
   unsigned next_variant_vs;
   /* vertex: the input formats it unpacks (sgx_compile_options.vtx_fmt; all
    * 0, f32 x 4, unless SGX_DEBUG=zerocopy picked a variant) */
   uint8_t vtx_fmt[16];
   /* fragment: the same decoding what the samplers in its mask sample as
    * sRGB (sgx_fs_for) */
   unsigned srgb;
   struct sgx_pipe_shader *variants_srgb[4];
   unsigned next_variant_srgb;
};

struct sgx_vertex_elements {
   unsigned num_elements;
   struct pipe_vertex_element elements[PIPE_MAX_ATTRIBS];
};

struct sgx_rasterizer {
   struct pipe_rasterizer_state base;
};

struct sgx_dsa {
   struct pipe_depth_stencil_alpha_state base;
};

struct sgx_blend {
   struct pipe_blend_state base;
   bool enable;              /* render target 0 blends, in a way sop2 can */
   struct sgxws_blend hw[2]; /* by whether the colour buffer has alpha */
};

/* one render target's TA/3D structures, by size (sgxws_rt) */
#define SGX_MAX_RTS 8

struct vcache_chunk;

struct sgx_context {
   struct pipe_context base;

   struct sgxws_ctx *hw;
   struct sgxws_prog vs_xy, vs_xyz, fs_uniform;
   struct sgxws_rt *rts[SGX_MAX_RTS];
   uint32_t rt_va[SGX_MAX_RTS]; /* the target each was made for */
   uint32_t rt_w[SGX_MAX_RTS], rt_h[SGX_MAX_RTS]; /* and its size */
   uint64_t rt_used[SGX_MAX_RTS], rt_clock;       /* when each was last taken */
   unsigned num_rts;
   struct sgx_resource *recording; /* the target of the frame being recorded */
   struct sgx_resource *recording_zs; /* and its depth/stencil buffer */

   struct pipe_framebuffer_state framebuffer;
   struct pipe_viewport_state viewport;
   struct pipe_scissor_state scissor;
   struct pipe_blend_color blend_color;
   struct pipe_stencil_ref stencil_ref;
   struct sgx_rasterizer *rasterizer;
   struct sgx_dsa *dsa;
   struct sgx_blend *blend;
   struct sgx_vertex_elements *velems;
   struct sgx_pipe_shader *vs, *fs;
   struct pipe_vertex_buffer vertex_buffers[PIPE_MAX_ATTRIBS];
   unsigned num_vertex_buffers;
   struct pipe_constant_buffer constants[MESA_SHADER_STAGES];
   struct pipe_sampler_state *samplers[PIPE_MAX_SAMPLERS]; /* fragment */
   struct pipe_sampler_view *views[PIPE_MAX_SHADER_SAMPLER_VIEWS]; /* fragment */

   /* SGX_STATS=1: where the time goes, printed every 2 s */
   struct {
      bool on;
      uint64_t t0, cpu0, draw_ns, flush_ns, tex_ns, compile_ns, rb_ns, rb_bytes;
      unsigned frames, draws, verts, flushes, tex_writes, tex_flushes;
      unsigned fl_eof, fl_other, fl_switch, fl_read,
         fl_deferred; /* flushes by cause */
      unsigned vc_hits, vc_misses, vc_uncacheable, zc_draws;
      unsigned blit_gpu, blit_copy, blit_cpu;
      uint64_t blit_cpu_px, blit_cpu_ns;
   } stats;

   struct blitter_context
      *blitter; /* GPU blits: a quad through our own shaders */

   /* draws from unchanged buffers, their vertices gathered and indices
    * rebased once into device memory (sgx_draw.c) */
   struct hash_table *vcache;
   struct list_head vcache_lru; /* most recently used first */
   uint64_t vcache_bytes;
   struct list_head
      vcache_chunks; /* the device memory its entries are cut from */
   struct vcache_chunk *vcache_cur;

   struct slab_child_pool transfer_pool;
   uint32_t *staging; /* vertices gathered for one draw */
   size_t staging_size;
   uint16_t *idx_staging;   /* its 16-bit indices */
   uint32_t *mem_consts[2]; /* a draw's constants in memory, vertex and fragment
                               (sgx_draw.c) */
   unsigned idx_staging_size;
   bool warned_shader, warned_prim, warned_blend, warned_varyings, warned_cube,
      warned_frag_depth;
};

static inline struct sgx_context *
sgx_context(struct pipe_context *pctx)
{
   return (struct sgx_context *)pctx;
}

struct pipe_context *sgx_context_create(struct pipe_screen *pscreen, void *priv,
                                        unsigned flags);

void sgx_state_init(struct sgx_context *ctx);
void sgx_draw_init(struct sgx_context *ctx);
void sgx_draw_fini(struct sgx_context *ctx);
void sgx_program_init(struct sgx_context *ctx);

void sgx_stats_report(struct sgx_context *ctx);

/* submit the frame being recorded (its render runs on while the CPU goes on) */
void sgx_flush_frame(struct sgx_context *ctx);
/* submit it and wait for every render submitted */
void sgx_finish(struct sgx_context *ctx);
/* before the CPU touches res: finish any frame that renders into it */
void sgx_flush_resource_writes(struct sgx_context *ctx,
                               struct sgx_resource *res);

/* The frame this context records, if it still is one: a frame whose target
 * was destroyed went at the destruction (sgx_resource.c), and another
 * context's frame on the shared render context ends ours (sgxws_begin).
 * Compares addresses only: the resource may be gone. */
struct sgx_resource *sgx_recording(struct sgx_context *ctx);

#endif
