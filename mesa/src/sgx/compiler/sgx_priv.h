/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * SGX545 compiler internals: the backend IR between NIR and USSE words.
 *
 * One sgx_ins is one USSE instruction (a form from sgx_forms.h) over up to
 * `rpt` iterations; operands are sgx_refs that become USSE operands after
 * register allocation. Virtual registers (vregs) are runs of consecutive
 * temporaries, so a vector op can be one instruction with a repeat.
 */
#ifndef SGX_PRIV_H
#define SGX_PRIV_H

#include "util/list.h"
#include "util/macros.h"
#include "util/u_dynarray.h"

#include "sgx_compiler.h"
#include "sgx_forms.h"

struct nir_shader;
struct nir_def;

enum sgx_file {
   SGX_F_NONE,
   SGX_F_V, /* vreg: n = vreg, comp = component */
   SGX_F_R, /* physical temporary */
   SGX_F_PA,
   SGX_F_SA,
   SGX_F_O,
   SGX_F_C, /* hardware constant */
   SGX_F_G, /* special register */
   SGX_F_I, /* internal register */
   SGX_F_IMM,
   SGX_F_U,     /* uniform constant number (becomes sa after layout) */
   SGX_F_P,     /* predicate */
   SGX_F_USLOT, /* the absolute SA number of uniform constant n (an immediate,
                   after layout) */
   SGX_F_IXSA,  /* sa[idx1 + n] */
   SGX_F_IDX,   /* index register n (destination) */
   SGX_F_LIT, /* literal constant n (driver_consts: an SA slot the driver fills)
               */
};

struct sgx_ref {
   uint8_t file;
   uint8_t comp;
   uint8_t neg : 1, abs : 1;
   uint8_t step
      : 1; /* steps by one per iteration (else the same every iteration) */
   uint8_t h16
      : 1; /* an F16 half: n (comp for vregs) counts halves, 2 * reg + half */
   uint8_t c10 : 1; /* a C10 operand of the colour classes (format control) */
   uint8_t lane;    /* pck .l<n> */
   uint32_t n;
};

enum sgx_ins_kind {
   SGX_INS_ALU, /* any encoded instruction */
   SGX_INS_LABEL,
   SGX_INS_BR,       /* br to label; pred */
   SGX_INS_FEEDBACK, /* discard: the nop between the feedback and phase-1 labels
                      */
   SGX_INS_SAMPLE,   /* a USP texture sample (block 5) + unpack (block 6) */
};

#define SGX_MAX_FLD 10

struct sgx_sample;

struct sgx_ins {
   struct list_head link;
   uint8_t kind;
   uint16_t form;
   uint8_t pred; /* PRED3 code: 0 always, 1 p0, 2 p1, 5 !p0, 6 !p1 */
   uint8_t rpt;  /* iterations (1-16) */
   uint8_t ndst; /* leading operands that are written */
   uint8_t nops;
   uint8_t writes_pred; /* test: predicate number + 1 */
   bool noskip;         /* runs on helper instances too (skipinv clear) */
   uint8_t nfld;
   struct sgx_ref op[SGX_MAX_OPS];
   struct {
      uint16_t id;
      int32_t v;
   } fld[SGX_MAX_FLD];
   unsigned label; /* LABEL, BR */
   unsigned ip;    /* index in the linear order (liveness) */
   struct sgx_sample *smp;
};

struct sgx_vreg {
   unsigned size; /* registers, or halves for a 16-bit vreg */
   bool half;
   bool pa; /* phys is a PA register (place_in_pa) */
   bool o;  /* ... an output register (place_in_o) */
   int phys;
   int first, last; /* live range in ins ips, -1 = unused */
};

struct sgx_iter {
   unsigned set, comps, pa;
   bool sample;     /* non-dependent sample entry (list A) */
   bool proj;       /* ... iterated projected (x/w, y/w) */
   bool f16;        /* iterated as F16 (mediump), two components per register */
   bool pointcoord; /* gl_PointCoord (set 0) */
   unsigned sampler;
};

struct sgx_sample {
   unsigned id;
   unsigned sampler;
   bool dependent, proj;
   unsigned dim; /* 2 = 2D, 3 = cube */
   unsigned set; /* non-dependent: the coordinate's texture coordinate set */
   int coord_pa; /* ... and its plain iteration, -1 none */
   /* where the PDS leaves the raw texel (RGBA8, unpacked in place) */
   unsigned pa;
   unsigned lod; /* dependent: 0 none, 1 bias, 2 replace */
   /* unpacked to F16 (two channels per register), else left as RGBA8 */
   bool f16;
   bool f16coord; /* dependent: F16 coordinates, two per register (smp .f16) */
   /* dependent: the SAMPLE pseudo-instruction (dst raw, coord, lod) */
   struct sgx_ins *ins;
};

struct sgx_ctx {
   uint64_t debug; /* enum sgx_debug */
   struct nir_shader *nir;
   const struct sgx_compile_options *opts;
   const struct sgx_varyings *vary;
   unsigned stage;
   char *log;
   size_t log_len, log_cap;
   bool failed;

   /* I/O */
   /* register per I/O component: [location * 4 + component] (VARYING_SLOT_* /
    * VERT_ATTRIB_*), -1 none */
   int in_reg[512], out_reg[512];
#define SGX_IN_HALF                                                            \
   0x10000 /* in_reg: an F16-iterated component, low bits = its half */
#define SGX_ITER_FRAGCOORD                                                     \
   14 /* list A iterator of gl_FragCoord (DDK capture) */
   struct sgx_iter iters[16];
   unsigned n_iters;
   unsigned n_pa_inputs;
   unsigned psize_reg, n_out_regs;

   /* NIR def -> value */
   struct sgx_val *vals;
   struct util_dynarray vregs; /* struct sgx_vreg */
   struct list_head ins;
   unsigned n_labels;

   /* uniforms */
   uint8_t const_used[1024];
   /* 1 f32 (list C kind 1), 2 f16 (kind 2), 3 c10 (kind 3) */
   uint8_t const_fmt[1024];
   int const_slot[1024];
   /* bit offset in the slot (f16 0/16, c10 0/10/20) */
   uint8_t const_shift[1024];
   /* the first of a lowp vec4 uniform (C10 candidate) */
   uint8_t const_lowp[1024];
   /* GLSL precision of the uniform behind constant k (0 unknown) */
   uint8_t const_prec[1024];
   /* dynamically indexed arrays: first constant, constants it spans */
   struct {
      uint16_t k, range;
   } ix_arr[64];
   unsigned n_ix_arr;
   unsigned n_sa; /* slots (constants, then literals) */
   unsigned sa_base;
   /* where the slots live (place_constants): SA slot from sa_base, -1 memory
    * only; with mem_dwords every slot is also block word slot + 1 */
   int slot_sa[1024 + 32];
   unsigned n_sa_phys; /* SA slots used: kept slots, then the bases */
   unsigned mem_dwords;
   unsigned n_mem_bases;
   uint8_t mem_base_sa[8]; /* SA slot holding block address + 512 * j */
   uint32_t lit_bits[32];  /* driver_consts: literal slots' values */
   /* ... or, >= 0, the slot is constants k..k+3 packed to BGRA8 (0x12) */
   int lit_packed_k[32];
   int lit_slot[32];
   unsigned n_lits;
   bool lit_off; /* while a register is needed (fmad16 src0) */

   /* fragment */
   struct sgx_ref color[4];
   unsigned color_mask; /* the channels of color[] a store wrote */
   bool has_color;
   struct sgx_ref depth; /* gl_FragDepth */
   bool has_depth;
   unsigned depth_pa;
   int keep_vreg; /* discard: 1.0 = keep, -1 none */
   bool discards;
   bool keep_pred; /* the single discard wrote p1 (= keep) itself */
   bool p16;       /* mediump in F16, as the DDK computes it */
   /* emit_alu writes its def, never the register NIR stores it to */
   bool def_dest;
   int face_vreg; /* gl_FrontFacing as made at entry; -1 none */
   bool face_int; /* ... as (g16 ^ sa) & 1, read only by selects */
   int face_f32;  /* ... as 1.0 / 0.0 (face_int: made when a select needs it) */
   struct sgx_ins *face_and; /* the and that makes face_int */
   unsigned colour_pa;
   struct sgx_sample *samples[16];
   unsigned n_samples;

   struct set *skip;        /* NIR instructions the backend doesn't emit */
   struct set *shadow_regs; /* decl_regs read as aliases (find_shadow_regs) */
   /* the colour as a lowp C10 expression (emit_c10) */
   struct nir_def *c10_root;
   /* the colour is a texel or a vec4 uniform as it is, through conversions
    * (find_colour_source): the def the store reads, the texel, the
    * uniform's constant number (-1 none) */
   struct nir_def *colour_end;
   struct nir_tex_instr *colour_tex;
   int colour_k;
   /* emit_color: the raw RGBA8 texel register / the packed literal slot */
   bool has_colour_raw, has_colour_packed;
   struct sgx_ref colour_raw;
   /* the one discard's comparison (p1 directly) */
   struct nir_alu_instr *discard_cmp;
   struct hash_table *proj; /* nir_tex_instr -> its projected varying load */
   struct hash_table *efo; /* nir_alu_instr (a chain's root) -> its efo_chain */
   /* nir_if -> the comparison it alone reads, tested into p0 directly */
   struct hash_table *if_cmp;

   /* results */
   unsigned n_temps, n_pa;
   struct util_dynarray words; /* flat main program (uint64_t) */
};

void sgx_lower(struct sgx_ctx *c);
void sgx_logf(struct sgx_ctx *c, const char *fmt, ...);
void sgx_fail(struct sgx_ctx *c, const char *fmt, ...);
int sgx_finish_program(struct sgx_ctx *c, struct sgx_shader *out);
int sgx_encode_ins(struct sgx_ctx *c, const struct sgx_ins *i, uint64_t *w);

static inline struct sgx_vreg *
sgx_vreg(struct sgx_ctx *c, unsigned i)
{
   return util_dynarray_element(&c->vregs, struct sgx_vreg, i);
}

/* the SA register of uniform constant k / literal i (kept in SA) */
/* SGX_COMPILER_DEBUG flags (sgx_compile.c): each no* flag turns one
 * optimization off, to bisect a miscompile without rebuilding */
enum sgx_debug {
   SGX_DBG_NO_PA = BITFIELD64_BIT(0),
   SGX_DBG_NO_OUT = BITFIELD64_BIT(1),
   SGX_DBG_NO_COALESCE = BITFIELD64_BIT(2),
   SGX_DBG_NO_SPLIT = BITFIELD64_BIT(3),
   SGX_DBG_NO_FOLD = BITFIELD64_BIT(4),
   SGX_DBG_NO_SELFMOV = BITFIELD64_BIT(5),
   SGX_DBG_NO_LIT32 = BITFIELD64_BIT(6),
   SGX_DBG_NO_NARROW16 = BITFIELD64_BIT(7),
   SGX_DBG_NO_MEDIUMP = BITFIELD64_BIT(8),
   SGX_DBG_NO_MINMAX32 = BITFIELD64_BIT(9),
   SGX_DBG_NO_CONDMASK = BITFIELD64_BIT(10),
   SGX_DBG_NO_CSEL1 = BITFIELD64_BIT(11),
   SGX_DBG_NO_CSEL32 = BITFIELD64_BIT(12),
   SGX_DBG_NO_COISSUE = BITFIELD64_BIT(13),
   SGX_DBG_NO_F16_CONST = BITFIELD64_BIT(14),
   SGX_DBG_NO_F16_ITER = BITFIELD64_BIT(15),
   SGX_DBG_UNROLL_EXITS = BITFIELD64_BIT(18),
   SGX_DBG_NO_MASK_BOOL = BITFIELD64_BIT(19),
   SGX_DBG_NO_IF_TEST = BITFIELD64_BIT(20),
   SGX_DBG_NO_TEX_COLOR = BITFIELD64_BIT(16),
   SGX_DBG_NO_PACKED_COLOR = BITFIELD64_BIT(17),
};

static inline bool
sgx_debug(const struct sgx_ctx *c, uint64_t flag)
{
   return (c->debug & flag) != 0;
}

static inline unsigned
sgx_const_sa(const struct sgx_ctx *c, unsigned k)
{
   return c->sa_base + c->slot_sa[c->const_slot[k]];
}

static inline unsigned
sgx_lit_sa(const struct sgx_ctx *c, unsigned i)
{
   return c->sa_base + c->slot_sa[c->lit_slot[i]];
}

#endif
