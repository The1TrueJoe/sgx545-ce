/*
 * Copyright © 2026 The sgx545-ce authors
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

#endif
