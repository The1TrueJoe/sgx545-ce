/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

#ifndef SGX_USSE_PROGS_H
#define SGX_USSE_PROGS_H

#include <stdbool.h>
#include <stdint.h>

/* a fixed program (sgx_usse_progs.c): n instructions of two words, low first;
 * reloc: the instructions whose pc field takes the program's offset */
struct usse_prog {
   const char *name;
   const uint32_t *w;
   unsigned n;
   uint8_t reloc[4];
   unsigned nreloc;
};

extern struct usse_prog usse_empty, usse_clear_vs, usse_vpgen_vs, usse_copy_fs,
   usse_vs_xy, usse_vs_xyz, usse_fs_uniform_sec, usse_fs_uniform, usse_pe_ptoff,
   usse_pe_emit;
/* state-emit programs by state-block length, 1 to 32 */
extern const struct usse_prog *usse_state_emit[33];

/* assembles them, once per process; false: they couldn't be */
bool sgx_usse_progs_init(void);

#endif
