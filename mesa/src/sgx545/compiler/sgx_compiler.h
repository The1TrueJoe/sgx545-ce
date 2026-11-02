/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * SGX545 shader compiler: NIR in, USSE code out, plus what the driver needs
 * to run it (constants, iterations, texture samples, branches). The DDK's
 * USP container is not written here: tools/usc/sgx_usp.c builds it from
 * struct sgx_shader for the A/B harness (docs/COMPILER-UPSTREAM.md §3).
 *
 * The input/output conventions are the ones the DDK's driver expects from
 * libusc (ISA.md "What a compiler needs"):
 *  - uniforms: one 32-bit constant per component, constant k of the driver's
 *    constant buffer loaded into sa[base + k] (fragment base 9, vertex 10);
 *  - fragment varyings: iterated by the PDS into pa, F32, one register per
 *    component, in the order of USP list A; a varying's iterator is its
 *    texture coordinate set + 1;
 *  - fragment colour: packed BGRA8 in a pa register (USP header 0x66-0x6c);
 *  - vertex attributes: attribute slot k at pa[4k..4k+3];
 *  - vertex outputs: position o0-o3, then the texture coordinate sets in
 *    order, each its size, then the point size.
 * Constant numbers, attribute slots and sampler units are the caller's to
 * choose (sgx_compile_options.layout); sgxc's choice matches the DDK's GLSL
 * compiler, for the A/B harness.
 */
#ifndef SGX_COMPILER_H
#define SGX_COMPILER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nir_shader;
struct nir_shader_compiler_options;

#define SGX_MAX_SETS 10

enum sgx_layout_kind {
   SGX_LAYOUT_UNIFORM,          /* -> first constant number (rows/elements at +4 each) */
   SGX_LAYOUT_ATTRIB,           /* -> attribute slot (pa 4*slot) */
   SGX_LAYOUT_SAMPLER,          /* -> texture unit */
};

/* Where the caller wants things. place() is asked for each uniform
 * variable, struct member ("s.f") and constant array element ("a[2]"), each
 * vertex input and each sampler, and returns a number or -1 for the
 * compiler's own: uniforms vec4-aligned in declaration order, attributes in
 * declaration order, samplers in order of first use, each from first_free()
 * on. */
struct sgx_layout {
   int (*place)(void *data, enum sgx_layout_kind kind, const char *name);
   unsigned (*first_free)(void *data, enum sgx_layout_kind kind);
   void *data;
};

struct sgx_compile_options {
   const struct sgx_layout *layout;     /* NULL: the compiler's own numbering */
   bool verbose;
   /* mediump/lowp arithmetic in F16 (NIR from Mesa's precision lowering):
    * F16 iteration, fmad16, F16/C10 uniforms, as the DDK does */
   bool f16;
   /* gl_FrontFacing: by default 1.0 (front). libusc reads p0 at entry and
    * flips it with a per-draw driver constant, but in our programs p0 at
    * entry is not reliable (EA3: ~11% of pixels differ run to run), so no
    * per-primitive facing source is known yet. front_p0 = read p0 (set =
    * front), for experiments. */
   bool front_p0;
   /* the driver loads constant formats the DDK's driver has no list C kind
    * for (sgx_const_map fmt 0x10 / 0x11): a lowp vector's xyz pre-converted
    * to F16 and literal constants, where libusc converts once per draw in its
    * secondary program and keeps literals with the uniforms */
   bool driver_consts;
};

/* varying -> texture coordinate set (sgx_assign_varyings) */
struct sgx_varying {
   char name[64];
   unsigned set;        /* first set (one per row) */
   unsigned rows, comps;
   unsigned precision;  /* GLSL: 0 none, 1 high, 2 medium, 3 low */
};

struct sgx_varyings {
   unsigned n;
   struct sgx_varying v[32];
   bool point_size;     /* set 0 reserved (vertex shaders that write gl_PointSize) */
   unsigned set_comps[SGX_MAX_SETS];
};

/* where the driver puts what the program reads (the USP's lists, decoded) */
struct sgx_const_map {
   uint16_t k;                  /* constant number: uniform component (sgx_nir.c's numbering) */
   uint16_t slot;               /* sa[sa_base + slot] */
   uint8_t shift;               /* bit offset in the slot: F16 0/16, C10 0/10/20 */
   uint8_t fmt;                 /* 1 f32, 2 F16 (round to nearest), 3 C10 (round(v * 255), signed 10 bits);
                                 * with driver_consts: 0x10 F16 of the C10 value c (fmt 3's rule):
                                 * c / 255 rounded to nearest F16, as pck.f16.c10 [scale] gives it
                                 * (hardware, tests/c10); 0x11 a literal: k = 0xffff, value (32 bits)
                                 * ORed into the slot at shift */
   uint32_t value;              /* fmt 0x11 */
};

struct sgx_iter_info {
   uint8_t set;                 /* texture coordinate set (DOUTI source = set + 1) */
   uint8_t comps;
   uint8_t pa;                  /* first PA register */
   uint8_t regs;                /* PA registers it fills */
   uint8_t f16;                 /* iterated as F16 (two components per register) */
   uint8_t sample;              /* a non-dependent sample: the raw RGBA8 texel lands in pa */
   uint8_t proj;                /* ... with a projected coordinate (x/w, y/w) */
   uint8_t sampler;             /* ... of this sampler */
   uint8_t pointcoord;          /* gl_PointCoord (set 0) */
};

/* A texture sample (docs/ISA.md "The texture-sample sequence"). A
 * non-dependent one is the PDS's: the raw RGBA8 texel lands in pa `reg`. A
 * dependent one is an smp the driver puts in before code[at], with the
 * sampler's state words as its src1, writing the raw texel to temporary
 * `reg`; the program unpacks it from there. */
struct sgx_sample_info {
   unsigned at;                 /* dependent: index in code[] it goes before */
   uint16_t id;
   uint8_t sampler;
   uint8_t set;                 /* non-dependent: the coordinate's set */
   uint8_t dim;                 /* 2 = 2D, 3 = cube */
   uint8_t dependent, proj;
   uint8_t reg;                 /* the raw texel: pa (non-dependent), r (dependent) */
   int16_t coord_pa;            /* non-dependent: the coordinate's plain iteration, -1 none */
   uint64_t smp;                /* dependent: the smp, destination r0, texture state #0 */
};

/* The program's structure, in order: runs of code, labels, branches, the
 * discard feedback point and texture samples. A driver needs only the
 * branches (to move their offsets when it inserts code) and the samples;
 * the USP writer needs all of it. */
enum sgx_block_kind {
   SGX_BLOCK_CODE,              /* code[at .. at + n) */
   SGX_BLOCK_LABEL,             /* label */
   SGX_BLOCK_BRANCH,            /* code[at] branches to label (offset resolved) */
   SGX_BLOCK_FEEDBACK,          /* code[at] is the discard feedback nop */
   SGX_BLOCK_SAMPLE,            /* samples[at] */
};

struct sgx_block {
   uint8_t kind;
   uint16_t label;
   unsigned at, n;
};

#define SGX_MAX_SAMPLES 16

struct sgx_shader {
   unsigned stage;              /* MESA_SHADER_VERTEX / _FRAGMENT */
   /* what the program reads and writes */
   unsigned sa_base;            /* first SA register of the constants (9 fragment, 10 vertex) */
   unsigned n_consts;
   struct sgx_const_map consts[256];
   unsigned n_iters;            /* fragment: PDS iterations, in PA order */
   struct sgx_iter_info iters[16];
   unsigned colour_pa;          /* fragment: packed BGRA8 colour register */
   bool discards;               /* fragment: p1 = keep at the feedback nop (USP labels 4/5) */
   uint32_t in_mask[4];         /* vertex: PA registers read (attribute slot k = pa 4k..4k+3) */
   unsigned set_comps[SGX_MAX_SETS]; /* vertex: output set sizes (o4 on); 0 = unused */
   bool point_size;             /* vertex: writes the point size, after the sets */
   unsigned n_out_regs;         /* vertex: o registers written */
   unsigned sa_slots;           /* SA registers used from sa_base on (constants and the compiler's own) */
   uint64_t *code;              /* main program, flat (branch targets resolved) */
   unsigned n_code;
   unsigned n_samples;
   struct sgx_sample_info samples[SGX_MAX_SAMPLES];
   struct sgx_block *blocks;
   unsigned n_blocks;
   unsigned n_alu, n_tex, n_flow; /* instruction counts */
   unsigned n_temps;
   unsigned n_pa;               /* PA registers: inputs, colour, and the placeholder
                                 * colour iteration of a fragment program with no
                                 * other (the DDK's driver iterates one) */
   char *log;                   /* listing / errors (malloc'd) */
};

const struct nir_shader_compiler_options *sgx_nir_options(void);

/* GLSL ES varyings to texture coordinate sets, as the DDK's glslcompiler
 * allocates them (ISA.md "Varyings to sets"); call on each stage's NIR
 * before linking removes unused ones */
void sgx_assign_varyings(struct nir_shader *nir, bool vertex, struct sgx_varyings *out);

/* lower and compile; 0 on success */
int sgx_compile(struct nir_shader *nir, const struct sgx_compile_options *opts,
                const struct sgx_varyings *vary, struct sgx_shader *out);

void sgx_shader_free(struct sgx_shader *s);

#ifdef __cplusplus
}
#endif

#endif
