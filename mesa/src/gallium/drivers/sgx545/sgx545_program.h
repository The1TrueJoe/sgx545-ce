/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

#ifndef SGX545_PROGRAM_H
#define SGX545_PROGRAM_H

struct nir_shader_compiler_options;

const struct nir_shader_compiler_options *sgx545_get_nir_options(void);

#endif
