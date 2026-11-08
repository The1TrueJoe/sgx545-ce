/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * The instruction forms the compiler emits, on top of the isaspec encoder
 * (src/sgx/isa). A form is an opcode plus the fields the compiler fixes
 * for it; its operands are the IR's op[] in text order (docs/ISA.md), and
 * sgx_form_slot() says which slot each one lands in and what that slot can
 * hold.
 */
#ifndef SGX_FORMS_H
#define SGX_FORMS_H

#include <stdbool.h>
#include <stdint.h>

#include "sgx_isa.h"

#define SGX_MAX_OPS 4

enum sgx_form {
   SGX_OP_NONE,
   /* hw 0, 2-4: float */
   SGX_OP_FMAD,
   SGX_OP_FADM,
   SGX_OP_FMSA,
   SGX_OP_FSUBFLR,
   SGX_OP_FDP,
   SGX_OP_FMAXMIN,
   SGX_OP_FMINMAX,
   SGX_OP_FDSX,
   SGX_OP_FDSY,
   /* hw 1: scalar functions, one source */
   SGX_OP_FRCP,
   SGX_OP_FRSQ,
   SGX_OP_FLOG2,
   SGX_OP_FEXP2,
   SGX_OP_FSQRT,
   SGX_OP_FSIN,
   SGX_OP_FCOS,
   /* hw 6 */
   SGX_OP_FMAD16,
   /* hw 5 */
   SGX_OP_MOV,
   SGX_OP_MOVC_F32,
   SGX_OP_MOVC_I32,
   /* hw 8: pck.<dst>.<src> */
   SGX_OP_PCK_F16_F32,
   SGX_OP_PCK_F16_F16,
   SGX_OP_PCK_F16_U8,
   SGX_OP_PCK_F16_C10,
   SGX_OP_PCK_F32_F16,
   SGX_OP_PCK_F32_U8,
   SGX_OP_PCK_U8_F32,
   SGX_OP_PCK_U8_F16,
   SGX_OP_PCK_U8_U8,
   SGX_OP_PCK_S16_F32,
   SGX_OP_PCK_C10_F32,
   SGX_OP_PCK_C10_C10,
   SGX_OP_PCK_F32_S16,
   /* hw 9, f, a-b */
   SGX_OP_TEST,
   SGX_OP_TESTMASK,
   SGX_OP_AND,
   SGX_OP_XOR,
   /* hw 11, 12: colour */
   SGX_OP_SOPWM,
   SGX_OP_SOP3,
   SGX_OP_SOP3L,
   /* hw 14, 15: integer */
   SGX_OP_IMA16,
   SGX_OP_IMAE,
   /* hw 1c, 1d, 1f */
   SGX_OP_SMP,
   SGX_OP_LD,
   SGX_OP_LIMM,
   SGX_OP_WDF,
   SGX_OP_BR,
   SGX_OP_NOP,
   /* hw 07, 1b: co-issue */
   SGX_OP_EFO,
   SGX_OP_FDUAL,
   SGX_OP_COUNT,
};

/*
 * Fields the compiler sets on a form, by name (docs/ISA.md). A select
 * (c1, c2, a, a1, a2) is its value with the complement as the next bit up.
 */
enum sgx_field {
   SGX_FLD_SKIPINV,
   SGX_FLD_PRED,
   SGX_FLD_NOSCHED,
   SGX_FLD_RPT,
   SGX_FLD_RCNT,
   SGX_FLD_MASK,
   SGX_FLD_MOE,
   SGX_FLD_INC0,
   SGX_FLD_INC1,
   SGX_FLD_INC2,
   SGX_FLD_FMT,  /* format select */
   SGX_FLD_F16,  /* hw 1: F16 source */
   SGX_FLD_C10,  /* hw 1: C10 source */
   SGX_FLD_CHAN, /* hw 1: half / channel */
   SGX_FLD_F16D, /* hw 1: F16 destination */
   SGX_FLD_SWZ0, /* fmad16 source swizzles */
   SGX_FLD_SWZ1,
   SGX_FLD_SWZ2,
   SGX_FLD_COND,  /* movc condition, test condition */
   SGX_FLD_ALU,   /* test ALU op */
   SGX_FLD_TYPE,  /* testmask width */
   SGX_FLD_DMASK, /* pck byte mask */
   SGX_FLD_SCALE, /* pck scale */
   SGX_FLD_SGN,   /* ima16 / imae */
   SGX_FLD_F1,    /* ima16 src1 / src2 formats */
   SGX_FLD_F2,
   SGX_FLD_T2,  /* imae src2 type */
   SGX_FLD_COP, /* sop colour / alpha ops and selects */
   SGX_FLD_AOP,
   SGX_FLD_C1,
   SGX_FLD_C2,
   SGX_FLD_A,
   SGX_FLD_A1,
   SGX_FLD_A2,
   SGX_FLD_DIM, /* smp */
   SGX_FLD_LOD,
   SGX_FLD_CFMT,
   SGX_FLD_EFO_D, /* efo: what the destination gets, the internal */
   SGX_FLD_EFO_I, /* writes, the adder and multiplier inputs */
   SGX_FLD_EFO_A,
   SGX_FLD_EFO_M,
   SGX_FLD_WI0, /* efo: write i0 / i1 */
   SGX_FLD_WI1,
   SGX_FLD_A0LNEG, /* efo: negate A0's left / right input */
   SGX_FLD_A0RNEG,
   SGX_FLD_PRI, /* fdual: the primary and secondary ops */
   SGX_FLD_SEC,
   SGX_FLD_SD, /* fdual: the secondary's selects */
   SGX_FLD_SB,
   SGX_FLD_SC,
   SGX_FLD_MSRC,
   SGX_FLD_COUNT,
};

enum sgx_role {
   SGX_ROLE_NONE,
   SGX_ROLE_DST,
   SGX_ROLE_SRC0,
   SGX_ROLE_SRC1,
   SGX_ROLE_SRC2,
   SGX_ROLE_PDST, /* test's predicate */
   SGX_ROLE_IMM,  /* limm's value, br's target */
};

#define SGX_FILE_BIT(f) (1u << (f))
/* a predicate operand: not a register file of the encoder */
#define SGX_FILE_PRED 15

struct sgx_slot {
   uint8_t role;   /* enum sgx_role */
   uint16_t files; /* SGX_FILE_BIT(enum sgx_isa_file) the slot encodes */
   bool neg, abs;
   bool fmt; /* follows format select (F16 numbers count halves) */
};

static inline bool
sgx_form_is_scalar(unsigned form)
{
   return form >= SGX_OP_FRCP && form <= SGX_OP_FCOS;
}

/* operand k of form f, NULL past its operands */
const struct sgx_slot *sgx_form_slot(unsigned form, unsigned k);
const char *sgx_form_name(unsigned form);

/* the instruction for form f with its fixed fields */
void sgx_form_init(struct sgx_isa_instr *instr, unsigned form);
/* set field id of form f's instruction */
void sgx_form_set(struct sgx_isa_instr *instr, unsigned form, unsigned id,
                  int32_t v);
/* operand k of form f into its slot */
void sgx_form_opd(struct sgx_isa_instr *instr, unsigned form, unsigned k,
                  const struct sgx_isa_src *o);

#endif
