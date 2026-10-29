/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

#ifndef SGX545_TEXTURE_H
#define SGX545_TEXTURE_H

#include <stdbool.h>
#include <stdint.h>

#include "pipe/p_state.h"

struct sgx545_context;
struct sgx545_resource;

/* T1 format code for a sampled gallium format, or 0 (docs/TEX.md section 1) */
unsigned sgx545_tex_format(enum pipe_format format);

/* the device layout of a texture: size in bytes, and each level's offset */
size_t sgx545_tex_size(const struct pipe_resource *prsc);
size_t sgx545_tex_level_offset(const struct pipe_resource *prsc, unsigned level);

/* lay a level out in device memory from the resource's linear CPU copy */
void sgx545_tex_encode_level(struct sgx545_resource *res, unsigned level);
void sgx545_tex_encode_box(struct sgx545_resource *res, unsigned level, unsigned x0, unsigned y0,
                           unsigned x1, unsigned y1);

/* the four DOUTT words for a view and sampler (docs/STATE.md section 5) */
void sgx545_tex_words(const struct pipe_sampler_view *view, const struct pipe_sampler_state *sampler,
                      uint32_t t[4]);

#endif
