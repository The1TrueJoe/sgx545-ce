/*
 * SGX545 compiler internals: the backend IR between NIR and USSE words.
 *
 * One sgx_ins is one USSE instruction (a form from usse_forms.h) over up to
 * `rpt` iterations; operands are sgx_refs that become USSE operands after
 * register allocation. Virtual registers (vregs) are runs of consecutive
 * temporaries, so a vector op can be one instruction with a repeat.
 */
#ifndef SGX_PRIV_H
#define SGX_PRIV_H

#include "util/list.h"
#include "util/u_dynarray.h"

#include "sgx_compiler.h"
#include "usse.h"

struct nir_shader;
struct nir_def;

enum sgx_file {
   SGX_F_NONE,
   SGX_F_V,     /* vreg: n = vreg, comp = component */
   SGX_F_R,     /* physical temporary */
   SGX_F_PA,
   SGX_F_SA,
   SGX_F_O,
   SGX_F_C,     /* hardware constant */
   SGX_F_I,     /* internal register */
   SGX_F_IMM,
   SGX_F_U,     /* uniform constant number (becomes sa after layout) */
   SGX_F_P,     /* predicate */
};

struct sgx_ref {
   uint8_t file;
   uint8_t comp;
   uint8_t neg : 1, abs : 1;
   uint8_t step : 1;    /* steps by one per iteration (else the same every iteration) */
   uint8_t h16 : 1;     /* an F16 half: n (comp for vregs) counts halves, 2 * reg + half */
   uint8_t lane;        /* pck .l<n> */
   uint32_t n;
};

enum sgx_ins_kind {
   SGX_INS_ALU,         /* any encoded instruction */
   SGX_INS_LABEL,
   SGX_INS_BR,          /* br to label; pred */
   SGX_INS_FEEDBACK,    /* discard: the nop between the feedback and phase-1 labels */
   SGX_INS_SAMPLE,      /* a USP texture sample (block 5) + unpack (block 6) */
};

#define SGX_MAX_FLD 10

struct sgx_sample;

struct sgx_ins {
   struct list_head link;
   uint8_t kind;
   uint16_t form;
   uint8_t pred;        /* PRED3 code: 0 always, 1 p0, 2 p1, 5 !p0, 6 !p1 */
   uint8_t rpt;         /* iterations (1-16) */
   uint8_t ndst;        /* leading operands that are written */
   uint8_t nops;
   uint8_t writes_pred; /* test: predicate number + 1 */
   uint8_t nfld;
   struct sgx_ref op[USSE_MAX_OPS];
   struct { uint16_t id; int32_t v; } fld[SGX_MAX_FLD];
   unsigned label;      /* LABEL, BR */
   unsigned ip;         /* index in the linear order (liveness) */
   struct sgx_sample *smp;
};

struct sgx_vreg {
   unsigned size;       /* registers, or halves for a 16-bit vreg */
   bool half;
   int phys;
   int first, last;     /* live range in ins ips, -1 = unused */
};

struct sgx_iter {
   unsigned set, comps, pa;
   bool sample;         /* non-dependent sample entry (list A) */
   bool proj;           /* ... iterated projected (x/w, y/w) */
   bool f16;            /* iterated as F16 (mediump), two components per register */
   unsigned sampler;
};

struct sgx_sample {
   unsigned id;
   unsigned sampler;
   bool dependent, proj;
   unsigned dim;        /* 2 = 2D, 3 = cube */
   unsigned set;        /* non-dependent: the coordinate's texture coordinate set */
   int coord_pa;        /* ... and its plain iteration, -1 none */
   unsigned pa;         /* where the PDS leaves the raw texel (RGBA8, unpacked in place) */
   unsigned lod;        /* dependent: 0 none, 1 bias, 2 replace */
   bool f16;            /* unpacked to F16 (two channels per register), else left as RGBA8 */
   struct sgx_ins *ins; /* dependent: the SAMPLE pseudo-instruction (dst raw, coord, lod) */
};

struct sgx_ctx {
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
#define SGX_IN_HALF 0x10000      /* in_reg: an F16-iterated component, low bits = its half */
   struct sgx_iter iters[16];
   unsigned n_iters;
   unsigned n_pa_inputs;
   unsigned psize_reg, n_out_regs;

   /* NIR def -> value */
   struct sgx_val *vals;
   struct util_dynarray vregs;   /* struct sgx_vreg */
   struct list_head ins;
   unsigned n_labels;

   /* uniforms */
   uint8_t const_used[1024];
   uint8_t const_fmt[1024];      /* 1 f32 (list C kind 1), 2 f16 (kind 2), 3 c10 (kind 3) */
   int const_slot[1024];
   uint8_t const_shift[1024];    /* bit offset in the slot (f16 0/16, c10 0/10/20) */
   unsigned n_sa;
   unsigned sa_base;

   /* fragment */
   struct sgx_ref color[4];
   bool has_color;
   int keep_vreg;                /* discard: 1.0 = keep, -1 none */
   bool discards;
   bool keep_pred;               /* the single discard wrote p1 (= keep) itself */
   bool p16;                     /* mediump in F16, as the DDK computes it */
   unsigned colour_pa;
   struct sgx_sample *samples[16];
   unsigned n_samples;

   struct set *skip;             /* NIR instructions the backend doesn't emit */
   struct nir_alu_instr *discard_cmp; /* the one discard's comparison (p1 directly) */
   struct hash_table *proj;      /* nir_tex_instr -> its projected varying load */

   /* results */
   unsigned n_temps, n_pa;
   struct util_dynarray words;   /* flat main program (uint64_t) */
};

void sgx_lower(struct sgx_ctx *c);
void sgx_logf(struct sgx_ctx *c, const char *fmt, ...);
void sgx_fail(struct sgx_ctx *c, const char *fmt, ...);
int sgx_write_usp(struct sgx_ctx *c, struct sgx_shader *out);
int sgx_encode_ins(struct sgx_ctx *c, const struct sgx_ins *i, uint64_t *w);

static inline struct sgx_vreg *
sgx_vreg(struct sgx_ctx *c, unsigned i)
{
   return util_dynarray_element(&c->vregs, struct sgx_vreg, i);
}

#endif
