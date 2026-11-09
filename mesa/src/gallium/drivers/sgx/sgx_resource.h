/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

#ifndef SGX_RESOURCE_H
#define SGX_RESOURCE_H

#include "pipe/p_state.h"

#include "sgxws.h"

struct sgx_context;
struct sgx_screen;

struct sgx_resource {
   struct pipe_resource base;

   /*
    * Buffers (vertex, index, constant) live in CPU memory: the draw path
    * reads them and copies what the GPU needs into the frame's rings.
    * Textures and render targets are device memory.
    */
   void *data;
   struct sgxws_bo bo;
   bool wrapped; /* resource_from_user_memory: an fbdev mapping */
   bool shared;  /* imported or exported dma-buf: its frames name it
                    (sgxws_use_bo) */
   struct renderonly_scanout
      *scanout; /* kmsro: its buffer on the display device */

   uint32_t uid, gen; /* buffers: an id never reused, and a count of writes
                         (vertex cache) */
   /* buffers (SGX_DEBUG=zerocopy): a device copy of data that draws DMA
    * from, made at the first draw that reads it, kept up to date by writes;
    * vbo_seq is the last kick that reads it */
   struct sgxws_bo vbo;
   uint32_t vbo_seq;
   uint64_t written_ns;     /* buffers: when last written */
   uint32_t stride;         /* bytes per row of level 0 */
   size_t level_offset[16]; /* each level in the linear CPU copy */
   bool texture;      /* data is the CPU copy of bo (texture heap, laid out by
                         sgx_texture.c) */
   uint32_t rendered; /* a texture's levels rendered into since data was last
                         brought up to date */
   uint32_t pitch;    /* pixels per row of a render target (a multiple of 32) */

   /* A texture written while a render may still sample it: the write goes to
    * a second copy, which takes over (the DDK's ghosting).  alt lacks what
    * was written since it was current: alt_dirty boxes, or everything. */
   uint32_t use_seq; /* the last kick that samples bo */
   struct sgxws_bo alt;
   uint32_t alt_use_seq;
   unsigned n_alt_dirty; /* SGX_ALT_ALL: re-encode everything */
   struct {
      uint16_t level, layer, x0, y0, x1, y1;
   } alt_dirty[32];

   /* A cube map with mip levels sampled without mipmapping: the TPU then
    * spaces the faces by level 0 alone, so it samples a copy of the six
    * level-0 faces laid out that way (docs/TEX.md: 0x1800 bytes for a 16x16
    * RGBA cube with 5 levels, 0x3000 with mipmapping).
    * level0_gen counts the writes to level 0; cube0_gen is the one cube0 has. */
   struct sgxws_bo cube0;
   uint32_t level0_gen, cube0_gen;

   /* A depth/stencil buffer's contents between renders: the ZLS buffer its
    * renders store and load (sgxws_target), and the render size they were
    * last stored at (0: nothing stored yet, so nothing to load). */
   struct sgxws_bo zls;
   uint32_t zls_bytes, zls_w, zls_h;
};

static inline struct sgx_resource *
sgx_resource(struct pipe_resource *p)
{
   return (struct sgx_resource *)p;
}

/* what the TPU can sample: a texture in the texture heap, or a 32-bit
 * render target read in place as a strided texture (TEX.md sections 3, 11) */
static inline bool
sgx_resource_sampleable(const struct sgx_resource *res)
{
   if (res->texture)
      return true;
   return res->bo.va && res->pitch && res->base.last_level == 0 &&
          (res->base.format == PIPE_FORMAT_B8G8R8A8_UNORM ||
           res->base.format == PIPE_FORMAT_B8G8R8X8_UNORM ||
           res->base.format == PIPE_FORMAT_R8G8B8A8_UNORM ||
           res->base.format == PIPE_FORMAT_R8G8B8X8_UNORM);
}

#define SGX_ALT_ALL (~0u)

struct sgx_transfer {
   struct pipe_transfer base;
};

void sgx_resource_screen_init(struct sgx_screen *screen);
/* with the screen lock held: a buffer's device copy, made if need be */
bool sgx_buffer_vbo(struct sgx_context *ctx, struct sgx_resource *res);
void sgx_resource_context_init(struct sgx_context *ctx);

/* the target a render into this level and layer of the resource writes; with
 * the screen lock held */
bool sgx_resource_target(struct sgx_screen *screen, struct sgx_resource *res,
                         unsigned level, unsigned layer,
                         struct sgxws_target *t);

/* brings res->cube0 up to date with level 0 (sgx_tex_words samples it) */
bool sgx_resource_cube0(struct sgx_context *ctx, struct sgx_resource *res);

#endif
