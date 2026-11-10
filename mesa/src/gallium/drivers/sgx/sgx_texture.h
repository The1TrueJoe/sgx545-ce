/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

#ifndef SGX_TEXTURE_H
#define SGX_TEXTURE_H

#include <stdbool.h>
#include <stdint.h>

#include "pipe/p_state.h"

struct sgx_context;
struct sgx_resource;

/* T1 format code for a sampled gallium format, or 0 (docs/TEX.md section 1) */
unsigned sgx_tex_format(enum pipe_format format);
unsigned sgx_tex_bpp(enum pipe_format format);
bool sgx_tex_bgra(enum pipe_format format);
unsigned sgx_tex_dev_last(const struct pipe_resource *prsc);
void sgx_tex_set_tail(struct sgx_resource *res, unsigned l);

/* the device layout of a texture: size in bytes, and each level and face's
 * offset */
size_t sgx_tex_size(const struct pipe_resource *prsc);
size_t sgx_tex_offset(const struct pipe_resource *prsc, unsigned level,
                      unsigned layer);

/* lay a level out in device memory from the resource's linear CPU copy */
void sgx_tex_encode_level(struct sgx_resource *res, unsigned level);
void sgx_tex_encode_box(struct sgx_resource *res, unsigned level,
                        unsigned layer, unsigned x0, unsigned y0, unsigned x1,
                        unsigned y1);
/* and back, after the GPU rendered into a level */
void sgx_tex_decode_level(struct sgx_resource *res, unsigned level);
void sgx_tex_decode_box(struct sgx_resource *res, unsigned level,
                        unsigned layer, unsigned x0, unsigned y0, unsigned x1,
                        unsigned y1);

/* a cube map's six level-0 faces alone (res->cube0), and whether a sampler
 * reads that copy: no mipmapping on a cube map that has levels */
size_t sgx_tex_cube0_size(const struct pipe_resource *prsc);
void sgx_tex_encode_cube0(struct sgx_resource *res);
bool sgx_tex_uses_cube0(const struct pipe_sampler_view *view,
                        const struct pipe_sampler_state *sampler);

/* the four DOUTT words for a view and sampler (docs/STATE.md section 5) */
void sgx_tex_words(const struct pipe_sampler_view *view,
                   const struct pipe_sampler_state *sampler, uint32_t t[4]);

#endif
