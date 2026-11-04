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
 * fs: itself when their varyings' sets agree, else a variant writing fs's
 * sets (gl_PointCoord's set 0 included); NULL if it can't */
struct sgx545_shader *sgx545_vs_for(struct sgx545_context *ctx, struct sgx545_shader *vs,
                                    const struct sgx545_shader *fs);

#endif
