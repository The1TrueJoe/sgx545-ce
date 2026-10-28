/*
 * USSE (SGX545) machine instructions: one instruction = a form from
 * re/isa/hw.py (usse_forms.h, generated), its field values and operands.
 * usse_encode() is a port of hw.encode() over the generated tables.
 */
#ifndef USSE_H
#define USSE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "usse_forms.h"

enum usse_kind {
   USSE_K_R,      /* temporary */
   USSE_K_O,      /* output */
   USSE_K_PA,     /* primary attribute */
   USSE_K_SA,     /* secondary attribute */
   USSE_K_C,      /* hardware constant */
   USSE_K_G,      /* special register */
   USSE_K_I,      /* internal register */
   USSE_K_IMM,    /* #n */
   USSE_K_IX,     /* bank[idxN+off] */
   USSE_K_IDX,    /* index register (destination) */
   USSE_K_P,      /* predicate p0-p3 */
   USSE_K_NONE = 0xff,
};

struct usse_opd {
   uint8_t kind;
   uint8_t ixbank;   /* USSE_K_IX: r/o/pa/sa (0-3) */
   uint8_t idx;      /* USSE_K_IX: 1 or 2 */
   uint8_t neg : 1, abs : 1;
   uint8_t f16 : 1;  /* an F16 operand (format select or efo's format control) */
   uint8_t half : 1; /* which F16 half, where numbers count halves */
   uint8_t fcsel : 1; /* format-control operand (C10 in the colour classes, test's C10 compares) */
   uint8_t swz : 1;  /* smlsi value is a swizzle (n = four 2-bit selects) */
   uint8_t nw : 1;   /* atst8 predicate destination not written */
   uint8_t lane;     /* pck .l<n> lane select */
   int32_t n;        /* number; USSE_K_IX: the offset; immediates may be negative where signed */
};

struct usse_bits {
   uint8_t n;
   uint8_t b[32];
};

struct usse_fld {
   uint16_t id;
   uint8_t n;
   uint8_t b[32];
   int32_t def;      /* stored value when not given */
   int32_t bias;     /* text value = stored + bias */
   int16_t dep;      /* the field is only there when field dep... */
   uint8_t depop;    /* ...== depk (0) or != depk (1); dep < 0: always */
   int32_t depk;
};

enum usse_slot_type {
   USSE_SLOT_GEN,      /* hw.Opd (and COpd, SOpd) */
   USSE_SLOT_IMM,      /* hw_mem.Imm: plain immediate bits */
   USSE_SLOT_MOEVAL,   /* hw_mem.MoeVal: increment or swizzle (extra = the flag bit) */
   USSE_SLOT_PRED,     /* hw_pack.POpd: p0-p3 */
   USSE_SLOT_PREDDST,  /* hw_mem.PredDst: p<n> in h7-8, h6 = write */
   USSE_SLOT_SAMEBANK, /* hw_int.SameBank: number only, bank from src1 */
};

struct usse_slot {
   const char *role;   /* NULL: a slot this encoder can't fill */
   uint8_t type;
   struct usse_bits num, bank, ext, neg, abs;
   struct usse_bits extra; /* MOEVAL: swizzle flag; GEN: the .l<n> lane bits */
   int8_t enc[11];   /* kind -> bank << 1 | ext, -1 = not here (0 for a fixed kind) */
   uint8_t fmt;      /* follows format select */
   uint8_t half16;
   int8_t fc;        /* format-control bit that applies, -1 none */
   uint8_t fmtname;  /* test's operands: a C10 one is a whole register */
   uint8_t isigned;  /* IMM: two's complement */
   int16_t dep;      /* only present in some modes, as usse_fld */
   uint8_t depop;
   int32_t depk;
};

struct usse_form {
   const char *name;
   uint64_t fmask, fval;
   unsigned nflds;
   const struct usse_fld *flds;
   unsigned nops;
   const struct usse_slot *ops;
};

#define USSE_MAX_OPS 6

struct usse_inst {
   uint16_t form;
   uint8_t nops;
   uint8_t set[USSE_NUM_FIELDS];
   int32_t f[USSE_NUM_FIELDS];
   struct usse_opd op[USSE_MAX_OPS];
};

/* format control state (setfc): bit 0 efo F16, bit 1 colour C10; 2 at program start */
#define USSE_FC_DEFAULT 2

void usse_init(struct usse_inst *inst, enum usse_form_id form);
void usse_set(struct usse_inst *inst, enum usse_field_id f, int32_t v);
int32_t usse_get(const struct usse_inst *inst, enum usse_field_id f);
/* -> 0, or -1 with *err set */
int usse_encode(const struct usse_inst *inst, unsigned fc, uint64_t *word, const char **err);
const char *usse_form_name(enum usse_form_id form);
const char *usse_field_name(enum usse_field_id f);

static inline struct usse_opd
usse_reg(enum usse_kind k, unsigned n)
{
   struct usse_opd o = {0};
   o.kind = k;
   o.n = n;
   return o;
}

#endif
