/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * The fixed USSE programs a frame needs besides the shaders (docs/GEN.md):
 * the TA state-emit programs, the empty task, the clear's and the viewport
 * object's vertex programs, the copy and uniform-colour pixel programs, the
 * position-only vertex programs and the pixel-event tasks. They are assembly
 * text (ISA.md "Text notation and defaults"), assembled through the isa
 * library when a context is created.
 */
#ifndef SGX_FIXED_H
#define SGX_FIXED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SGX_FIXED_MAX_INSTS 16

struct sgx_fixed_prog {
   const char *name;
   uint32_t w[2 * SGX_FIXED_MAX_INSTS]; /* low word first */
   unsigned n;                          /* instructions */
   /* the instructions whose phas.i target is relative to the program: the
    * caller adds the program's offset in its code heap (in instructions) */
   uint8_t reloc[4];
   unsigned nreloc;
};

struct sgx_fixed_progs {
   struct sgx_fixed_prog empty, clear_vs, vpgen_vs, copy_fs, vs_xy, vs_xyz,
      fs_uniform_sec, fs_uniform, pe_ptoff, pe_emit;
   struct sgx_fixed_prog state_emit[33]; /* [n]: emit n state dwords, 1-32 */
};

/* Assemble every program into p. False, with a message in err, if a line
 * doesn't assemble. */
bool sgx_fixed_progs_build(struct sgx_fixed_progs *p, char *err, size_t size);

#ifdef __cplusplus
}
#endif

#endif
