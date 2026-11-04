/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * The compiler's instruction forms as struct sgx_isa_instr: opcode, fixed
 * fields and operand slots, from docs/ISA.md's class sections.
 */
#include "sgx_forms.h"

#include "util/macros.h"

#define F(x) SGX_FILE_BIT(SGX_ISA_FILE_##x)

/* the generic destination and source slots (ISA.md "Operand slots") */
#define DST_FILES  (F(R) | F(O) | F(PA) | F(IX) | F(SA) | F(G) | F(IDX) | F(I))
#define SRC_FILES  (F(R) | F(O) | F(PA) | F(SA) | F(IX) | F(C) | F(G) | F(IMM) | F(I))
/* src0 without an extension bit, and with one (smp, ld) */
#define SRC0_FILES (F(R) | F(PA))
#define SRC0X_FILES (F(R) | F(PA) | F(O) | F(SA))
/* one-bit destination bank (smp, ld) */
#define DST1_FILES (F(R) | F(PA))

#define D    { SGX_ROLE_DST, DST_FILES }
#define D1   { SGX_ROLE_DST, DST1_FILES }
#define S0   { SGX_ROLE_SRC0, SRC0_FILES }
#define S0X  { SGX_ROLE_SRC0, SRC0X_FILES }
#define S1   { SGX_ROLE_SRC1, SRC_FILES }
#define S2   { SGX_ROLE_SRC2, SRC_FILES }
#define IMM  { SGX_ROLE_IMM, F(IMM) }
/* float: modifiers, format select */
#define S0A  { SGX_ROLE_SRC0, SRC0_FILES, false, true, true }
#define S1M  { SGX_ROLE_SRC1, SRC_FILES, true, true, true }
#define S2M  { SGX_ROLE_SRC2, SRC_FILES, true, true, true }
/* hw 1 and fmad16: modifiers, no format select */
#define S0Ah { SGX_ROLE_SRC0, SRC0_FILES, false, true, false }
#define S1Mh { SGX_ROLE_SRC1, SRC_FILES, true, true, false }
#define S2Mh { SGX_ROLE_SRC2, SRC_FILES, true, true, false }
/* test / testmask: format select, no modifiers */
#define S1T  { SGX_ROLE_SRC1, SRC_FILES, false, false, true }
#define S2T  { SGX_ROLE_SRC2, SRC_FILES, false, false, true }
#define PD   { SGX_ROLE_PDST, SGX_FILE_BIT(SGX_FILE_PRED) }
/* ima16's src2 negate */
#define S2N  { SGX_ROLE_SRC2, SRC_FILES, true }

#define FLOAT3(op)  [SGX_OP_##op] = { #op, SGX_ISA_OPC_##op, 4, { D, S0A, S1M, S2M } }
#define FLOAT2(op)  [SGX_OP_##op] = { #op, SGX_ISA_OPC_##op, 3, { D, S1M, S2M } }
#define SCALAR(op)  [SGX_OP_##op] = { #op, SGX_ISA_OPC_##op, 2, { D, S1Mh } }
#define PCK(d, s)   [SGX_OP_PCK_##d##_##s] = { "PCK_" #d "_" #s, SGX_ISA_OPC_PCK, 3, { D, S1, S2 }, \
                                              SGX_ISA_PCK_FMT_##d, SGX_ISA_PCK_FMT_##s }

static const struct sgx_form_info {
   const char *name;
   enum sgx_isa_opc opc;
   uint8_t nops;
   struct sgx_slot slot[SGX_MAX_OPS];
   uint8_t dfmt, sfmt;  /* pck */
} forms[SGX_OP_COUNT] = {
   FLOAT3(FMAD), FLOAT3(FADM), FLOAT3(FMSA), FLOAT3(FMAXMIN), FLOAT3(FMINMAX),
   FLOAT2(FSUBFLR), FLOAT2(FDP), FLOAT2(FDSX), FLOAT2(FDSY),
   SCALAR(FRCP), SCALAR(FRSQ), SCALAR(FLOG2), SCALAR(FEXP2), SCALAR(FSQRT),
   SCALAR(FSIN), SCALAR(FCOS),
   [SGX_OP_FMAD16] = { "FMAD16", SGX_ISA_OPC_FMAD16, 4, { D, S0Ah, S1Mh, S2Mh } },
   [SGX_OP_MOV] = { "MOV", SGX_ISA_OPC_MOV, 2, { D, S1 } },
   [SGX_OP_MOVC_F32] = { "MOVC_F32", SGX_ISA_OPC_MOVC_F32, 4, { D, S0, S1, S2 } },
   [SGX_OP_MOVC_I32] = { "MOVC_I32", SGX_ISA_OPC_MOVC_I32, 4, { D, S0, S1, S2 } },
   PCK(F16, F32), PCK(F16, F16), PCK(F16, U8), PCK(F16, C10), PCK(F32, F16),
   PCK(F32, U8), PCK(U8, F32), PCK(U8, F16), PCK(S16, F32), PCK(C10, F32),
   PCK(C10, C10),
   [SGX_OP_TEST] = { "TEST", SGX_ISA_OPC_TEST, 3, { PD, S1T, S2T } },
   [SGX_OP_TESTMASK] = { "TESTMASK", SGX_ISA_OPC_TESTMASK, 3, { D, S1T, S2T } },
   [SGX_OP_AND] = { "AND", SGX_ISA_OPC_AND, 3, { D, S1, S2 } },
   [SGX_OP_XOR] = { "XOR", SGX_ISA_OPC_XOR, 3, { D, S1, S2 } },
   [SGX_OP_SOPWM] = { "SOPWM", SGX_ISA_OPC_SOPWM, 3, { D, S1, S2 } },
   [SGX_OP_SOP3] = { "SOP3", SGX_ISA_OPC_SOP3, 4, { D, S0, S1, S2 } },
   [SGX_OP_SOP3L] = { "SOP3L", SGX_ISA_OPC_SOP3L, 4, { D, S0, S1, S2 } },
   [SGX_OP_IMA16] = { "IMA16", SGX_ISA_OPC_IMA16, 4, { D, S0, S1, S2N } },
   [SGX_OP_IMAE] = { "IMAE", SGX_ISA_OPC_IMAE, 4, { D, S0, S1, S2 } },
   [SGX_OP_SMP] = { "SMP", SGX_ISA_OPC_SMP, 4, { D1, S0X, S1, S2 } },
   [SGX_OP_LD] = { "LD", SGX_ISA_OPC_LD, 3, { D1, S0X, S1 } },
   [SGX_OP_LIMM] = { "LIMM", SGX_ISA_OPC_LIMM, 2, { D, IMM } },
   [SGX_OP_WDF] = { "WDF", SGX_ISA_OPC_WDF, 0 },
   [SGX_OP_BR] = { "BR", SGX_ISA_OPC_BR, 1, { IMM } },
   [SGX_OP_NOP] = { "NOP", SGX_ISA_OPC_NOP, 0 },
};

const struct sgx_slot *
sgx_form_slot(unsigned form, unsigned k)
{
   if (form >= SGX_OP_COUNT || k >= forms[form].nops)
      return NULL;
   return &forms[form].slot[k];
}

const char *
sgx_form_name(unsigned form)
{
   return form < SGX_OP_COUNT && forms[form].name ? forms[form].name : "?";
}

void
sgx_form_init(struct sgx_isa_instr *instr, unsigned form)
{
   const struct sgx_form_info *f = &forms[form];

   /* the write mask's default: the first channel, all four in sopwm */
   *instr = (struct sgx_isa_instr){ .opc = f->opc, .mask = form == SGX_OP_SOPWM ? 0xf : 1 };
   if (f->opc == SGX_ISA_OPC_PCK) {
      instr->dfmt = f->dfmt;
      instr->sfmt = f->sfmt;
      instr->dmask = 0xf;
   }
   /* without a LOD / range source, src2 is the unused pattern #0 */
   if (form == SGX_OP_SMP || form == SGX_OP_LD)
      instr->src[2] = (struct sgx_isa_src){ .file = SGX_ISA_FILE_IMM };
}

void
sgx_form_set(struct sgx_isa_instr *instr, unsigned form, unsigned id, int32_t v)
{
   switch (id) {
   case SGX_FLD_SKIPINV: instr->skipinv = v; break;
   case SGX_FLD_PRED:    instr->pred = v; break;
   case SGX_FLD_NOSCHED: instr->nosched = v; break;
   case SGX_FLD_RPT:     instr->rpt = v; break;
   case SGX_FLD_RCNT:    instr->rcnt = v; break;
   case SGX_FLD_MASK:    instr->mask = v; break;
   case SGX_FLD_MOE:     instr->moe = v; break;
   case SGX_FLD_INC0:    instr->inc[0] = v; break;
   case SGX_FLD_INC1:    instr->inc[1] = v; break;
   case SGX_FLD_INC2:    instr->inc[2] = v; break;
   case SGX_FLD_FMT:     instr->fmt = v; break;
   case SGX_FLD_F16:
      if (v)
         instr->src[1].fmt = SGX_ISA_FMT_F16;
      break;
   case SGX_FLD_C10:
      if (v)
         instr->src[1].fmt = SGX_ISA_FMT_C10;
      break;
   case SGX_FLD_CHAN:    instr->chan = v; break;
   case SGX_FLD_F16D:    instr->f16d = v; break;
   case SGX_FLD_SWZ0:    instr->swz[0] = v; break;
   case SGX_FLD_SWZ1:    instr->swz[1] = v; break;
   case SGX_FLD_SWZ2:    instr->swz[2] = v; break;
   case SGX_FLD_COND:
      if (form == SGX_OP_TEST || form == SGX_OP_TESTMASK)
         instr->tcond = v;
      else
         instr->cond = v;
      break;
   case SGX_FLD_ALU:     instr->alu = v; break;
   case SGX_FLD_TYPE:    instr->ttype = v; break;
   case SGX_FLD_DMASK:   instr->dmask = v; break;
   case SGX_FLD_SCALE:   instr->scale = v; break;
   case SGX_FLD_SGN:     instr->sgn = v; break;
   case SGX_FLD_F1:      instr->f1 = v; break;
   case SGX_FLD_F2:      instr->f2 = v; break;
   case SGX_FLD_T2:      instr->t2 = v; break;
   case SGX_FLD_COP:     instr->cop = v; break;
   case SGX_FLD_AOP:     instr->aop = v; break;
   case SGX_FLD_C1:      instr->c1 = v & 7; instr->c1c = v >> 3; break;
   case SGX_FLD_C2:      instr->c2 = v & 7; instr->c2c = v >> 3; break;
   case SGX_FLD_A:       instr->asel = v & 3; instr->aselc = v >> 2; break;
   case SGX_FLD_A1:      instr->a1 = v & 1; instr->a1c = v >> 1; break;
   case SGX_FLD_A2:      instr->a2 = v & 1; instr->a2c = v >> 1; break;
   case SGX_FLD_DIM:     instr->dim = v; break;
   case SGX_FLD_LOD:     instr->lod = v; break;
   case SGX_FLD_CFMT:    instr->cfmt = v; break;
   default:
      UNREACHABLE("unknown field");
   }
}

void
sgx_form_opd(struct sgx_isa_instr *instr, unsigned form, unsigned k,
             const struct sgx_isa_src *o)
{
   const struct sgx_slot *s = sgx_form_slot(form, k);

   switch (s ? s->role : SGX_ROLE_NONE) {
   case SGX_ROLE_DST:  instr->dst = *o; break;
   case SGX_ROLE_SRC0: instr->src[0] = *o; break;
   case SGX_ROLE_SRC1: instr->src[1] = *o; break;
   case SGX_ROLE_SRC2: instr->src[2] = *o; break;
   case SGX_ROLE_PDST: instr->pdst = o->num; break;
   case SGX_ROLE_IMM:
      if (form == SGX_OP_BR)
         instr->target = o->num;
      else
         instr->imm = o->num;
      break;
   default:
      break;
   }
}
