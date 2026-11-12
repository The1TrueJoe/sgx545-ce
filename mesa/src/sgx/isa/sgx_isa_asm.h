/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

#ifndef SGX_ISA_ASM_H
#define SGX_ISA_ASM_H

#include <stdbool.h>
#include <stddef.h>

#include "sgx_isa.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parse one line of the USSE text notation (README.md) into *instr. Returns
 * false with a message in err on a parse error, or for a flag the form has no
 * field for (it would be dropped from the word). A ';' starts a comment.
 */
bool sgx_isa_asm_line(const char *line, struct sgx_isa_instr *instr, char *err,
                      size_t errsize);

/* The integer / fixed-point / dual-issue part (sgx_isa_asm_int.c). */
bool sgx_isa_asm_mnemonic_int(struct sgx_isa_instr *instr, char **parts,
                              unsigned nparts, unsigned nops,
                              const char **pattern);
bool sgx_isa_asm_item_int(struct sgx_isa_instr *instr, const char *name,
                          const char *value);
bool sgx_isa_asm_pred2_int(enum sgx_isa_opc opc);

#ifdef __cplusplus
}
#endif

#endif
