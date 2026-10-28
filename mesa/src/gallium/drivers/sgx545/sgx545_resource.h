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

   uint32_t stride;             /* bytes per row of level 0 */
   size_t level_offset[16];     /* each level in the linear CPU copy */
   bool texture;                /* data is the CPU copy of bo (texture heap, laid out by sgx545_texture.c) */
   uint32_t pitch;              /* pixels per row of a render target (a multiple of 32) */

   /* the kernel sync data, mapped once: the CPU waits on it before reading */
   struct pvr_buf sync_map;
   volatile PVRSRV_SYNC_DATA *sync_data;
};

static inline struct sgx545_resource *
sgx545_resource(struct pipe_resource *p)
{
   return (struct sgx545_resource *)p;
}

struct sgx545_transfer {
   struct pipe_transfer base;
};

void sgx545_resource_screen_init(struct sgx545_screen *screen);
void sgx545_resource_context_init(struct sgx545_context *ctx);

/* the target a render into this resource writes */
bool sgx545_resource_target(struct sgx545_screen *screen, struct sgx545_resource *res,
                            struct sgxws_target *t);

#endif
