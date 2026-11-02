/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/* Text notation of the integer / fixed-point / dual-issue classes. */

#include <string.h>

#include "sgx545_isa_asm.h"

bool
sgx545_isa_asm_mnemonic_int(struct sgx_isa_instr *I, char **parts,
                            unsigned nparts, unsigned nops, const char **pat)
{
   return false;
}

bool
sgx545_isa_asm_item_int(struct sgx_isa_instr *I, const char *name,
                        const char *value)
{
   return false;
}

bool
sgx545_isa_asm_pred2_int(enum sgx_isa_opc opc)
{
   return false;
}
