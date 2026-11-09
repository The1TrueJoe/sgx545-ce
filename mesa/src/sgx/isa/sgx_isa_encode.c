/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * Glue for the encoder isaspec generates from sgx.xml: the leaf
 * selection for each bitset hierarchy, and the helpers the XML's <map>
 * expressions use. Everything about bit positions lives in the XML.
 */

#include "sgx_isa.h"

#include "util/macros.h"

/*
 * gen is what the XML's expressions read as ISA_GPU_ID(): the format
 * control state, bit 0 = C10 enable, bit 1 = efo F16 enable.
 */
struct encode_state {
   uint32_t gen;
};

static inline enum sgx_isa_opc
__instruction_case(struct encode_state *s, const struct sgx_isa_instr *instr)
{
   return instr->opc;
}

static inline enum sgx_isa_case_dst
__dst_case(struct encode_state *s, const struct sgx_isa_src *o)
{
   switch (o->file) {
   case SGX_ISA_FILE_R:
      return SGX_DST_R;
   case SGX_ISA_FILE_O:
      return SGX_DST_O;
   case SGX_ISA_FILE_PA:
      return SGX_DST_PA;
   case SGX_ISA_FILE_SA:
      return SGX_DST_SA;
   case SGX_ISA_FILE_IX:
      return SGX_DST_IX;
   case SGX_ISA_FILE_C:
   case SGX_ISA_FILE_G:
      return SGX_DST_G;
   case SGX_ISA_FILE_IDX:
      return SGX_DST_IDX;
   case SGX_ISA_FILE_I:
      return SGX_DST_I;
   default:
      return SGX_DST__NONE;
   }
}

static inline enum sgx_isa_case_src
__src_case(struct encode_state *s, const struct sgx_isa_src *o)
{
   switch (o->file) {
   case SGX_ISA_FILE_R:
      return SGX_SRC_R;
   case SGX_ISA_FILE_O:
      return SGX_SRC_O;
   case SGX_ISA_FILE_PA:
      return SGX_SRC_PA;
   case SGX_ISA_FILE_SA:
      return SGX_SRC_SA;
   case SGX_ISA_FILE_IX:
      return SGX_SRC_IX;
   case SGX_ISA_FILE_C:
   case SGX_ISA_FILE_G:
      return SGX_SRC_C;
   case SGX_ISA_FILE_IMM:
      return SGX_SRC_IMM;
   case SGX_ISA_FILE_I:
      return SGX_SRC_I;
   default:
      return SGX_SRC__NONE;
   }
}

/* src0 slots have no internal bank: i<n> is r(124 + n) (ISA.md "Registers"). */
static inline enum sgx_isa_case_src0
__src0_case(struct encode_state *s, const struct sgx_isa_src *o)
{
   switch (o->file) {
   case SGX_ISA_FILE_R:
   case SGX_ISA_FILE_I:
      return SGX_SRC0_R;
   case SGX_ISA_FILE_O:
      return SGX_SRC0_O;
   case SGX_ISA_FILE_PA:
      return SGX_SRC0_PA;
   case SGX_ISA_FILE_SA:
      return SGX_SRC0_SA;
   default:
      return SGX_SRC0__NONE;
   }
}

static inline enum sgx_isa_case_dst1
__dst1_case(struct encode_state *s, const struct sgx_isa_src *o)
{
   switch (o->file) {
   case SGX_ISA_FILE_R:
   case SGX_ISA_FILE_I:
      return SGX_DST1_R;
   case SGX_ISA_FILE_PA:
      return SGX_DST1_PA;
   default:
      return SGX_DST1__NONE;
   }
}

/*
 * The number field: g<n> is n + 0x40 on the c/g bank, i<n> in an r slot is
 * 124 + n, a negative immediate its 7-bit two's complement (the signed
 * immediates of ima16, imae, ...: ISA.md "imae").
 */
static inline uint32_t
sgx_isa_opnd_num(const struct sgx_isa_src *o)
{
   switch (o->file) {
   case SGX_ISA_FILE_G:
      return o->num + 0x40;
   case SGX_ISA_FILE_IMM:
      return (int32_t)o->num < 0 ? o->num & 0x7f : o->num;
   default:
      return o->num;
   }
}

static inline uint32_t
sgx_isa_reg_num(const struct sgx_isa_src *o)
{
   return o->file == SGX_ISA_FILE_I ? 124 + o->num : o->num;
}

static inline const struct sgx_isa_src *
sgx_isa_noext(const struct sgx_isa_src *o, struct sgx_isa_src *tmp)
{
   if (o->file != SGX_ISA_FILE_I)
      return o;
   *tmp = *o;
   tmp->file = SGX_ISA_FILE_R;
   tmp->num = 124 + o->num;
   return tmp;
}

static inline unsigned
sgx_isa_rpt(const struct sgx_isa_instr *instr)
{
   return instr->rpt ? instr->rpt - 1 : 0;
}

/*
 * Operand helpers for the <map>s: encode an operand through its slot's
 * hierarchy and return NUM | BANK << 7 | EXT << 9 (src0: EXT << 8).
 */
#define SGX_OPND(root, m, o)                                                   \
   ((uint32_t)bitmask_to_uint64_t(                                             \
      encode_##root(s, &(struct bitset_params){.MODE = (m)}, (o))))
#define SGX_DST(m)    SGX_OPND(_dst, m, &src->dst)
#define SGX_SRC(n, m) SGX_OPND(_src, m, &src->src[n])
#define SGX_SRC0(m)   SGX_OPND(_src0, m, &src->src[0])
#define SGX_DST1()    SGX_OPND(_dst1, 0, &src->dst)
/* Slots without an extension bit: i<n> is r(124 + n) there. */
#define SGX_DSTX(m)                                                            \
   SGX_OPND(_dst, m, sgx_isa_noext(&src->dst, &(struct sgx_isa_src){0}))
#define SGX_SRCX(n, m)                                                         \
   SGX_OPND(_src, m, sgx_isa_noext(&src->src[n], &(struct sgx_isa_src){0}))
#define SGX_IS_IMM(n) (src->src[n].file == SGX_ISA_FILE_IMM)
/* Without a destination, test's dst field holds nodst (default 0x280). */
#define SGX_TDST(m)                                                            \
   (src->dst.file != SGX_ISA_FILE_NONE                                         \
       ? SGX_DST(m)                                                            \
       : (src->has_nodst ? src->nodst : 0x280))
/* rlp's src2 slot as one 10-bit value (default 0x300, the unused pattern) */
#define SGX_UNUSED2() (src->has_unused2 ? src->unused2 : 0x300)
/* A named <expr> of the #instruction hierarchy: SGX_EXPR(__float_smode). */
#define SGX_EXPR(n) __instruction_expr_##n(s, p, src)

#include "sgx-isa-encode.h"

uint64_t
sgx_isa_encode(const struct sgx_isa_instr *instr,
                  const struct sgx_isa_fc *fc)
{
   struct sgx_isa_fc f = fc ? *fc : SGX_ISA_FC_START;
   struct encode_state s = {
      .gen = (f.c10 ? 1 : 0) | (f.f16 ? 2 : 0),
   };

   return bitmask_to_uint64_t(
      encode__instruction(&s, NULL, (struct sgx_isa_instr *)instr));
}
