/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/* The DDK 1.7 USP container for a compiled program (A/B harness only). */
#ifndef SGX_USP_H
#define SGX_USP_H

#include "sgx_compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 0 on success; *usp is malloc'd */
int sgx_write_usp(const struct sgx_shader *s, uint8_t **usp, size_t *size);

#ifdef __cplusplus
}
#endif

#endif
