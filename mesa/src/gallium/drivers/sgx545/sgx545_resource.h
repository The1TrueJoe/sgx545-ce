/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

#ifndef SGX545_RESOURCE_H
#define SGX545_RESOURCE_H

#include "pipe/p_state.h"

#include "sgx545_frame.h"

struct sgx545_context;
struct sgx545_screen;

struct sgx545_resource {
   struct pipe_resource base;

   /*
    * Buffers (vertex, index, constant) live in CPU memory: the draw path
    * reads them and copies what the GPU needs into the frame's rings.
    * Textures and render targets are device memory.
    */
   void *data;
   struct sgxws_bo bo;
   bool wrapped;                /* resource_from_user_memory: an fbdev mapping */

   uint32_t uid, gen;           /* buffers: an id never reused, and a count of writes (vertex cache) */
   uint64_t written_ns;         /* buffers: when last written */
   uint32_t stride;             /* bytes per row of level 0 */
   size_t level_offset[16];     /* each level in the linear CPU copy */
   bool texture;                /* data is the CPU copy of bo (texture heap, laid out by sgx545_texture.c) */
   uint32_t pitch;              /* pixels per row of a render target (a multiple of 32) */

   /* A texture written while a render may still sample it: the write goes to
    * a second copy, which takes over (the DDK's ghosting).  alt lacks what
    * was written since it was current: alt_dirty boxes, or everything. */
   uint32_t use_seq;            /* the last kick that samples bo */
   struct sgxws_bo alt;
   uint32_t alt_use_seq;
   unsigned n_alt_dirty;        /* SGX545_ALT_ALL: re-encode everything */
   struct { uint16_t level, x0, y0, x1, y1; } alt_dirty[32];

   /* the kernel sync data, mapped once: the CPU waits on it before reading */
   struct pvr_buf sync_map;
   volatile PVRSRV_SYNC_DATA *sync_data;
};

static inline struct sgx545_resource *
sgx545_resource(struct pipe_resource *p)
{
   return (struct sgx545_resource *)p;
}

/* what the TPU can sample: a texture in the texture heap, or a 32-bit
 * render target read in place as a strided texture (TEX.md sections 3, 11) */
static inline bool
sgx545_resource_sampleable(const struct sgx545_resource *res)
{
   if (res->texture)
      return true;
   return res->bo.va && res->pitch && res->base.last_level == 0 &&
          (res->base.format == PIPE_FORMAT_B8G8R8A8_UNORM || res->base.format == PIPE_FORMAT_B8G8R8X8_UNORM ||
           res->base.format == PIPE_FORMAT_R8G8B8A8_UNORM || res->base.format == PIPE_FORMAT_R8G8B8X8_UNORM);
}

#define SGX545_ALT_ALL (~0u)

struct sgx545_transfer {
   struct pipe_transfer base;
};

void sgx545_resource_screen_init(struct sgx545_screen *screen);
void sgx545_resource_context_init(struct sgx545_context *ctx);

/* the target a render into this resource writes */
bool sgx545_resource_target(struct sgx545_screen *screen, struct sgx545_resource *res,
                            struct sgxws_target *t);

#endif
