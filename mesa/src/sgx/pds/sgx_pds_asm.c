/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * The disassembler's text back into struct sgx_pds_instr (tests, and
 * programs written by hand):
 *
 *    [(p0)] <op>[.x2][.s27][.cc<n>] <operands> [r1=n] [r2=n]
 *
 * Words: ds0[n], ds1[n], in<n>, with .hi for the high half of S1/S2
 * operands; immediates #n.
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "sgx_pds.h"

struct cursor {
   const char *p;
};

static void
skip(struct cursor *c)
{
   while (isspace((unsigned char)*c->p) || *c->p == ',')
      c->p++;
}

static bool
eat(struct cursor *c, const char *s)
{
   skip(c);
   size_t n = strlen(s);
   if (strncmp(c->p, s, n))
      return false;
   c->p += n;
   return true;
}

static bool
number(struct cursor *c, unsigned long *v)
{
   char *end;

   skip(c);
   *v = strtoul(c->p, &end, 0);
   if (end == c->p)
      return false;
   c->p = end;
   return true;
}

/* ds0[n] / ds1[n] / in<n>, [.hi] */
static bool
word(struct cursor *c, struct sgx_pds_reg *r)
{
   unsigned long n;

   memset(r, 0, sizeof(*r));
   if (eat(c, "ds0[") || eat(c, "ds1[")) {
      r->file = c->p[-2] == '0' ? SGX_PDS_FILE_DS0 : SGX_PDS_FILE_DS1;
      if (!number(c, &n) || !eat(c, "]"))
         return false;
   } else if (eat(c, "in")) {
      r->file = SGX_PDS_FILE_IN;
      if (!number(c, &n))
         return false;
   } else {
      return false;
   }
   r->num = n;
   if (!strncmp(c->p, ".hi", 3)) {
      r->hi = true;
      c->p += 3;
   }
   return true;
}

static const struct {
   const char *name;
   enum sgx_pds_opc opc;
} ops[] = {
   {"op0xd.s1", SGX_PDS_OPC_OP0XD_S1}, {"op0x4.s1", SGX_PDS_OPC_OP0X4_S1},
   {"op0x6.s1", SGX_PDS_OPC_OP0X6_S1}, {"add", SGX_PDS_OPC_ADD},
   {"mul16", SGX_PDS_OPC_MUL16},       {"or", SGX_PDS_OPC_OR},
   {"and", SGX_PDS_OPC_AND},           {"xor", SGX_PDS_OPC_XOR},
   {"tst", SGX_PDS_OPC_TST},           {"bra", SGX_PDS_OPC_BRA},
   {"shl", SGX_PDS_OPC_SHL},           {"shr", SGX_PDS_OPC_SHR},
   {"halt", SGX_PDS_OPC_HALT},
};

static const char *const douts[] = {
   [SGX_PDS_DOUTI] = "douti", [SGX_PDS_DOUTD] = "doutd",
   [SGX_PDS_DOUTT] = "doutt", [SGX_PDS_DOUTU] = "doutu",
   [SGX_PDS_DOUTA] = "douta",
};

static const char *
movs(struct cursor *c, struct sgx_pds_instr *in)
{
   bool r1_set = false, r2_set = false;

   for (unsigned k = 0; k < 4; k++) {
      struct sgx_pds_reg r;

      if (!word(c, &r) || r.hi)
         return "expected a data word";
      unsigned w = r.file == SGX_PDS_FILE_IN ? 0x40 + r.num : r.num;
      bool s2 = r.file == SGX_PDS_FILE_DS1;
      uint8_t *row = s2 ? &in->r2 : &in->r1;
      bool *set = s2 ? &r2_set : &r1_set;

      if (*set && *row != w >> 1)
         return "words from two different rows of one store";
      *row = w >> 1;
      *set = true;
      in->sel[k] = (s2 ? 2 : 0) | (w & 1);
   }
   for (;;) {
      unsigned long n;

      if (eat(c, "[r1=") && number(c, &n) && eat(c, "]"))
         in->r1 = n;
      else if (eat(c, "[r2=") && number(c, &n) && eat(c, "]"))
         in->r2 = n;
      else
         break;
   }
   return NULL;
}

static const char *operands(struct cursor *c, struct sgx_pds_instr *in);

const char *
sgx_pds_asm(const char *text, struct sgx_pds_instr *in)
{
   struct cursor c = {text};

   memset(in, 0, sizeof(*in));
   in->cc = SGX_PDS_CC_ALWAYS;
   if (eat(&c, "(p0)"))
      in->cc = SGX_PDS_CC_P0;
   skip(&c);

   size_t len = 0;
   while (c.p[len] && !isspace((unsigned char)c.p[len]))
      len++;
   char op[32];
   if (!len || len >= sizeof(op))
      return "no mnemonic";
   memcpy(op, c.p, len);
   op[len] = '\0';
   c.p += len;

   /* the mnemonic, then its suffixes */
   char *sfx = NULL;
   in->opc = SGX_PDS_OPC_NONE;
   for (unsigned i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
      size_t l = strlen(ops[i].name);
      if (!strncmp(op, ops[i].name, l) && (op[l] == '\0' || op[l] == '.')) {
         in->opc = ops[i].opc;
         sfx = op + l;
         break;
      }
   }
   if (in->opc == SGX_PDS_OPC_NONE && !strncmp(op, "dout", 4)) {
      in->opc = SGX_PDS_OPC_MOVS;
      sfx = op + 4;
      while (*sfx && *sfx != '.')
         sfx++;
      char save = *sfx;
      *sfx = '\0';
      in->dest = 0xff;
      for (unsigned d = 0; d < sizeof(douts) / sizeof(douts[0]); d++) {
         if (douts[d] && !strcmp(op, douts[d]))
            in->dest = d;
      }
      if (in->dest == 0xff)
         in->dest = strtoul(op + 4, NULL, 10);
      *sfx = save;
   }
   if (in->opc == SGX_PDS_OPC_NONE)
      return "unknown mnemonic";
   while (*sfx == '.') {
      sfx++;
      if (!strncmp(sfx, "x2", 2) && in->opc == SGX_PDS_OPC_MOVS) {
         in->x2 = true;
         sfx += 2;
      } else if (!strncmp(sfx, "s27", 3) && in->opc == SGX_PDS_OPC_MOVS) {
         in->s27 = true;
         sfx += 3;
      } else if (!strncmp(sfx, "cc", 2) && sfx[2] >= '1' && sfx[2] <= '6') {
         in->cc = sfx[2] - '0';
         sfx += 3;
      } else {
         return "unknown suffix";
      }
   }
   if (*sfx)
      return "unknown suffix";

   const char *err = operands(&c, in);
   if (err)
      return err;
   skip(&c);
   return *c.p ? "trailing text" : NULL;
}

static const char *
operands(struct cursor *c, struct sgx_pds_instr *in)
{
   unsigned long n;

   switch (in->opc) {
   case SGX_PDS_OPC_MOVS:
      if (in->dest > 31)
         return "bad DOUT target";
      return movs(c, in);
   case SGX_PDS_OPC_ADD:
   case SGX_PDS_OPC_MUL16:
   case SGX_PDS_OPC_OR:
   case SGX_PDS_OPC_AND:
   case SGX_PDS_OPC_XOR:
   case SGX_PDS_OPC_OP0XD_S1:
   case SGX_PDS_OPC_OP0X4_S1:
   case SGX_PDS_OPC_OP0X6_S1:
      if (!word(c, &in->dst) || in->dst.file == SGX_PDS_FILE_IN ||
          in->dst.hi)
         return "bad destination";
      if (!word(c, &in->src1) || in->src1.file == SGX_PDS_FILE_DS1)
         return "src1 is ds0 or an input";
      if (!word(c, &in->src2) || in->src2.file != SGX_PDS_FILE_DS1)
         return "src2 is ds1";
      return NULL;
   case SGX_PDS_OPC_TST:
      if (!word(c, &in->src1))
         return "bad source";
      if (in->src1.file == SGX_PDS_FILE_DS1) {
         in->opc = SGX_PDS_OPC_TST_S2;
         in->src2 = in->src1;
         in->src1 = (struct sgx_pds_reg){0};
      }
      return NULL;
   case SGX_PDS_OPC_BRA:
      if (!eat(c, "#") || !number(c, &n))
         return "expected #target";
      in->target = n;
      return NULL;
   case SGX_PDS_OPC_SHL:
   case SGX_PDS_OPC_SHR:
      if (!word(c, &in->dst) || in->dst.hi || !word(c, &in->src1) ||
          in->src1.hi)
         return "bad operands";
      if (!eat(c, "#") || !number(c, &n))
         return "expected #amount";
      in->amount = n;
      return NULL;
   case SGX_PDS_OPC_HALT:
      return NULL;
   default:
      return "unknown mnemonic";
   }
}
