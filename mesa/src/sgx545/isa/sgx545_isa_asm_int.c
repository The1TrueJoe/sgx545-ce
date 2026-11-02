/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/* Text notation of the integer / fixed-point / dual-issue classes. */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "sgx545_isa_asm.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static int
lookup(const char *const *names, unsigned n, const char *s)
{
   for (unsigned i = 0; i < n; i++) {
      if (names[i] && !strcmp(names[i], s))
         return i;
   }
   return -1;
}

#define LOOKUP(tbl, s) lookup(tbl, ARRAY_LEN(tbl), s)

/* A select: a name from the table, or "1-name" (one = 1-zero, zero = 1-one). */
static bool
parse_sel(const char *const *names, unsigned n, const char *s,
          uint8_t *v, bool *comp)
{
   int i;
   *comp = false;
   if (!strcmp(s, "one"))
      s = "1-zero";
   if (!strncmp(s, "1-", 2)) {
      *comp = true;
      s += 2;
   }
   if (!strcmp(s, "zero") || (s[0] == 'x' && s[1] >= '0' && s[1] <= '9')) {
      i = !strcmp(s, "zero") ? lookup(names, n, "zero") : atoi(s + 1);
      if (i < 0)
         return false;
   } else {
      i = lookup(names, n, s);
   }
   if (i < 0 || i >= (int)n)
      return false;
   *v = i;
   return true;
}

#define SEL(tbl, s, v, c) parse_sel(tbl, ARRAY_LEN(tbl), s, v, c)

static const char *const sop2_c1[] = { "zero", "s1", "s2", "s1a", "s2a", "asat", "k18", NULL };
static const char *const sop2_c2[] = { "zero", "s1", "s2", "s1a", "s2a", "asat", "k19", NULL };
static const char *const sop2_a1[] = { "zero", "s1a", "s2a", "k18" };
static const char *const sop2_a2[] = { "zero", "s1a", "s2a", "k19" };
static const char *const sopwm_c[] = { "zero", "asat", "s1", "s1a", NULL, NULL, "s2", "s2a" };
static const char *const sop3_c[] = { "zero", "asat", "s1", "s1a", "s0", "s0a", "s2", "s2a" };
static const char *const sop3_a[] = { "zero", "s0a", "s1a", "s2a" };
static const char *const sop3_a1[] = { "zero", "s1a" };
static const char *const sop3_a2[] = { "zero", "s2a" };
static const char *const lrp1_l1[] = { "zero", "s1" };
static const char *const lrp1_l2[] = { "zero", "s2" };
static const char *const lrp1_cs[] = { "s0", "s0a" };
static const char *const sop3x_p[] = { "s1.s2", "s1.1-s2", "one.s2", "1-s1.s2" };
static const char *const dot_a[] = { "zero", "s1a", "s2a", "x3" };
static const char *const dot_scale[] = { "x1", "x2", "x4", "x8" };
static const char *const ima8_c0[] = { "s0", "s1", "s0a", "s1a" };
static const char *const ima8_c1[] = { "s1", "s1a" };
static const char *const ima8_c2[] = { "s2", "s2a" };
static const char *const ima8_a0[] = { "s0a", "s1a" };
static const char *const ops4[] = { "add", "sub", "min", "max" };
static const char *const efo_d[] = { "i0", "i1", "a0", "a1" };
static const char *const efo_i[] = { "a0/a1", "a1/a0", "m0/m1", "a0/m1" };
static const char *const efo_a[] = { "m0+m1/i1+i0", "m0+s0/i1+i0", "m0+i0/i1+m1", "s0+s2/s1+s2" };
static const char *const efo_m[] = { "s0*s2/s1*s2", "s2*s1/s2*s2", "s1*s0/s2*s2", "s1*i0/s2*i1" };
static const char *const fmt8[] = { "u8", "s8", "o8", "x3" };
static const char *const imae_t2[] = { "u16", "s16", "u32", "x3" };
static const char *const ii01[] = { "i0", "i1" };
static const char *const fdual_pri[] = { "fmad", "mov", "fadm", "fmsa", "fddp", "fminmax", "ima32", "fssq" };
static const char *const fdual_sec[] = { "fmad", "fmad16", "fmsa", "fadm", "fmaxmin", "frcp", "frsq", "fexp2",
                                         "flog2", "fsin", "fcos", "fadd", "fsub", "fmul", "ima32", "mov" };
static const char *const fdual_sd[] = { "i0", "i1", "i2", "i0s0" };
static const char *const fdual_sb[] = { "i0", "i1", "i2", "src1" };
static const char *const fdual_sc[] = { "i0", "i1", "i2", "src2" };
static const char *const fdual_msrc[] = { "i0", "i1", "i2", "x3", "src0", "src1", "src2", "x7" };

static const struct {
   const char *name;
   enum sgx_isa_opc opc;
   const char *ops;
   uint8_t mask;
} forms[] = {
   { "efo", SGX_ISA_OPC_EFO, "D012" },
   { "sop2", SGX_ISA_OPC_SOP2, "D12" },
   { "sopwm", SGX_ISA_OPC_SOPWM, "D12", 15 },
   { "sop3", SGX_ISA_OPC_SOP3, "D012" },
   { "sop3l", SGX_ISA_OPC_SOP3L, "D012" },
   { "lrp1", SGX_ISA_OPC_LRP1, "D012" },
   { "sop3x", SGX_ISA_OPC_SOP3X, "D012" },
   { "dot3", SGX_ISA_OPC_DOT3, "D12" },
   { "dot4", SGX_ISA_OPC_DOT4, "D12" },
   { "dot3s", SGX_ISA_OPC_DOT3S, "D012" },
   { "dot4s", SGX_ISA_OPC_DOT4S, "D012" },
   { "ima8", SGX_ISA_OPC_IMA8, "D012" },
   { "fpma", SGX_ISA_OPC_FPMA, "D012" },
   { "ima16", SGX_ISA_OPC_IMA16, "D012" },
   { "imae", SGX_ISA_OPC_IMAE, "D012" },
   { "ima32", SGX_ISA_OPC_IMA32, "D012" },
   { "op1adrc", SGX_ISA_OPC_OP1ADRC, "D12" },
   { "adif", SGX_ISA_OPC_ADIF, "D12" },
   { "adifsum", SGX_ISA_OPC_ADIFSUM, "D012" },
   { "op16s1", SGX_ISA_OPC_OP16S1, "D012" },
   { "bilin", SGX_ISA_OPC_BILIN, "D012" },
   { "ssum16", SGX_ISA_OPC_SSUM16, "D012" },
   { "ssum16i", SGX_ISA_OPC_SSUM16I, "D02" },
   { "firv", SGX_ISA_OPC_FIRV, "D012" },
   { "firh", SGX_ISA_OPC_FIRH, "D012" },
};

bool
sgx545_isa_asm_mnemonic_int(struct sgx_isa_instr *I, char **parts,
                            unsigned nparts, unsigned nops, const char **pat)
{
   if (!strcmp(parts[0], "fdual")) {
      int p = nparts == 3 ? LOOKUP(fdual_pri, parts[1]) : -1;
      int s = nparts == 3 ? LOOKUP(fdual_sec, parts[2]) : -1;
      if (p < 0 || s < 0)
         return false;
      I->opc = SGX_ISA_OPC_FDUAL;
      I->pri = p;
      I->sec = s;
      *pat = "D012";
      return true;
   }

   for (unsigned i = 0; i < ARRAY_LEN(forms); i++) {
      if (strcmp(parts[0], forms[i].name))
         continue;
      if (nparts == 2 && !strcmp(parts[1], "sgn") &&
          (forms[i].opc == SGX_ISA_OPC_IMA16 || forms[i].opc == SGX_ISA_OPC_IMAE ||
           forms[i].opc == SGX_ISA_OPC_IMA32)) {
         I->sgn = true;
      } else if (nparts != 1) {
         return false;
      }
      I->opc = forms[i].opc;
      I->mask = forms[i].mask;
      *pat = forms[i].ops;
      return true;
   }
   return false;
}

static bool
set_sel(const char *const *tbl, unsigned n, const char *v, uint8_t *sel, bool *comp)
{
   return v && parse_sel(tbl, n, v, sel, comp);
}

#define SET_SEL(tbl, sel, comp) set_sel(tbl, ARRAY_LEN(tbl), value, &I->sel, &I->comp)

static bool
set_enum(const char *const *tbl, unsigned n, const char *v, uint8_t *out)
{
   int i = v ? lookup(tbl, n, v) : -1;
   if (i < 0)
      return false;
   *out = i;
   return true;
}

#define SET_ENUM(tbl, field) set_enum(tbl, ARRAY_LEN(tbl), value, &I->field)

static bool
set_num(const char *v, uint8_t *out)
{
   char *end;
   if (!v || !*v)
      return false;
   long n = strtol(v, &end, 0);
   if (*end)
      return false;
   *out = n;
   return true;
}

bool
sgx545_isa_asm_item_int(struct sgx_isa_instr *I, const char *name,
                        const char *value)
{
   enum sgx_isa_opc o = I->opc;
   bool sop2 = o == SGX_ISA_OPC_SOP2, sopwm = o == SGX_ISA_OPC_SOPWM;
   bool sop3 = o == SGX_ISA_OPC_SOP3 || o == SGX_ISA_OPC_SOP3L ||
               o == SGX_ISA_OPC_LRP1 || o == SGX_ISA_OPC_SOP3X;
   bool dot = o == SGX_ISA_OPC_DOT3 || o == SGX_ISA_OPC_DOT4 ||
              o == SGX_ISA_OPC_DOT3S || o == SGX_ISA_OPC_DOT4S;
   bool ima8 = o == SGX_ISA_OPC_IMA8 || o == SGX_ISA_OPC_FPMA;
   bool dummy;

   /* plain flags */
   static const struct { const char *name; size_t off; } flags[] = {
#define F(n) { #n, offsetof(struct sgx_isa_instr, n) }
      F(cs1c), F(as1c), F(aneg), F(arev), F(cneg), F(f4), F(f6), F(s1c), F(s2c),
      F(s1ac), F(sat), F(dflag1), F(s0hi), F(s1hi), F(s2hi), F(cout), F(negp),
      F(ileave), F(src23), F(dst23), F(x1b), F(x1c), F(x1d), F(nf16), F(s8),
      F(x38), F(s0f13), F(f2_27), F(wi0), F(wi1), F(a0lneg), F(a0rneg), F(f3_10),
#undef F
   };
   if (!value) {
      for (unsigned i = 0; i < ARRAY_LEN(flags); i++) {
         if (!strcmp(name, flags[i].name)) {
            *(bool *)((char *)I + flags[i].off) = true;
            return true;
         }
      }
      if (!strcmp(name, "cin") && o == SGX_ISA_OPC_IMAE)
         return I->cin = true;
      if (!strcmp(name, "rnd") && o == SGX_ISA_OPC_BILIN)
         return I->irnd = true;
      if (!strcmp(name, "a1c") && ima8)
         return I->a1c = true;
      if (!strcmp(name, "a2c") && ima8)
         return I->a2c = true;
      return false;
   }

   if (!strcmp(name, "c1")) {
      if (sop2) return SET_SEL(sop2_c1, c1, c1c);
      if (sopwm) return SET_SEL(sopwm_c, c1, c1c);
      if (sop3) return SET_SEL(sop3_c, c1, c1c);
      if (ima8) return SET_SEL(ima8_c1, c1, c1c);
   }
   if (!strcmp(name, "c2")) {
      if (sop2) return SET_SEL(sop2_c2, c2, c2c);
      if (sopwm) return SET_SEL(sopwm_c, c2, c2c);
      if (sop3) return SET_SEL(sop3_c, c2, c2c);
      if (ima8) return SET_SEL(ima8_c2, c2, c2c);
   }
   if (!strcmp(name, "c0") && ima8)
      return SET_SEL(ima8_c0, c0, c0c);
   if (!strcmp(name, "a0") && ima8)
      return SET_SEL(ima8_a0, a0, a0c);
   if (!strcmp(name, "a1")) {
      if (sop2) return SET_SEL(sop2_a1, a1, a1c);
      if (sop3) return SET_SEL(sop3_a1, a1, a1c);
      if (dot) return SET_SEL(dot_a, a1, a1c);
   }
   if (!strcmp(name, "a2")) {
      if (sop2) return SET_SEL(sop2_a2, a2, a2c);
      if (sop3) return SET_SEL(sop3_a2, a2, a2c);
      if (dot) return SET_SEL(dot_a, a2, a2c);
   }
   if (!strcmp(name, "a") && sop3)
      return SET_SEL(sop3_a, asel, aselc);
   if (!strcmp(name, "l1"))
      return SET_SEL(lrp1_l1, l1, l1c);
   if (!strcmp(name, "l2"))
      return SET_SEL(lrp1_l2, l2, l2c);
   if (!strcmp(name, "cs")) {
      if (o == SGX_ISA_OPC_LRP1) return SET_ENUM(lrp1_cs, lcs);
      if (dot) return SET_ENUM(dot_scale, dcs);
   }
   if (!strcmp(name, "as") && dot)
      return SET_ENUM(dot_scale, das);
   if (!strcmp(name, "p") && o == SGX_ISA_OPC_SOP3X)
      return SET_ENUM(sop3x_p, p);
   if (!strcmp(name, "cop"))
      return SET_ENUM(ops4, cop);
   if (!strcmp(name, "aop"))
      return SET_ENUM(ops4, aop);
   if (!strcmp(name, "d") && o == SGX_ISA_OPC_EFO)
      return SET_ENUM(efo_d, efo_d);
   if (!strcmp(name, "i") && o == SGX_ISA_OPC_EFO)
      return SET_ENUM(efo_i, efo_i);
   if (!strcmp(name, "a") && o == SGX_ISA_OPC_EFO)
      return SET_ENUM(efo_a, efo_a);
   if (!strcmp(name, "m") && o == SGX_ISA_OPC_EFO)
      return SET_ENUM(efo_m, efo_m);
   if (!strcmp(name, "f1"))
      return set_enum(fmt8, 3, value, &I->f1) && ++I->f1;
   if (!strcmp(name, "f2"))
      return set_enum(fmt8, 3, value, &I->f2) && ++I->f2;
   if (!strcmp(name, "t2"))
      return SET_ENUM(imae_t2, t2);
   if (!strcmp(name, "ci")) {
      uint8_t v;
      if (!SET_ENUM(ii01, ii))
         return false;
      v = I->ii;
      I->ii = 0;
      if (o == SGX_ISA_OPC_OP16S1)
         I->ii = v;
      else
         I->ci = v;
      return true;
   }
   if (!strcmp(name, "hi") && o == SGX_ISA_OPC_IMA32) {
      static const char *const hi[] = { NULL, NULL, NULL, NULL, "i0", "i1", "i2" };
      return SET_ENUM(hi, hi) || set_num(value, &I->hi);
   }
   if (!strcmp(name, "cin") && o == SGX_ISA_OPC_IMA32) {
      static const char *const cin[] = { NULL, "i0", "i1", "i2" };
      return SET_ENUM(cin, cin3);
   }
   if (!strcmp(name, "fmt") && o == SGX_ISA_OPC_SSUM16) {
      /* ssum16's format picks the form (fmt 3 is ssum16i) */
      static const enum sgx_isa_opc f[] = { SGX_ISA_OPC_SSUM16, SGX_ISA_OPC_SSUM16_S8,
                                            SGX_ISA_OPC_SSUM16_O8 };
      if (!set_enum(fmt8, 3, value, &I->ifmt))
         return false;
      I->opc = f[I->ifmt];
      return true;
   }
   if (!strcmp(name, "fmt"))
      return SET_ENUM(fmt8, ifmt);
   if (!strcmp(name, "sd"))
      return SET_ENUM(fdual_sd, sd);
   if (!strcmp(name, "sb"))
      return SET_ENUM(fdual_sb, sb);
   if (!strcmp(name, "sc"))
      return SET_ENUM(fdual_sc, sc);
   if (!strcmp(name, "msrc"))
      return SET_ENUM(fdual_msrc, msrc);
   if (!strcmp(name, "f2_15"))
      return set_num(value, &I->f2_15);
   if (!strcmp(name, "x39"))
      return set_num(value, &I->x39);
   if (!strcmp(name, "a5"))
      return set_num(value, &I->a5);
   if (!strcmp(name, "coef"))
      return set_num(value, &I->coef);
   if (!strcmp(name, "shift"))
      return set_num(value, &I->shift);
   if (!strcmp(name, "a8"))
      return set_num(value, &I->a8);
   (void)dummy;
   return false;
}

bool
sgx545_isa_asm_pred2_int(enum sgx_isa_opc opc)
{
   switch (opc) {
   case SGX_ISA_OPC_EFO: case SGX_ISA_OPC_FDUAL: case SGX_ISA_OPC_SOP2:
   case SGX_ISA_OPC_SOPWM: case SGX_ISA_OPC_SOP3: case SGX_ISA_OPC_SOP3L:
   case SGX_ISA_OPC_LRP1: case SGX_ISA_OPC_SOP3X: case SGX_ISA_OPC_DOT3:
   case SGX_ISA_OPC_DOT4: case SGX_ISA_OPC_DOT3S: case SGX_ISA_OPC_DOT4S:
   case SGX_ISA_OPC_IMA8: case SGX_ISA_OPC_FPMA: case SGX_ISA_OPC_IMA16:
   case SGX_ISA_OPC_IMAE: case SGX_ISA_OPC_IMA32: case SGX_ISA_OPC_OP1ADRC:
   case SGX_ISA_OPC_ADIF: case SGX_ISA_OPC_ADIFSUM: case SGX_ISA_OPC_OP16S1:
   case SGX_ISA_OPC_BILIN: case SGX_ISA_OPC_SSUM16: case SGX_ISA_OPC_SSUM16_S8:
   case SGX_ISA_OPC_SSUM16_O8: case SGX_ISA_OPC_SSUM16I: case SGX_ISA_OPC_FIRV:
   case SGX_ISA_OPC_FIRH:
      return true;
   default:
      return false;
   }
}
