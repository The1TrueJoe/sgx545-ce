/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

#ifndef SGX_PROGRAM_H
#define SGX_PROGRAM_H

#include <stdint.h>

struct nir_shader_compiler_options;
struct sgx_context;
struct sgx_pipe_shader;
struct sgxws_blend;

const struct nir_shader_compiler_options *sgx_get_nir_options(void);
uint32_t sgx_fs_program(struct sgx_context *ctx, struct sgx_pipe_shader *so,
                        const struct sgxws_blend *b);

/* the compiled vertex shader vs as it goes with the compiled fragment shader
 * fs (or NULL) and inputs in vtx_fmt (or NULL, f32 x 4): itself when their
 * varyings' sets agree and the inputs are f32 x 4, else a variant writing
 * fs's sets (gl_PointCoord's set 0 included) and unpacking vtx_fmt; NULL if
 * it can't.  For points it keeps the point size st's program writes, which
 * every other draw goes without */
struct sgx_pipe_shader *sgx_vs_for(struct sgx_context *ctx,
                                   struct sgx_pipe_shader *vs,
                                   const struct sgx_pipe_shader *fs,
                                   const uint8_t *vtx_fmt, bool points);

/* the texture units a fragment shader samples (all, when it isn't known) */
uint32_t sgx_fs_textures_used(const struct sgx_pipe_shader *fs);

/* the bound fragment shader as the draw samples its textures: itself, or a
 * variant decoding the sRGB views it samples; NULL if it can't */
struct sgx_pipe_shader *sgx_fs_for(struct sgx_context *ctx,
                                   struct sgx_pipe_shader *fs);

#endif
