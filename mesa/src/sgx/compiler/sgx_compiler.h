/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * SGX545 shader compiler: NIR in, USSE code out, plus what the driver needs
 * to run it (constants, iterations, texture samples, branches). No
 * container format is written here: the caller packages struct sgx_shader.
 *
 * The input/output conventions:
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
 * choose (sgx_compile_options.layout).
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
   SGX_LAYOUT_UNIFORM, /* -> first constant number (rows/elements at +4 each) */
   SGX_LAYOUT_ATTRIB,  /* -> attribute slot (pa 4*slot) */
   SGX_LAYOUT_SAMPLER, /* -> texture unit */
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
   const struct sgx_layout *layout; /* NULL: the compiler's own numbering */
   bool verbose;
   /* mediump/lowp arithmetic in F16 (NIR from Mesa's precision lowering):
    * F16 iteration, fmad16, F16/C10 uniforms */
   bool f16;
   /* gl_FrontFacing = ((g16 ^ sa[front_sa]) & 1) != 0: g16 bit 0 is the
    * primitive's winding in memory, and the
    * driver loads this SA register with what makes that winding front.
    * 0: constant true */
   unsigned front_sa;
   /* the driver loads constant formats the USP has no list C kind for
    * (sgx_const_map fmt 0x10 / 0x11): a lowp vector's xyz pre-converted
    * to F16 and literal constants, loaded rather than computed per draw */
   bool driver_consts;
   /* (with f16) every float of a fragment program is mediump, texture
    * coordinates excepted: for programs without precision qualifiers whose
    * results only need the framebuffer's precision (st's fixed-function
    * fragment programs); off by default */
   bool fs_mediump;
   /* vertex shader: what the PDS leaves in PA[4 * driver_location] for each
    * attribute (enum sgx_vtx_fmt); 0, f32 x 4, is the default */
   uint8_t vtx_fmt[16];
   /* fragment shader: bit n set, texture unit n's texels arrive with red and
    * blue swapped (an RGBA8 view sampled as T1 0x0d):
    * byte 0 is blue, 2 red, the colour output's order. A texel that is the
    * colour as it is then needs no instruction; other reads unpack zyxw */
   uint32_t tex_bgra;
   /* fragment shader: bit n set, texture unit n's texel is one F32 value in
    * the first register (a depth texture), read as (d, 0, 0, 1) */
   uint32_t tex_f32;
   /* fragment shader: the driver's PDS samples a non-dependent texture from
    * the coordinate set itself, so a varying only such samples read needs no
    * plain iteration of its own (one PDS sample, no iteration).
    * Off when other code may read that iteration */
   bool pds_samples_alone;
   /* fragment shader: the driver can have a non-dependent sample return F16
    * (the T3 word's F16-return bits):
    * a texel read at mediump then arrives as F16, two channels per register
    * (sgx_sample_info.f16), and is not unpacked */
   bool f16_samples;
   /* fragment shader with a discard: split it at the visibility feedback.
    * Phase 0 (code)
    * runs for every rasterized fragment and computes only the discard test,
    * ending in the feedback nop; phase 1 (code1) runs for visible pixels
    * only and computes the colour from the inputs again. Only PA carries
    * over: both phases share one iteration list and one constant map.
    * Straight-line programs with one discard and a colour store; others stay
    * one block (n_code1 = 0) */
   bool split_phases;
   /* with f16_samples: the driver also sets the F16-return T3 bits in a
    * dependent sample's state words (sgx_sample_info.f16), so a mediump
    * texel a dependent smp reads arrives as F16 in two registers (the
    * result format is the state's) */
   bool dep_f16_samples;
};

/* a vertex attribute's raw format in PA, unpacked by the shader. Components
 * a format doesn't hold read as 0, 0, 1 (y, z, w) */
enum sgx_vtx_fmt {
   SGX_VF_F32_4 = 0,
   SGX_VF_F32_1,
   SGX_VF_F32_2,
   SGX_VF_F32_3,
   SGX_VF_UNORM8_4,    /* one dword, x in the low byte, each / 255 */
   SGX_VF_SSCALED16_2, /* one dword, x the signed low half, y the high one, not
                          normalised */
};

/* varying -> texture coordinate set (sgx_assign_varyings) */
struct sgx_varying {
   char name[64];
   unsigned set;  /* first set (one per row) */
   unsigned comp; /* first component in its sets: varyings may share a set */
   unsigned rows, comps;
   unsigned precision; /* GLSL: 0 none, 1 high, 2 medium, 3 low */
};

struct sgx_varyings {
   unsigned n;
   struct sgx_varying v[32];
   /* set 0 reserved (vertex shaders that write gl_PointSize) */
   bool point_size;
   /* set 0 is the TA's vertex colour (output select b11): four F32
    * components after the position, iterated by the PDS as the colour source
    * into one register as BGRA8 (B in byte 0, R in byte 2),
    * the one source it iterates flat */
   bool colour;
   unsigned set_comps[SGX_MAX_SETS];
};

/* where the driver puts what the program reads (the USP's lists, decoded) */
#define SGX_SLOT_NONE     0xffff
#define SGX_MAX_CONST_MAP (1024 + 32) /* every constant number and literal */

struct sgx_const_map {
   uint16_t k; /* constant number: uniform component (sgx_nir.c's numbering) */
   uint16_t slot; /* sa[sa_base + slot]; SGX_SLOT_NONE: in memory only */
   uint16_t mem;  /* word of the constant memory block (mem_dwords), 0 none */
   uint8_t shift; /* bit offset in the slot: F16 0/16, C10 0/10/20 */
   /* 1 f32, 2 F16 (round to nearest), 3 C10 (round(v * 255), signed 10
    * bits); with driver_consts: 0x10 F16 of the C10 value c (fmt 3's rule):
    * c / 255 rounded to nearest F16, as pck.f16.c10 [scale] gives it
    * (hardware, tests/c10); 0x11 a literal: k = 0xffff, value (32 bits) ORed
    * into the slot at shift; 0x12 the vec4 at constants k..k+3 as packed
    * BGRA8, A << 24 | R << 16 | G << 8 | B, each channel clamped to [0, 1]
    * and rounded to nearest (the whole slot, shift 0) */
   uint8_t fmt;
   uint32_t value; /* fmt 0x11 */
};

struct sgx_iter_info {
   uint8_t set; /* texture coordinate set (DOUTI source = set + 1) */
   uint8_t comps;
   uint8_t pa;     /* first PA register */
   uint8_t regs;   /* PA registers it fills */
   uint8_t f16;    /* iterated as F16 (two components per register) */
   uint8_t sample; /* a non-dependent sample: the raw RGBA8 texel lands in pa */
   uint8_t proj;   /* ... with a projected coordinate (x/w, y/w) */
   uint8_t sampler;    /* ... of this sampler */
   uint8_t pointcoord; /* gl_PointCoord (set 0) */
   /* not interpolated: every varying in the set is flat, the provoking
    * vertex's value (DOUTI b25-26 = 2; 3 interpolates) */
   uint8_t flat;
};

/* A texture sample. A
 * non-dependent one is the PDS's: the raw RGBA8 texel lands in pa `reg`. A
 * dependent one is an smp the driver puts in before code[at], with the
 * sampler's state words as its src1, writing the raw texel to temporary
 * `reg`; the program unpacks it from there. */
struct sgx_sample_info {
   unsigned at; /* dependent: index in code[] it goes before */
   uint16_t id;
   uint8_t sampler;
   uint8_t set; /* non-dependent: the coordinate's set */
   uint8_t dim; /* 2 = 2D, 3 = cube or volume (smp.3d) */
   uint8_t dependent, proj;
   uint8_t reg; /* the raw texel: pa (non-dependent), r (dependent) */
   /* non-dependent: the coordinate's plain iteration, -1 none */
   int16_t coord_pa;
   uint8_t phase; /* dependent, split program: at indexes code1 */
   /* non-dependent, f16_samples: the texel returns F16 into reg (R lo, G hi)
    * and reg + 1 (B lo, A hi), not RGBA8 into reg; RGBA order for a
    * tex_bgra unit too (the TAG converts by channel) */
   uint8_t f16;
   uint64_t smp; /* dependent: the smp, destination r0, texture state #0 */
};

/* The program's structure, in order: runs of code, labels, branches, the
 * discard feedback point and texture samples. A driver needs only the
 * branches (to move their offsets when it inserts code) and the samples;
 * the USP writer needs all of it. */
enum sgx_block_kind {
   SGX_BLOCK_CODE,     /* code[at .. at + n) */
   SGX_BLOCK_LABEL,    /* label */
   SGX_BLOCK_BRANCH,   /* code[at] branches to label (offset resolved) */
   SGX_BLOCK_FEEDBACK, /* code[at] is the discard feedback nop */
   SGX_BLOCK_SAMPLE,   /* samples[at] */
};

struct sgx_block {
   uint8_t kind;
   uint16_t label;
   unsigned at, n;
};

#define SGX_MAX_SAMPLES 16

struct sgx_shader {
   unsigned stage; /* MESA_SHADER_VERTEX / _FRAGMENT */
   /* what the program reads and writes */
   /* first SA register of the constants (9 fragment, 10 vertex) */
   unsigned sa_base;
   unsigned n_consts;
   struct sgx_const_map consts[SGX_MAX_CONST_MAP];
   unsigned n_iters; /* fragment: PDS iterations, in PA order */
   struct sgx_iter_info iters[16];
   unsigned colour_pa; /* fragment: packed BGRA8 colour register */
   bool discards; /* fragment: p1 = keep at the feedback nop (USP labels 4/5) */
   /* fragment: gl_FragDepth, clamped to [0, 1], is in pa<depth_pa> at the
    * feedback nop, for a depthf there (predicated on p1 when it discards
    * too); not in the USP */
   bool writes_depth;
   unsigned depth_pa;
   /* vertex: PA registers read (attribute slot k = pa 4k..4k+3) */
   uint32_t in_mask[4];
   /* vertex: output set sizes (o4 on); 0 = unused */
   unsigned set_comps[SGX_MAX_SETS];
   bool point_size; /* vertex: writes the point size (f32), after the sets */
   unsigned n_out_regs; /* vertex: o registers written */
   /* vertex: user clip distances 0..n_clip-1, plane p's in o<clip_o + p>
    * through fdpc into cp<p>; the driver enables planes 0..n_clip-1 in the
    * TA's output select (b0-5) and counts n_clip more output dwords */
   unsigned n_clip, clip_o;
   /* SA registers used from sa_base on (constants and the compiler's own) */
   unsigned sa_slots;
   /* Constants past sa127 (counting 4 per dependent sample of a fragment
    * program after sa_slots): the most-used stay in SA, every constant gets
    * a word of a memory block the driver fills per draw like the SA ones,
    * and the program loads the others from it (ld.a.d). Word 0 is unused.
    * The driver loads SA slot mem_base_slot[j] with the block's address +
    * 512 * j. 0: no block */
   unsigned mem_dwords;
   unsigned n_mem_bases;
   uint8_t mem_base_slot[8];
   /* fragment: reads sa[front_sa], so it must be loaded */
   bool uses_front_face;
   uint64_t *code; /* main program, flat (branch targets resolved) */
   unsigned n_code;
   unsigned n_samples;
   struct sgx_sample_info samples[SGX_MAX_SAMPLES];
   struct sgx_block *blocks;
   unsigned n_blocks;
   /* split_phases: phase 1's program and its blocks (0: one block) */
   uint64_t *code1;
   unsigned n_code1;
   struct sgx_block *blocks1;
   unsigned n_blocks1;
   unsigned n_alu, n_tex, n_flow; /* instruction counts */
   unsigned n_temps;
   unsigned n_pa; /* PA registers: inputs and colour */
   char *log;     /* listing / errors (malloc'd) */
};

const struct nir_shader_compiler_options *sgx_nir_options(void);

/* GLSL ES varyings to texture coordinate sets; call on each stage's NIR
 * before linking removes unused ones */
void sgx_assign_varyings(struct nir_shader *nir, bool vertex,
                         struct sgx_varyings *out);

/* Varyings whose sets go past the ten (or past a set's four components):
 * re-placed in their order, sharing sets
 * (sgx_varying.comp); set_comps follows. Deterministic in the list, so two
 * stages with the same list agree. Anything that already fits is left as it
 * is. False: more than 10 x 4 components */
bool sgx_pack_varyings(struct sgx_varyings *v);

/* GL user clip planes per pixel: discard where any plane in ucp_enables
 * has a negative distance. The distances are the vertex shader's clip
 * distances (nir_lower_clip_vs: vec4 outputs clipdist_0 / clipdist_1 at
 * VARYING_SLOT_CLIP_DIST0 / 1), read here as inputs of the same names and
 * slots, which the driver places as it places other built-in varyings.
 * Call on the fragment shader before sgx_compile. Not for lines: a
 * discarding program breaks a lines draw. */
bool sgx_lower_clip_fs(struct nir_shader *fs, unsigned ucp_enables);

/* lower and compile; 0 on success */
int sgx_compile(struct nir_shader *nir, const struct sgx_compile_options *opts,
                const struct sgx_varyings *vary, struct sgx_shader *out);

void sgx_shader_free(struct sgx_shader *s);

#ifdef __cplusplus
}
#endif

#endif
