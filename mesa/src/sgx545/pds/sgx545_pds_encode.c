/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * Glue for the encoder isaspec generates from sgx545_pds.xml: the leaf
 * selection of each hierarchy. Bit positions live in the XML.
 */

#include "sgx545_pds.h"

#include "util/macros.h"

struct encode_state {
   uint32_t gen;
};

static inline enum sgx545_pds_opc
__instruction_case(struct encode_state *s, const struct sgx545_pds_instr *instr)
{
   return instr->opc;
}

static inline enum sgx545_pds_case_u
__u_case(struct encode_state *s, const struct sgx545_pds_reg *r)
{
   switch (r->file) {
   case SGX545_PDS_FILE_DS0:
      return SGX545_PDS_U_DS0;
   case SGX545_PDS_FILE_DS1:
      return SGX545_PDS_U_DS1;
   default:
      return SGX545_PDS_U_IN;
   }
}

static inline enum sgx545_pds_case_ud
__ud_case(struct encode_state *s, const struct sgx545_pds_reg *r)
{
   return r->file == SGX545_PDS_FILE_DS1 ? SGX545_PDS_UD_DS1 : SGX545_PDS_UD_DS0;
}

static inline enum sgx545_pds_case_s1
__s1_case(struct encode_state *s, const struct sgx545_pds_reg *r)
{
   return r->file == SGX545_PDS_FILE_IN ? SGX545_PDS_S1_IN : SGX545_PDS_S1_DS0;
}

static inline enum sgx545_pds_case_s2
__s2_case(struct encode_state *s, const struct sgx545_pds_reg *r)
{
   return SGX545_PDS_S2_DS1;
}

#include "sgx545pds-isa-encode.h"

uint32_t
sgx545_pds_encode(const struct sgx545_pds_instr *instr)
{
   struct encode_state s = {0};

   return bitmask_to_uint64_t(encode__instruction(&s, NULL, (struct sgx545_pds_instr *)instr));
}

#define DATA_WORD(fn, root, type)                                                                  \
   uint32_t fn(const struct type *v)                                                               \
   {                                                                                               \
      struct encode_state s = {0};                                                                 \
      return bitmask_to_uint64_t(encode__##root(&s, NULL, (struct type *)v));                      \
   }

DATA_WORD(sgx545_pds_doutu_w0, doutu_w0, sgx545_pds_doutu)
DATA_WORD(sgx545_pds_doutu_w1, doutu_w1, sgx545_pds_doutu)
DATA_WORD(sgx545_pds_doutd_ctrl, doutd_ctrl, sgx545_pds_doutd)
DATA_WORD(sgx545_pds_douti_w0, douti, sgx545_pds_douti)
DATA_WORD(sgx545_pds_douti_w1, douti_w1, sgx545_pds_douti)
DATA_WORD(sgx545_pds_douta_ctrl, douta_ctrl, sgx545_pds_douta)
