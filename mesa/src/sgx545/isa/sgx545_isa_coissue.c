/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * What a compiler needs to pair operations with efo and fdual: which
 * internal registers and slots each half reads and writes, and the same-word
 * hazards (ISA.md "fdual for a compiler", "efo").
 */

#include "sgx545_isa.h"

#define SLOT(k) (1u << (k))

/* an operand as an internal register: i<n>, or r124-r127 (the alias) */
static int
internal(const struct sgx_isa_src *o)
{
   if (o->file == SGX_ISA_FILE_I)
      return o->num;
   if (o->file == SGX_ISA_FILE_R && o->num >= 124 && o->num <= 127)
      return o->num - 124;
   return -1;
}

static void
read_sel(struct sgx545_isa_fdual_use *u, unsigned sel, unsigned slot)
{
   /* a select: i0-i2, or 3 = the slot */
   if (sel < 3)
      u->sec_reads_i |= 1u << sel;
   else
      u->sec_reads_slots |= SLOT(slot);
}

bool
sgx545_isa_fdual_use(const struct sgx_isa_instr *instr, struct sgx545_isa_fdual_use *u)
{
   unsigned sd = instr->sd & 3;

   *u = (struct sgx545_isa_fdual_use){.settled = true};
   if (instr->opc != SGX_ISA_OPC_FDUAL)
      return false;

   /* the secondary writes the internal register sd names (i0s0: i0) */
   u->sec_writes_i = 1u << (sd == 3 ? 0 : sd);

   switch (instr->sec) {
   case 0: /* fmad: sd * sb + sc */
   case 2: /* fmsa: sd * sd + sb * sc */
   case 4: /* fmaxmin: sb and sc, and maybe sd (not drawn yet) */
      if (sd == 3)
         u->sec_reads_slots |= SLOT(0);
      else
         u->sec_reads_i |= 1u << sd;
      read_sel(u, instr->sb, 1);
      read_sel(u, instr->sc, 2);
      u->settled = instr->sec != 4;
      break;
   case 3: /* fsubflr: sb - floor(sc) */
   case 11:
   case 12:
   case 13: /* fadd, fsub, fmul: sb op sc */
      read_sel(u, instr->sb, 1);
      read_sel(u, instr->sc, 2);
      break;
   case 5:
   case 6:
   case 7:
   case 8:
   case 9:
   case 10: /* op(sb) */
      read_sel(u, instr->sb, 1);
      break;
   case 15: /* mov: msrc (0-2 i0-i2, 4-6 the src0-src2 slot) */
      if (instr->msrc < 3)
         u->sec_reads_i |= 1u << instr->msrc;
      else if (instr->msrc >= 4 && instr->msrc <= 6)
         u->sec_reads_slots |= SLOT(instr->msrc - 4);
      else
         u->settled = false;
      break;
   default: /* fmad16, ima32: F16 / integer meaning not drawn */
      if (sd == 3)
         u->sec_reads_slots |= SLOT(0);
      else
         u->sec_reads_i |= 1u << sd;
      read_sel(u, instr->sb, 1);
      read_sel(u, instr->sc, 2);
      u->settled = false;
      break;
   }

   switch (instr->pri) {
   case 1: /* mov: src1 */
      u->pri_reads_slots = SLOT(1);
      break;
   case 7: /* fssq: src1^2 + src2^2 */
      u->pri_reads_slots = SLOT(1) | SLOT(2);
      break;
   case 4: /* fddp: also leaves src0 * src2 in an internal register (i0
            * seen; whether it follows sd is not drawn) */
      u->pri_reads_slots = SLOT(0) | SLOT(1) | SLOT(2);
      u->pri_writes_i = 1u << 0;
      u->settled = false;
      break;
   default: /* fmad, fadm, fmsa, fminmax, ima32: all three slots */
      u->pri_reads_slots = SLOT(0) | SLOT(1) | SLOT(2);
      u->settled &= instr->pri != 6;
      break;
   }
   for (unsigned k = 0; k < 3; k++) {
      int i = internal(&instr->src[k]);
      if ((u->pri_reads_slots & SLOT(k)) && i >= 0)
         u->pri_reads_i |= 1u << i;
   }
   int d = internal(&instr->dst);
   if (d >= 0)
      u->pri_writes_i |= 1u << d;
   return true;
}

const char *
sgx545_isa_coissue_hazard(const struct sgx_isa_instr *instr)
{
   if (instr->opc == SGX_ISA_OPC_FDUAL) {
      struct sgx545_isa_fdual_use u;

      sgx545_isa_fdual_use(instr, &u);
      /* the secondary reads before the primary writes (fine); the primary
       * reading what the secondary writes is a race */
      if (u.pri_reads_i & u.sec_writes_i)
         return "the primary reads an internal register the secondary writes in the same word";
      return NULL;
   }
   if (instr->opc == SGX_ISA_OPC_EFO) {
      /* d naming an internal register the word writes is a race */
      if ((instr->efo_d == 0 && instr->wi0) || (instr->efo_d == 1 && instr->wi1))
         return "efo's d names an internal register the same word writes";
      return NULL;
   }
   return NULL;
}
