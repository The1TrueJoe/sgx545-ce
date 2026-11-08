/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

#ifndef SGX545_PROGRAM_H
#define SGX545_PROGRAM_H

#include <stdint.h>

struct nir_shader_compiler_options;
struct sgx545_context;
struct sgx545_shader;
struct sgxws_blend;

const struct nir_shader_compiler_options *sgx545_get_nir_options(void);
uint32_t sgx545_fs_program(struct sgx545_context *ctx, struct sgx545_shader *so, const struct sgxws_blend *b);

/* the compiled vertex shader vs as it goes with the compiled fragment shader
 * fs (or NULL) and inputs in vtx_fmt (or NULL, f32 x 4): itself when their
 * varyings' sets agree and the inputs are f32 x 4, else a variant writing
 * fs's sets (gl_PointCoord's set 0 included) and unpacking vtx_fmt; NULL if
 * it can't */
struct sgx545_shader *sgx545_vs_for(struct sgx545_context *ctx, struct sgx545_shader *vs,
                                    const struct sgx545_shader *fs, const uint8_t *vtx_fmt);

/* the bound fragment shader as the draw samples its textures: itself, or a
 * variant decoding the sRGB views it samples; NULL if it can't */
struct sgx545_shader *sgx545_fs_for(struct sgx545_context *ctx, struct sgx545_shader *fs);

#endif
