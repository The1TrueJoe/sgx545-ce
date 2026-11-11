/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "sgx_fixed.h"
#include "sgx_isa.h"
#include "sgx_isa_asm.h"

/* The prologue of every vertex and state task (GEN.md). test.and.ne of #0
 * and #0 never sets p0, so the predicated load never runs. */
#define PROLOGUE                                                               \
   "nop [nosched]", "test.and.ne p0, #0, #0 [skipinv nodst=0]",                \
      "(p0) ld.a.d pa0, pa0, #0 [skipinv rpt=4]", "wdf"

/* A vertex program's last phase: the earlier phase ends with emit.t3 (the
 * vertex's outputs) and points here with phas.i; this one sends emit.t1. */
#define VS_EPILOGUE                                                            \
   "phas.i #0, #0 [wait=7]", "emit.t1 r0, #0, #0 [end f12 f10 f3_20]"

/* `name:` is a label; `@name` in phas.i is its instruction index */
static const char *const empty[] = {"phas.i #0, #0 [end wait=7]", NULL};

/* screen-space x, y, z and an ARGB8888 colour per vertex -> position (w =
 * c3 = 1.0) and the colour as four floats */
static const char *const clear_vs[] = {
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
static const char *const vpgen_vs[] = {
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

/* the iterated colour is already the packed pixel */
static const char *const copy_fs[] = {
   "phas.i #0, #0 [wait=7]",
   "mov o0, pa0 [skipinv end rcnt]",
   NULL,
};

/* gl_Position = vec4(a0.xy, 0, 1): phase 0 fills pa2/pa3, phase 1 copies
 * pa0-pa3 out */
static const char *const vs_xy[] = {
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
static const char *const vs_xyz[] = {
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
 * F16 uniform (DMA'd to pa9, pa10) to BGRA8888 in pa9, which the main
 * program reads as sa9 */
static const char *const fs_uniform_sec[] = {
   "phas.i #0, #0 [wait=7]",
   "nop [nosched]",
   "test.and.ne p0, #0, #0 [skipinv]",
   "(p0) ld.a.d pa0, pa0, #0 [skipinv moe]",
   "wdf",
   "pck.u8.f16 pa9, pa9.l2, pa9 [skipinv dmask=6 scale]",
   "pck.u8.f16 pa9, pa10, pa10.l2 [skipinv end dmask=9 scale]",
   NULL,
};

static const char *const fs_uniform[] = {
   "phas.i #0, #0 [wait=7]",
   "mov o0, sa9 [skipinv end mask=1]",
   NULL,
};

/* pixel-event tasks (the pixel-event PDS program picks one per event) */
static const char *const pe_ptoff[] = {
   "phas.i #0, #0 [wait=7]",
   "ptoff [end]",
   NULL,
};

static const char *const pe_emit[] = {
   "phas.i #0, #0 [wait=7]",
   "limm r0, #0 [skipinv]",
   "emit.t0 r0, #0, r0 [end f3_20 imm=512]",
   NULL,
};

static bool
assemble(struct sgx_fixed_prog *p, const char *name,
         const char *const *lines, char *err, size_t size)
{
   const char *label[8];
   unsigned at[8], nlabels = 0, n = 0;

   memset(p, 0, sizeof(*p));
   p->name = name;
   for (const char *const *l = lines; *l; l++) {
      size_t len = strlen(*l);
      if (len && (*l)[len - 1] == ':') {
         if (nlabels == 8)
            goto fail_size;
         label[nlabels] = *l;
         at[nlabels++] = n;
      } else {
         n++;
      }
   }
   if (n > SGX_FIXED_MAX_INSTS)
      goto fail_size;

   for (const char *const *l = lines; *l; l++) {
      size_t len = strlen(*l);
      if (len && (*l)[len - 1] == ':')
         continue;
      char text[160], msg[128];
      const char *a = strchr(*l, '@');
      if (a) {
         size_t k = strcspn(a + 1, ", ");
         unsigned i;
         for (i = 0; i < nlabels; i++) {
            if (strlen(label[i]) == k + 1 && !strncmp(label[i], a + 1, k))
               break;
         }
         if (i == nlabels || p->nreloc == sizeof(p->reloc)) {
            snprintf(err, size, "%s: %s: no such label", name, *l);
            return false;
         }
         snprintf(text, sizeof(text), "%.*s#%u%s", (int)(a - *l), *l, at[i],
                  a + 1 + k);
         p->reloc[p->nreloc++] = p->n;
      } else {
         snprintf(text, sizeof(text), "%s", *l);
      }
      struct sgx_isa_instr I;
      if (!sgx_isa_asm_line(text, &I, msg, sizeof(msg))) {
         snprintf(err, size, "%s: %s: %s", name, text, msg);
         return false;
      }
      uint64_t w = sgx_isa_encode(&I, NULL);
      p->w[2 * p->n] = (uint32_t)w;
      p->w[2 * p->n + 1] = (uint32_t)(w >> 32);
      p->n++;
   }
   return true;

fail_size:
   snprintf(err, size, "%s: too long", name);
   return false;
}

/* copy n state dwords (DMA'd to pa0..) to the outputs and emit them to the
 * MTE; mov repeats at most 16 times, so longer blocks take two */
static bool
state_emit(struct sgx_fixed_prog *p, unsigned n, char *err, size_t size)
{
   char movs[2][64];
   const char *lines[SGX_FIXED_MAX_INSTS + 1];
   unsigned k = 0;
   const char *const pro[] = {"phas.i #0, #0 [wait=7]", PROLOGUE};

   for (unsigned i = 0; i < 5; i++)
      lines[k++] = pro[i];
   for (unsigned r = 0, m = 0; r < n; r += 16, m++) {
      snprintf(movs[m], sizeof(movs[m]), "mov o%u, pa%u [skipinv rcnt rpt=%u]",
               r, r, n - r < 16 ? n - r : 16);
      lines[k++] = movs[m];
   }
   lines[k++] = "emit.t3 r0, #0, #0";
   lines[k++] = "op0xd6 #0";
   lines[k++] = "emit.t1 o0, #0, #0 [end f3_20]";
   lines[k] = NULL;
   return assemble(p, "state_emit", lines, err, size);
}

bool
sgx_fixed_progs_build(struct sgx_fixed_progs *p, char *err, size_t size)
{
   memset(p, 0, sizeof(*p));
   if (!assemble(&p->empty, "empty", empty, err, size) ||
       !assemble(&p->clear_vs, "clear_vs", clear_vs, err, size) ||
       !assemble(&p->vpgen_vs, "vpgen_vs", vpgen_vs, err, size) ||
       !assemble(&p->copy_fs, "copy_fs", copy_fs, err, size) ||
       !assemble(&p->vs_xy, "vs_xy", vs_xy, err, size) ||
       !assemble(&p->vs_xyz, "vs_xyz", vs_xyz, err, size) ||
       !assemble(&p->fs_uniform_sec, "fs_uniform_sec", fs_uniform_sec, err,
                 size) ||
       !assemble(&p->fs_uniform, "fs_uniform", fs_uniform, err, size) ||
       !assemble(&p->pe_ptoff, "pe_ptoff", pe_ptoff, err, size) ||
       !assemble(&p->pe_emit, "pe_emit", pe_emit, err, size))
      return false;
   for (unsigned n = 1; n <= 32; n++) {
      if (!state_emit(&p->state_emit[n], n, err, size))
         return false;
   }
   return true;
}
