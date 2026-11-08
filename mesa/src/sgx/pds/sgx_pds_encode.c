/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * Glue for the encoder isaspec generates from sgx_pds.xml: the leaf
 * selection of each hierarchy. Bit positions live in the XML.
 */

#include "sgx_pds.h"

#include "util/macros.h"

struct encode_state {
   uint32_t gen;
};

static inline enum sgx_pds_opc
__instruction_case(struct encode_state *s, const struct sgx_pds_instr *instr)
{
   return instr->opc;
}

static inline enum sgx_pds_case_u
__u_case(struct encode_state *s, const struct sgx_pds_reg *r)
{
   switch (r->file) {
   case SGX_PDS_FILE_DS0:
      return SGX_PDS_U_DS0;
   case SGX_PDS_FILE_DS1:
      return SGX_PDS_U_DS1;
   default:
      return SGX_PDS_U_IN;
   }
}

static inline enum sgx_pds_case_ud
__ud_case(struct encode_state *s, const struct sgx_pds_reg *r)
{
   return r->file == SGX_PDS_FILE_DS1 ? SGX_PDS_UD_DS1
                                         : SGX_PDS_UD_DS0;
}

static inline enum sgx_pds_case_s1
__s1_case(struct encode_state *s, const struct sgx_pds_reg *r)
{
   return r->file == SGX_PDS_FILE_IN ? SGX_PDS_S1_IN : SGX_PDS_S1_DS0;
}

static inline enum sgx_pds_case_s2
__s2_case(struct encode_state *s, const struct sgx_pds_reg *r)
{
   return SGX_PDS_S2_DS1;
}

#include "sgxpds-isa-encode.h"

uint32_t
sgx_pds_encode(const struct sgx_pds_instr *instr)
{
   struct encode_state s = {0};

   return bitmask_to_uint64_t(
      encode__instruction(&s, NULL, (struct sgx_pds_instr *)instr));
}

#define DATA_WORD(fn, root, type)                                              \
   uint32_t fn(const struct type *v)                                           \
   {                                                                           \
      struct encode_state s = {0};                                             \
      return bitmask_to_uint64_t(encode__##root(&s, NULL, (struct type *)v));  \
   }

DATA_WORD(sgx_pds_doutu_w0, doutu_w0, sgx_pds_doutu)
DATA_WORD(sgx_pds_doutu_w1, doutu_w1, sgx_pds_doutu)
DATA_WORD(sgx_pds_doutd_ctrl, doutd_ctrl, sgx_pds_doutd)
DATA_WORD(sgx_pds_douti_w0, douti, sgx_pds_douti)
DATA_WORD(sgx_pds_douti_w1, douti_w1, sgx_pds_douti)
DATA_WORD(sgx_pds_douta_ctrl, douta_ctrl, sgx_pds_douta)
