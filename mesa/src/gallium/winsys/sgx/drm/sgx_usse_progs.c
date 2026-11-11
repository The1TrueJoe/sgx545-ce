/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * The fixed USSE programs a frame needs on top of the shaders (docs/GEN.md):
 * the TA state-emit programs (one per state-block length), the empty task,
 * the clear's and the scissor objects' vertex programs, the background
 * object's pixel program, the pixel-event tasks and the built-in shaders.
 * They are source text in docs/ISA.md's notation, assembled once per process
 * by the instruction set's assembler (src/sgx/isa).
 *
 * `name:` starts a label; `@name` as the first operand of `phas.i` is that
 * label's instruction index from the program start.  The caller adds the
 * program's offset in its code heap (in instructions) to those words, listed
 * in reloc (phas.i's pc field is l0-17, absolute; ISA.md).
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/macros.h"
#include "util/u_call_once.h"

#include "sgx_isa.h"
#include "sgx_isa_asm.h"
#include "sgx_usse_progs.h"

/* The prologue every vertex and state task starts with: test.and.ne of #0
 * and #0 never sets p0, so the predicated load never runs (the vendor
 * driver's tasks all have it). */
#define PROLOGUE                                                               \
   "nop [nosched]", "test.and.ne p0, #0, #0 [skipinv nodst=0]",                \
      "(p0) ld.a.d pa0, pa0, #0 [skipinv rpt=4]", "wdf"

/* The last phase of every vertex program: the phase before it ends with
 * emit.t3 (the vertex's outputs); this one sends emit.t1 with f3_20, as the
 * state-emit programs end.  A vertex program's earlier phase points here with
 * phas.i; without it the task runs on into whatever follows. */
#define VS_EPILOGUE                                                            \
   "phas.i #0, #0 [wait=7]", "emit.t1 r0, #0, #0 [end f12 f10 f3_20]"

/* a task that does nothing (empty secondary updates, vertex and pixel) */
static const char *const src_empty[] = {"phas.i #0, #0 [end wait=7]", NULL};

/* clear: screen-space x, y, z and an ARGB8888 colour per vertex (16 bytes)
 * -> position (w = c3 = 1.0) and the colour as four floats 0..1 */
static const char *const src_clear_vs[] = {
   "phas.i @end, #0 [wait=2]",
   PROLOGUE,
   "mov o0, pa0 [skipinv rcnt rpt=3]",
   "mov o3, c3 [skipinv rcnt]",
   "pck.f32.u8 o4, pa3.l2, pa3.l2 [skipinv rcnt scale]",
   "pck.f32.u8 o5, pa3.l1, pa3.l1 [skipinv rcnt scale]",
   "pck.f32.u8 o6, pa3, pa3 [skipinv rcnt scale]",
   "pck.f32.u8 o7, pa3.l3, pa3.l3 [skipinv rcnt scale]",
   "emit.t3 r0, #0, #0 [end f12 f10]",
   "end:",
   VS_EPILOGUE,
   NULL,
};

/* pass-type-3 objects (STATE.md "Scissor and partial viewports"):
 * screen-space x, y -> position with z = w = 1.0 */
static const char *const src_vpgen_vs[] = {
   "phas.i @end, #0 [wait=2]",
   PROLOGUE,
   "mov o0, pa0 [skipinv rcnt rpt=2]",
   "mov o2, c3 [skipinv rcnt]",
   "mov o3, c3 [skipinv rcnt]",
   "emit.t3 r0, #0, #0 [end f12 f10]",
   "end:",
   VS_EPILOGUE,
   NULL,
};

/* the iterated colour (PDS DOUTI colour x4) is already the packed pixel */
static const char *const src_copy_fs[] = {
   "phas.i #0, #0 [wait=7]",
   "mov o0, pa0 [skipinv end rcnt]",
   NULL,
};

/* gl_Position = vec4(a0.xy, 0, 1): phase 0 fills pa2, pa3, phase 1 copies
 * pa0-pa3 out */
static const char *const src_vs_xy[] = {
   "phas.i @p1, #0",
   PROLOGUE,
   "limm pa3, #0x3f800000 [skipinv]",
   "limm pa2, #0 [skipinv end]",
   "p1:",
   "phas.i @p2, #0 [wait=2]",
   "mov o0, pa0 [skipinv rcnt rpt=4]",
   "emit.t3 r0, #0, #0 [end f12 f10]",
   "p2:",
   VS_EPILOGUE,
   NULL,
};

/* gl_Position = vec4(a0.xyz, 1) */
static const char *const src_vs_xyz[] = {
   "phas.i @p1, #0",
   PROLOGUE,
   "limm pa3, #0x3f800000 [skipinv end]",
   "p1:",
   "phas.i @p2, #0 [wait=2]",
   "mov o0, pa0 [skipinv rcnt rpt=4]",
   "emit.t3 r0, #0, #0 [end f12 f10]",
   "p2:",
   VS_EPILOGUE,
   NULL,
};

/* gl_FragColor = c (mediump uniform vec4): the secondary program packs the
 * FP16 uniform (DMA'd to pa9, pa10) to BGRA8888 in pa9, which the main
 * program reads as sa9 */
static const char *const src_fs_uniform_sec[] = {
   "phas.i #0, #0 [wait=7]",
   "nop [nosched]",
   "test.and.ne p0, #0, #0 [skipinv]",
   "(p0) ld.a.d pa0, pa0, #0 [skipinv moe]",
   "wdf",
   "pck.u8.f16 pa9, pa9.l2, pa9 [skipinv dmask=6 scale]",
   "pck.u8.f16 pa9, pa10, pa10.l2 [skipinv end dmask=9 scale]",
   NULL,
};
static const char *const src_fs_uniform[] = {
   "phas.i #0, #0 [wait=7]",
   "mov o0, sa9 [skipinv end mask=1]",
   NULL,
};

/* pixel event tasks (the pixel-event PDS program picks one per event) */
static const char *const src_pe_ptoff[] = {
   "phas.i #0, #0 [wait=7]",
   "ptoff [end]",
   NULL,
};
static const char *const src_pe_emit[] = {
   "phas.i #0, #0 [wait=7]",
   "limm r0, #0 [skipinv]",
   "emit.t0 r0, #0, r0 [end f3_20 imm=512]",
   NULL,
};

struct usse_prog usse_empty, usse_clear_vs, usse_vpgen_vs, usse_copy_fs,
   usse_vs_xy, usse_vs_xyz, usse_fs_uniform_sec, usse_fs_uniform,
   usse_pe_ptoff, usse_pe_emit;
static struct usse_prog state_emit[33];
const struct usse_prog *usse_state_emit[33];

static bool progs_ok;

#define MAX_LINES 48

/* one program: labels resolved, every line assembled */
static bool
assemble(struct usse_prog *p, const char *name, const char *const *src)
{
   const char *insts[MAX_LINES];
   unsigned label_at[MAX_LINES], n = 0, n_labels = 0;
   const char *label[MAX_LINES];
   uint32_t *w;

   for (unsigned i = 0; src[i]; i++) {
      size_t len = strlen(src[i]);

      if (len && src[i][len - 1] == ':') {
         label[n_labels] = src[i];
         label_at[n_labels++] = n;
      } else {
         assert(n < MAX_LINES);
         insts[n++] = src[i];
      }
   }
   w = malloc(n * 8);
   if (!w)
      return false;
   p->name = name;
   p->nreloc = 0;
   for (unsigned i = 0; i < n; i++) {
      const char *at = strchr(insts[i], '@');
      char line[160], err[128];
      struct sgx_isa_instr instr;
      uint64_t word;

      if (at) {
         /* @label -> #index */
         size_t l = strcspn(at + 1, ", ");
         unsigned k;

         for (k = 0; k < n_labels; k++) {
            if (strlen(label[k]) == l + 1 && !strncmp(label[k], at + 1, l))
               break;
         }
         if (k == n_labels) {
            fprintf(stderr, "sgx: %s: no label %.*s\n", name, (int)l, at + 1);
            return false;
         }
         snprintf(line, sizeof(line), "%.*s#%u%s", (int)(at - insts[i]),
                  insts[i], label_at[k], at + 1 + l);
         assert(p->nreloc < ARRAY_SIZE(p->reloc));
         p->reloc[p->nreloc++] = i;
      } else {
         snprintf(line, sizeof(line), "%s", insts[i]);
      }
      memset(&instr, 0, sizeof(instr));
      if (!sgx_isa_asm_line(line, &instr, err, sizeof(err))) {
         fprintf(stderr, "sgx: %s: %s: %s\n", name, line, err);
         return false;
      }
      word = sgx_isa_encode(&instr, NULL);
      w[2 * i] = (uint32_t)word;
      w[2 * i + 1] = (uint32_t)(word >> 32);
   }
   p->w = w;
   p->n = n;
   return true;
}

/* copy n DMA'd state dwords (pa0..) to the outputs and emit them to the MTE;
 * mov repeats at most 16 times, so longer blocks take several movs */
static bool
assemble_state_emit(struct usse_prog *p, unsigned n)
{
   static const char *const head[] = {"phas.i #0, #0 [wait=7]", PROLOGUE};
   static const char *const tail[] = {"emit.t3 r0, #0, #0", "op0xd6 #0",
                                      "emit.t1 o0, #0, #0 [end f3_20]"};
   char movs[2][64];
   const char *src[ARRAY_SIZE(head) + 2 + ARRAY_SIZE(tail) + 1];
   unsigned k = 0, m = 0;

   for (unsigned i = 0; i < ARRAY_SIZE(head); i++)
      src[k++] = head[i];
   for (unsigned r = 0; r < n; r += 16, m++) {
      snprintf(movs[m], sizeof(movs[m]), "mov o%u, pa%u [skipinv rcnt rpt=%u]",
               r, r, MIN2(16, n - r));
      src[k++] = movs[m];
   }
   for (unsigned i = 0; i < ARRAY_SIZE(tail); i++)
      src[k++] = tail[i];
   src[k] = NULL;
   return assemble(p, "state_emit", src);
}

static void
progs_init(void)
{
   bool ok = assemble(&usse_empty, "empty", src_empty) &&
             assemble(&usse_clear_vs, "clear_vs", src_clear_vs) &&
             assemble(&usse_vpgen_vs, "vpgen_vs", src_vpgen_vs) &&
             assemble(&usse_copy_fs, "copy_fs", src_copy_fs) &&
             assemble(&usse_vs_xy, "vs_xy", src_vs_xy) &&
             assemble(&usse_vs_xyz, "vs_xyz", src_vs_xyz) &&
             assemble(&usse_fs_uniform_sec, "fs_uniform_sec",
                      src_fs_uniform_sec) &&
             assemble(&usse_fs_uniform, "fs_uniform", src_fs_uniform) &&
             assemble(&usse_pe_ptoff, "pe_ptoff", src_pe_ptoff) &&
             assemble(&usse_pe_emit, "pe_emit", src_pe_emit);

   for (unsigned n = 1; ok && n <= 32; n++) {
      ok = assemble_state_emit(&state_emit[n], n);
      usse_state_emit[n] = &state_emit[n];
   }
   progs_ok = ok;
}

bool
sgx_usse_progs_init(void)
{
   static util_once_flag once = UTIL_ONCE_FLAG_INIT;

   util_call_once(&once, progs_init);
   return progs_ok;
}
