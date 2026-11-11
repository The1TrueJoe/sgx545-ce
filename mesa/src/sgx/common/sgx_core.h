/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * What a core is: one entry per SGX core, found by the kernel's core id and
 * revision.  Everything the driver, the winsys and the compiler need to know
 * about a core and the kernel doesn't report lives here, so adding a core is
 * adding an entry (docs/porting/mesa.md).  No code tests a core's id.
 *
 * The state words (csbgen) and the instruction sets (isaspec, PDS) are per
 * family, not here: a core of another family needs its own descriptions.
 *
 * Each field says how its value is known: [HW] measured on hardware with the
 * open stack, [D] from the vendor driver's behaviour (captures, its limits),
 * [G] a guess that works.
 */
#ifndef SGX_CORE_H
#define SGX_CORE_H

#include <stdbool.h>
#include <stdint.h>

/* the USSE generation: its instruction set and register files */
enum sgx_usse {
   SGX_USSE1 = 1, /* SGX520-545 */
   SGX_USSE2 = 2, /* SGX543, 544, 554 */
};

struct sgx_core {
   const char *name; /* "SGX545" */
   uint16_t id;      /* EUR_CR_CORE_ID bits 16-31 */
   /* revisions (EUR_CR_CORE_REVISION, major << 16 | minor << 8 | maint) */
   uint32_t rev_min, rev_max;
   enum sgx_usse usse;
   /* where it has run: the machines whose dEQP-GLES2 mustpass and desktop
    * sessions accepted this entry (docs/porting/mesa.md) */
   const char *verified_on;

   /* ---- limits ---- */
   unsigned max_texture_size; /* texels a side, 2-D and cube [D] */
   unsigned max_cube_levels;  /* [D] */
   unsigned samplers;         /* texture units [D] */
   unsigned vs_inputs;        /* vertex attribute vec4 slots [HW] */
   unsigned vs_outputs;       /* vec4 outputs: position + coordinate sets */
   unsigned fs_inputs;        /* vec4 varyings */
   unsigned gl_max_temps;     /* GL's temporary limit (not the register file) */
   unsigned temps;            /* USSE temporaries r0.. (compiler) [HW] */
   unsigned coord_sets;       /* TSP texture coordinate sets of 4 [HW] */
   /* render targets, where the kernel doesn't report them (services) */
   unsigned rt_max_width, rt_max_height, rt_max_samples; /* [D] */
   unsigned line_width_max; /* ISPB: width * 16 in 8 bits [D] */
   unsigned point_size_max; /* [D] */

   /* ---- vertex work ---- */
   unsigned vertex_task_dwords; /* vertices per USSE task: this / output
                                   dwords... [HW: half of it hangs] */
   unsigned vertex_task_max;    /* ...and at most this many [D] */
   unsigned dma_chunk_dwords;   /* a PDS DMA moves at most this [HW] */

   /* ---- features ---- */
   bool f16;              /* USSE F16 and C10 arithmetic [HW] */
   bool f16_sample_return; /* the TAG returns F16 texels (T3 01028800) [HW] */

   /* ---- what hangs it ---- */
   bool sample_texture_heap_only; /* sampling outside the texture heap hangs
                                     (the sample cache erratum) [D] */
   bool npot_mip_hangs;           /* a mipmapped NPOT texture locks the 3D [HW] */
};

/* the core with this id and revision, or NULL: not driven */
const struct sgx_core *sgx_core_lookup(uint32_t core_id, uint32_t core_revision);

#endif
