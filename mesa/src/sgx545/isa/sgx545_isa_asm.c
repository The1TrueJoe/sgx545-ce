/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * A small assembler for ISA.md's text notation, into struct sgx_isa_instr:
 *
 *    [(pred)] mnemonic[.suffix...] operand, operand, ... [item item=value ...]
 *
 * It only knows the notation (names, operand order, defaults); the bits
 * come from the encoder generated from sgx545.xml. Used by the tests and
 * by tools that want to write USSE by hand.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sgx545_isa_asm.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

struct asm_ctx {
   char *err;
   size_t errsize;
};

static bool
fail(struct asm_ctx *c, const char *fmt, const char *arg)
{
   if (c->err && c->errsize) {
      snprintf(c->err, c->errsize, fmt, arg);
   }
   return false;
}

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

static const char *const pck_fmt[] = { "u8", "s8", "o8", "u16", "s16", "f16", "f32", "c10" };
static const char *const movc_cond[] = { "z", "nz", "ltz", "gez" };
static const char *const movc_type[] = { NULL, "u8", "i16", "i32", "f32", "c10" };
static const char *const smp_dim[] = { "1d", "2d", "3d", "dim3" };
static const char *const smp_lod[] = { NULL, "bias", "lod", "grad" };
static const char *const smp_cfmt[] = { NULL, "f16", "c10", "cfmt3" };
static const char *const smp_pcf[] = { NULL, "pcf", "pcf16", "pcf3" };
static const char *const smp_ret[] = { NULL, "rdata", "rinfo", "ret3" };
static const char *const ld_am[] = { "a", "l", "t", "am3" };
static const char *const ld_size[] = { "d", "w", "b", "size3" };
static const char *const tm_type[] = { "byte", "word", "dword", "type3" };
static const char *const emit_tgt[] = { "t0", "t1", "t2", "t3" };
static const char *const rnd_names[] = { "ne", "down", "up", "zero" };
static const char *const f16swz[] = { NULL, "ll", "hh", "f32" };

static const char *const test_alu[64] = {
   [0x00] = "fadd", [0x03] = "fsubflr", [0x04] = "frcp", [0x05] = "frsq",
   [0x06] = "flog2", [0x07] = "fexp2", [0x08] = "fdp", [0x09] = "fmin",
   [0x0a] = "fmax", [0x0b] = "fdsx", [0x0c] = "fdsy", [0x0d] = "fmul",
   [0x0e] = "fsub", [0x16] = "iadd16", [0x17] = "isub16", [0x18] = "imul16",
   [0x19] = "iaddu16", [0x1a] = "isubu16", [0x1b] = "imulu16",
   [0x1c] = "iadd32", [0x1d] = "iaddu32", [0x20] = "i8op0", [0x21] = "i8op1",
   [0x22] = "i8op2", [0x23] = "i8op3", [0x24] = "i8op4", [0x25] = "i8op5",
   [0x26] = "i8op6", [0x27] = "fpadd8", [0x28] = "fpsub8", [0x30] = "and",
   [0x31] = "or", [0x32] = "xor", [0x33] = "shl", [0x34] = "shr",
   [0x35] = "rol", [0x37] = "asr",
};

static int
parse_test_alu(const char *s)
{
   int v = LOOKUP(test_alu, s);
   if (v < 0 && !strncmp(s, "alu", 3) && strlen(s) == 5)
      v = strtol(s + 3, NULL, 16);
   return v;
}

static int
parse_test_cond(const char *s)
{
   static const struct { const char *name; int v; } named[] = {
      { "lt", 0x19 }, { "le", 0x05 }, { "eq", 0x14 }, { "ne", 0x18 },
      { "gt", 0x1a }, { "ge", 0x06 }, { "neg", 0x11 }, { "pos", 0x12 },
   };
   for (unsigned i = 0; i < ARRAY_LEN(named); i++) {
      if (!strcmp(s, named[i].name))
         return named[i].v;
   }
   /* s<sign>z<zero><o|a> */
   if (strlen(s) == 5 && s[0] == 's' && s[2] == 'z' && (s[4] == 'o' || s[4] == 'a'))
      return (s[1] - '0') | (s[3] - '0') << 2 | (s[4] == 'a') << 4;
   return -1;
}

static char *
trim(char *s)
{
   while (isspace((unsigned char)*s))
      s++;
   char *e = s + strlen(s);
   while (e > s && isspace((unsigned char)e[-1]))
      *--e = '\0';
   return s;
}

static bool
parse_int(const char *s, long *v)
{
   char *end;
   if (!*s)
      return false;
   *v = strtol(s, &end, 0);
   return *end == '\0';
}

/* Register-like prefixes, longest first. */
static const struct { const char *prefix; uint8_t file; } files[] = {
   { "idx", SGX_ISA_FILE_IDX }, { "pa", SGX_ISA_FILE_PA }, { "sa", SGX_ISA_FILE_SA },
   { "r", SGX_ISA_FILE_R }, { "o", SGX_ISA_FILE_O }, { "c", SGX_ISA_FILE_C },
   { "g", SGX_ISA_FILE_G }, { "i", SGX_ISA_FILE_I },
};

static bool
parse_operand(struct asm_ctx *c, char *s, struct sgx_isa_src *o)
{
   memset(o, 0, sizeof(*o));
   s = trim(s);
   if (*s == '-') {
      o->neg = true;
      s = trim(s + 1);
   }
   if (*s == '|') {
      size_t l = strlen(s);
      if (l < 2 || s[l - 1] != '|')
         return fail(c, "bad |abs| in '%s'", s);
      o->abs = true;
      s[l - 1] = '\0';
      s++;
   }

   /* suffixes: .f16[.lo|.hi], .c10, .l<n> */
   char *dot = strchr(s, '.');
   char *base = s;
   if (*s == '#') {
      /* an immediate's suffixes come after its digits */
      dot = strchr(s, '.');
   }
   char *sfx = NULL;
   if (dot) {
      *dot = '\0';
      sfx = dot + 1;
   }
   while (sfx && *sfx) {
      char *next = strchr(sfx, '.');
      if (next)
         *next++ = '\0';
      if (!strcmp(sfx, "f16"))
         o->fmt = SGX_ISA_FMT_F16;
      else if (!strcmp(sfx, "c10"))
         o->fmt = SGX_ISA_FMT_C10;
      else if (!strcmp(sfx, "lo"))
         o->half = 0;
      else if (!strcmp(sfx, "hi"))
         o->half = 1;
      else if (sfx[0] == 'l' && sfx[1] >= '0' && sfx[1] <= '3' && !sfx[2])
         o->lane = sfx[1] - '0';
      else
         return fail(c, "unknown operand suffix '%s'", sfx);
      sfx = next;
   }

   long v;
   if (*base == '#') {
      if (!parse_int(base + 1, &v))
         return fail(c, "bad immediate '%s'", base);
      o->file = SGX_ISA_FILE_IMM;
      o->num = (uint32_t)v;
      return true;
   }

   char *br = strchr(base, '[');
   if (br) {
      /* bank[idx<k>+off] */
      *br = '\0';
      static const char *const ib[] = { "r", "o", "pa", "sa" };
      int b = LOOKUP(ib, base);
      unsigned k, off = 0;
      if (b < 0 || sscanf(br + 1, "idx%u+%u]", &k, &off) < 1)
         return fail(c, "bad indexed operand '%s'", base);
      o->file = SGX_ISA_FILE_IX;
      o->ixbank = b;
      o->ixreg = k;
      o->num = off;
      return true;
   }

   for (unsigned i = 0; i < ARRAY_LEN(files); i++) {
      size_t l = strlen(files[i].prefix);
      if (!strncmp(base, files[i].prefix, l) && isdigit((unsigned char)base[l]) &&
          parse_int(base + l, &v)) {
         o->file = files[i].file;
         o->num = (uint32_t)v;
         return true;
      }
   }
   return fail(c, "bad operand '%s'", base);
}

static int
parse_pn(const char *s)
{
   if (s[0] == 'p' && s[1] >= '0' && s[1] <= '3' && (!s[2] || s[2] == '.'))
      return s[1] - '0';
   return -1;
}

static bool
parse_hash(struct asm_ctx *c, const char *s, long *v)
{
   s = trim((char *)s);
   if (*s != '#' || !parse_int(s + 1, v))
      return fail(c, "expected #value, got '%s'", s);
   return true;
}

static bool
parse_pred(const char *s, bool short_form, uint8_t *pred)
{
   static const char *const p3[] = { NULL, "p0", "p1", "p2", "p3", "!p0", "!p1", "pn" };
   static const char *const p2[] = { NULL, "p0", "p1", "!p0" };
   int v = short_form ? LOOKUP(p2, s) : LOOKUP(p3, s);
   if (v < 0)
      return false;
   *pred = v;
   return true;
}

/* Bracket items: "name" or "name=value". */
static bool
parse_item(struct asm_ctx *c, struct sgx_isa_instr *I, char *item)
{
   char *eq = strchr(item, '=');
   const char *val = NULL;
   long v = 0;
   if (eq) {
      *eq = '\0';
      val = eq + 1;
      if (!parse_int(val, &v))
         v = -1;
   }

#define FLAG(name, field) if (!strcmp(item, name) && !eq) { I->field = true; return true; }
#define VAL(name, field)  if (!strcmp(item, name) && eq && v >= 0) { I->field = v; return true; }
   FLAG("skipinv", skipinv) FLAG("nosched", nosched) FLAG("syncs", syncs)
   FLAG("end", end) FLAG("fmt", fmt) FLAG("moe", moe)
   FLAG("inc0", inc[0]) FLAG("inc1", inc[1]) FLAG("inc2", inc[2])
   FLAG("f16d", f16d) FLAG("f1_1", f1_1) FLAG("f1_26", f1_26) FLAG("f2_0", f2_0)
   FLAG("partial", partial) FLAG("preinc", preinc) FLAG("postinc", postinc)
   FLAG("range", range) FLAG("f2_2", f2_2) FLAG("f2_3", f2_3) FLAG("f2_9", f2_9)
   FLAG("f3_13", f3_13) FLAG("syncend", syncend) FLAG("xsyncend", xsyncend)
   FLAG("link", link) FLAG("f1_22", f1_22) FLAG("f2_26", f2_26) FLAG("f3_5", f3_5)
   FLAG("f10", f10) FLAG("f12", f12) FLAG("f13", f13) FLAG("f3_20", f3_20)
   FLAG("f3_6", f3_6) FLAG("f3_11", f3_11) FLAG("f3_12", f3_12)
   FLAG("sel34", sel34) FLAG("sel36", sel36) FLAG("sel37", sel37) FLAG("sel43", sel43)
   FLAG("f2_25", f2_25) FLAG("scale", scale) FLAG("rint", scale)
   VAL("mask", mask) VAL("drc", drc) VAL("chan", chan) VAL("a3f", a3f)
   VAL("dmask", dmask) VAL("rot", rot) VAL("path", path)
   VAL("n", n) VAL("imm", imm) VAL("rate", rate) VAL("wait", wait)
   VAL("am0", am4[0]) VAL("am1", am4[1]) VAL("am2", am4[2]) VAL("am3", am4[3])
   VAL("lim0", lim[0]) VAL("lim1", lim[1]) VAL("lim2", lim[2])
#undef FLAG
#undef VAL

   if (!strcmp(item, "unused2") && eq && v >= 0 && I->opc == SGX_ISA_OPC_RLP) {
      I->has_unused2 = true;
      I->unused2 = v;
      return true;
   }
   if (!strcmp(item, "nodst") && eq && v >= 0) {
      I->has_nodst = true;
      I->nodst = v;
      return true;
   }
   /* the classes with h21 rcnt write "rcnt" or "rcnt rpt=n" (checked at the end) */
   if (!strcmp(item, "rcnt") && !eq) {
      I->rcnt = true;
      return true;
   }
   if (!strcmp(item, "rpt") && eq && v >= 1) {
      I->rpt = v;
      return true;
   }
   if ((!strcmp(item, "swz0") || !strcmp(item, "swz1") || !strcmp(item, "swz2")) && eq) {
      int sw = v >= 0 ? v : LOOKUP(f16swz, val);
      if (sw < 0 || sw > 3)
         return fail(c, "bad swizzle '%s'", val);
      I->swz[item[3] - '0'] = sw;
      return true;
   }
   if (!strcmp(item, "rnd") && eq) {
      int r = v >= 0 ? v : LOOKUP(rnd_names, val);
      if (r < 0)
         return fail(c, "bad rnd '%s'", val);
      I->rnd = r;
      return true;
   }
   if ((!strcmp(item, "f16") || !strcmp(item, "c10")) && !eq) {
      /* hw 1: the source's format */
      if (item[0] == 'f')
         I->sf16 = true;
      else
         I->sc10 = true;
      return true;
   }
   return sgx545_isa_asm_item_int(I, item, val) ? true :
          fail(c, "unknown item '%s'", item);
}

/*
 * Operand patterns: D dst, 0/1/2 src slots, P pdst (pN), Q pdst with .nw,
 * I i<n> second dst, C/c clip planes, T #target, V #value (into v[]),
 * L smlsi operand, A #a (setfc), N #n, M #imm (into imm), E #imm11 (eld),
 * U #temps4 (phas.i).
 */
static bool
parse_operands(struct asm_ctx *c, struct sgx_isa_instr *I, const char *pat,
               char **ops, unsigned nops)
{
   unsigned vi = 0, ai = 0;

   if (strlen(pat) != nops)
      return fail(c, "wrong operand count for pattern %s", pat);

   for (unsigned i = 0; i < nops; i++) {
      char *s = trim(ops[i]);
      long v;
      int p;
      switch (pat[i]) {
      case 'D':
         if (!parse_operand(c, s, &I->dst))
            return false;
         break;
      case '0': case '1': case '2': {
         struct sgx_isa_src *o = &I->src[pat[i] - '0'];
         if (!parse_operand(c, s, o))
            return false;
         break;
      }
      case 'P':
      case 'Q':
         p = parse_pn(s);
         if (p < 0)
            return fail(c, "expected pN, got '%s'", s);
         I->pdst = p;
         if (pat[i] == 'Q')
            I->pw = !strstr(s, ".nw");
         break;
      case 'I':
         if (strcmp(s, "i0") && strcmp(s, "i1"))
            return fail(c, "expected i0/i1, got '%s'", s);
         I->idst = s[1] - '0';
         break;
      case 'C':
      case 'c':
         if (strncmp(s, "cp", 2) || !parse_int(s + 2, &v))
            return fail(c, "expected cp<n>, got '%s'", s);
         I->cp[pat[i] == 'c'] = v;
         break;
      case 'T':
         if (!parse_hash(c, s, &v))
            return false;
         I->target = v;
         break;
      case 'V':
         if (!parse_hash(c, s, &v))
            return false;
         I->v[vi++] = v;
         break;
      case 'L':
         if (!strncmp(s, "#0.", 3) && strlen(s) == 7) {
            unsigned sw = 0;
            for (unsigned k = 0; k < 4; k++) {
               const char *ch = strchr("xyzw", s[3 + k]);
               if (!ch)
                  return fail(c, "bad swizzle '%s'", s);
               sw |= (ch - "xyzw") << (2 * k);
            }
            I->swzf[vi] = true;
            I->v[vi++] = sw;
         } else {
            if (!parse_hash(c, s, &v))
               return false;
            I->v[vi++] = v & 0xff;
         }
         break;
      case 'A':
         if (!parse_hash(c, s, &v))
            return false;
         I->a[ai++] = v;
         break;
      case 'N':
         if (!parse_hash(c, s, &v))
            return false;
         I->n = v;
         break;
      case 'U':
         if (!parse_hash(c, s, &v))
            return false;
         I->temps4 = v;
         break;
      case 'M':
      case 'E':
         if (!parse_hash(c, s, &v))
            return false;
         I->imm = (uint32_t)v;
         break;
      default:
         return fail(c, "bad pattern %s", pat);
      }
   }
   return true;
}

struct simple_form {
   const char *name;
   enum sgx_isa_opc opc;
   const char *ops;
   uint8_t mask;        /* default write mask */
};

static const struct simple_form simple[] = {
   { "fmad", SGX_ISA_OPC_FMAD, "D012" }, { "fadm", SGX_ISA_OPC_FADM, "D012" },
   { "fmsa", SGX_ISA_OPC_FMSA, "D012" }, { "fsubflr", SGX_ISA_OPC_FSUBFLR, "D12" },
   { "fdp", SGX_ISA_OPC_FDP, "D12" }, { "fdpc", SGX_ISA_OPC_FDPC, "DC12" },
   { "fmaxmin", SGX_ISA_OPC_FMAXMIN, "D012" }, { "fminmax", SGX_ISA_OPC_FMINMAX, "D012" },
   { "fdsx", SGX_ISA_OPC_FDSX, "D12" }, { "fdsy", SGX_ISA_OPC_FDSY, "D12" },
   { "fddp", SGX_ISA_OPC_FDDP, "DI012" }, { "fddpc", SGX_ISA_OPC_FDDPC, "DICc012" },
   { "frcp", SGX_ISA_OPC_FRCP, "D1" }, { "frsq", SGX_ISA_OPC_FRSQ, "D1" },
   { "flog2", SGX_ISA_OPC_FLOG2, "D1" }, { "fexp2", SGX_ISA_OPC_FEXP2, "D1" },
   { "fsqrt", SGX_ISA_OPC_FSQRT, "D1" }, { "fsin", SGX_ISA_OPC_FSIN, "D1" },
   { "fcos", SGX_ISA_OPC_FCOS, "D1" }, { "fmad16", SGX_ISA_OPC_FMAD16, "D012" },
   { "mov", SGX_ISA_OPC_MOV, "D1" },
   { "op0xf3.s6", SGX_ISA_OPC_OP0XF3_S6, "D12" }, { "op0xf3.s7", SGX_ISA_OPC_OP0XF3_S7, "D012" },
   { "and", SGX_ISA_OPC_AND, "D12", 1 }, { "or", SGX_ISA_OPC_OR, "D12", 1 },
   { "xor", SGX_ISA_OPC_XOR, "D12", 1 }, { "shl", SGX_ISA_OPC_SHL, "D12", 1 },
   { "rol", SGX_ISA_OPC_ROL, "D12", 1 }, { "shr", SGX_ISA_OPC_SHR, "D12", 1 },
   { "asr", SGX_ISA_OPC_ASR, "D12", 1 }, { "rlp", SGX_ISA_OPC_RLP, "D1P", 1 },
   { "ba", SGX_ISA_OPC_BA, "T" }, { "br", SGX_ISA_OPC_BR, "T" },
   { "lapc", SGX_ISA_OPC_LAPC, "" }, { "setl", SGX_ISA_OPC_SETL, "1" },
   { "savl", SGX_ISA_OPC_SAVL, "D" }, { "nop", SGX_ISA_OPC_NOP, "" },
   { "op0x98", SGX_ISA_OPC_OP0X98, "" },
   { "smoa", SGX_ISA_OPC_SMOA, "VVVV" }, { "smr", SGX_ISA_OPC_SMR, "VVVV" },
   { "smbo", SGX_ISA_OPC_SMBO, "VVVV" }, { "imo", SGX_ISA_OPC_IMO, "VVVV" },
   { "smlsi", SGX_ISA_OPC_SMLSI, "LLLL" },
   { "smoa.r", SGX_ISA_OPC_SMOA_R, "12" }, { "smr.r", SGX_ISA_OPC_SMR_R, "12" },
   { "smlsi.r", SGX_ISA_OPC_SMLSI_R, "12" }, { "smbo.r", SGX_ISA_OPC_SMBO_R, "12" },
   { "setfc", SGX_ISA_OPC_SETFC, "AA" }, { "op0xfd", SGX_ISA_OPC_OP0XFD, "DN" },
   { "idf", SGX_ISA_OPC_IDF, "" }, { "wdf", SGX_ISA_OPC_WDF, "" },
   { "op0xd7", SGX_ISA_OPC_OP0XD7, "N" }, { "limm", SGX_ISA_OPC_LIMM, "DM" },
   { "lock", SGX_ISA_OPC_LOCK, "N" }, { "release", SGX_ISA_OPC_RELEASE, "N" },
   { "ldr", SGX_ISA_OPC_LDR, "D2" }, { "str", SGX_ISA_OPC_STR, "21" },
   { "op0xd6", SGX_ISA_OPC_OP0XD6, "N" },
   { "pcoeff", SGX_ISA_OPC_PCOEFF, "012" }, { "ptoff", SGX_ISA_OPC_PTOFF, "" },
   { "atst8", SGX_ISA_OPC_ATST8, "DQ012" }, { "depthf", SGX_ISA_OPC_DEPTHF, "012" },
   { "phas", SGX_ISA_OPC_PHAS, "12" }, { "phas.i", SGX_ISA_OPC_PHAS_I, "TU" },
   { "op0xff", SGX_ISA_OPC_OP0XFF, "12N" }, { "op0xfe", SGX_ISA_OPC_OP0XFE, "12N" },
   { "op0x100", SGX_ISA_OPC_OP0X100, "12N" },
};

/* The forms with h21 rcnt: h12-15 is a repeat count or a write mask. */
static bool
has_rcnt(enum sgx_isa_opc opc)
{
   switch (opc) {
   case SGX_ISA_OPC_FRCP: case SGX_ISA_OPC_FRSQ: case SGX_ISA_OPC_FLOG2:
   case SGX_ISA_OPC_FEXP2: case SGX_ISA_OPC_FSQRT: case SGX_ISA_OPC_FSIN:
   case SGX_ISA_OPC_FCOS: case SGX_ISA_OPC_MOV: case SGX_ISA_OPC_MOVC_U8:
   case SGX_ISA_OPC_MOVC_I16: case SGX_ISA_OPC_MOVC_I32: case SGX_ISA_OPC_MOVC_F32:
   case SGX_ISA_OPC_MOVC_C10: case SGX_ISA_OPC_OP0XF3_S6: case SGX_ISA_OPC_OP0XF3_S7:
   case SGX_ISA_OPC_PCK: case SGX_ISA_OPC_AND: case SGX_ISA_OPC_OR:
   case SGX_ISA_OPC_XOR: case SGX_ISA_OPC_SHL: case SGX_ISA_OPC_ROL:
   case SGX_ISA_OPC_SHR: case SGX_ISA_OPC_ASR: case SGX_ISA_OPC_RLP:
   case SGX_ISA_OPC_SMP:
      return true;
   default:
      return false;
   }
}

/* Forms with no predicate field reject a prefix. */
static bool
has_pred(enum sgx_isa_opc opc)
{
   switch (opc) {
   case SGX_ISA_OPC_NOP: case SGX_ISA_OPC_OP0X98: case SGX_ISA_OPC_IDF:
   case SGX_ISA_OPC_WDF: case SGX_ISA_OPC_LOCK: case SGX_ISA_OPC_RELEASE:
   case SGX_ISA_OPC_OP0XD6: case SGX_ISA_OPC_OP0XD7: case SGX_ISA_OPC_EMIT:
   case SGX_ISA_OPC_LDR: case SGX_ISA_OPC_STR: case SGX_ISA_OPC_SMOA:
   case SGX_ISA_OPC_SMR: case SGX_ISA_OPC_SMLSI: case SGX_ISA_OPC_SMBO:
   case SGX_ISA_OPC_IMO: case SGX_ISA_OPC_SMOA_R: case SGX_ISA_OPC_SMR_R:
   case SGX_ISA_OPC_SMLSI_R: case SGX_ISA_OPC_SMBO_R: case SGX_ISA_OPC_SETFC:
   case SGX_ISA_OPC_OP0XFD: case SGX_ISA_OPC_PCOEFF: case SGX_ISA_OPC_PTOFF:
   case SGX_ISA_OPC_PHAS: case SGX_ISA_OPC_PHAS_I: case SGX_ISA_OPC_OP0XFF:
   case SGX_ISA_OPC_OP0XFE: case SGX_ISA_OPC_OP0X100:
      return false;
   default:
      return true;
   }
}

static bool
uses_pred2(enum sgx_isa_opc opc)
{
   return opc == SGX_ISA_OPC_ATST8 || opc == SGX_ISA_OPC_DEPTHF ||
          sgx545_isa_asm_pred2_int(opc);
}

/* Split "a, b, c" at top-level commas. */
static unsigned
split_operands(char *s, char **ops, unsigned max)
{
   unsigned n = 0;
   s = trim(s);
   if (!*s)
      return 0;
   ops[n++] = s;
   for (char *p = s; *p && n < max; p++) {
      if (*p == ',') {
         *p = '\0';
         ops[n++] = p + 1;
      }
   }
   return n;
}

bool
sgx545_isa_asm_line(const char *line, struct sgx_isa_instr *I,
                    char *err, size_t errsize)
{
   struct asm_ctx c = { err, errsize };
   char buf[512], *s, *items = NULL;
   char predtext[8] = "";

   memset(I, 0, sizeof(*I));
   snprintf(buf, sizeof(buf), "%s", line);
   if ((s = strchr(buf, ';')))
      *s = '\0';
   s = trim(buf);

   if (*s == '(') {
      char *e = strchr(s, ')');
      if (!e || e - s - 1 >= (int)sizeof(predtext))
         return fail(&c, "bad predicate in '%s'", line);
      memcpy(predtext, s + 1, e - s - 1);
      predtext[e - s - 1] = '\0';
      s = trim(e + 1);
   }

   /* trailing [items], unless the '[' belongs to an indexed operand */
   char *lb = strrchr(s, '[');
   if (lb && lb > s && lb[-1] == ' ' && s[strlen(s) - 1] == ']') {
      *lb = '\0';
      items = lb + 1;
      items[strlen(items) - 1] = '\0';
   }

   char *mnem = s;
   char *rest = mnem;
   while (*rest && !isspace((unsigned char)*rest))
      rest++;
   if (*rest)
      *rest++ = '\0';

   char *ops[12];
   unsigned nops = split_operands(rest, ops, ARRAY_LEN(ops));

   char *parts[8];
   unsigned nparts = 0;
   char mbuf[64];
   snprintf(mbuf, sizeof(mbuf), "%s", mnem);
   for (char *t = strtok(mbuf, "."); t && nparts < ARRAY_LEN(parts); t = strtok(NULL, "."))
      parts[nparts++] = t;
   if (!nparts)
      return fail(&c, "empty line%s", "");
   const char *base = parts[0];
   const char *pat = NULL;
   int v;

   I->dmask = 15;

   for (unsigned i = 0; i < ARRAY_LEN(simple); i++) {
      if (!strcmp(mnem, simple[i].name)) {
         I->opc = simple[i].opc;
         I->mask = simple[i].mask;
         pat = simple[i].ops;
         break;
      }
   }

   if (I->opc) {
      /* done */
   } else if (!strcmp(base, "and") || !strcmp(base, "or") || !strcmp(base, "xor") ||
              !strcmp(base, "shl") || !strcmp(base, "rol") || !strcmp(base, "shr") ||
              !strcmp(base, "asr")) {
      if (nparts != 2 || strcmp(parts[1], "inv"))
         return fail(&c, "bad bitwise mnemonic '%s'", mnem);
      for (unsigned i = 0; i < ARRAY_LEN(simple); i++) {
         if (!strcmp(base, simple[i].name))
            I->opc = simple[i].opc;
      }
      I->inv = true;
      I->mask = 1;
      pat = "D12";
   } else if (!strcmp(base, "movc")) {
      static const enum sgx_isa_opc t[] = { 0, SGX_ISA_OPC_MOVC_U8, SGX_ISA_OPC_MOVC_I16,
                                            SGX_ISA_OPC_MOVC_I32, SGX_ISA_OPC_MOVC_F32,
                                            SGX_ISA_OPC_MOVC_C10 };
      int cnd = nparts == 3 ? LOOKUP(movc_cond, parts[1]) : -1;
      int ty = nparts == 3 ? LOOKUP(movc_type, parts[2]) : -1;
      if (cnd < 0 || ty < 1)
         return fail(&c, "bad movc '%s'", mnem);
      I->opc = t[ty];
      I->cond = cnd;
      pat = "D012";
   } else if (!strcmp(base, "pck")) {
      int d = nparts == 3 ? LOOKUP(pck_fmt, parts[1]) : -1;
      int sf = nparts == 3 ? LOOKUP(pck_fmt, parts[2]) : -1;
      if (d < 0 || sf < 0)
         return fail(&c, "bad pck '%s'", mnem);
      I->opc = SGX_ISA_OPC_PCK;
      I->dfmt = d;
      I->sfmt = sf;
      I->mask = 1;
      pat = "D12";
   } else if (!strcmp(base, "test") || !strcmp(base, "testmask")) {
      bool tm = base[4] == 'm';
      if (nparts != (tm ? 4u : 3u))
         return fail(&c, "bad test '%s'", mnem);
      int alu = parse_test_alu(parts[1]);
      int cnd = parse_test_cond(parts[2]);
      if (alu < 0 || cnd < 0)
         return fail(&c, "bad test op/cond '%s'", mnem);
      I->opc = tm ? SGX_ISA_OPC_TESTMASK : SGX_ISA_OPC_TEST;
      I->alu = alu;
      I->tcond = cnd;
      I->mask = 1;
      if (tm) {
         if ((v = LOOKUP(tm_type, parts[3])) < 0)
            return fail(&c, "bad testmask type '%s'", parts[3]);
         I->ttype = v;
         pat = nops == 3 ? "D12" : "12";
      } else {
         pat = nops == 4 ? "PD12" : "P12";
      }
   } else if (!strcmp(base, "smp")) {
      I->opc = SGX_ISA_OPC_SMP;
      I->mask = 1;
      if (nparts < 2 || (v = LOOKUP(smp_dim, parts[1])) < 0)
         return fail(&c, "bad smp dim '%s'", mnem);
      I->dim = v;
      for (unsigned i = 2; i < nparts; i++) {
         if ((v = LOOKUP(smp_lod, parts[i])) > 0)
            I->lod = v;
         else if ((v = LOOKUP(smp_cfmt, parts[i])) > 0)
            I->cfmt = v;
         else if ((v = LOOKUP(smp_pcf, parts[i])) > 0)
            I->pcf = v;
         else if ((v = LOOKUP(smp_ret, parts[i])) > 0)
            I->ret = v;
         else
            return fail(&c, "bad smp suffix '%s'", parts[i]);
      }
      pat = I->lod ? "D012" : "D01";
      if (!I->lod) {
         I->src[2].file = SGX_ISA_FILE_IMM;   /* the unused-source pattern */
      }
   } else if (!strcmp(base, "ld") || !strcmp(base, "st")) {
      int am = nparts == 3 ? LOOKUP(ld_am, parts[1]) : -1;
      int sz = nparts == 3 ? LOOKUP(ld_size, parts[2]) : -1;
      if (am < 0 || sz < 0)
         return fail(&c, "bad ld/st '%s'", mnem);
      I->am = am;
      I->size = sz;
      if (base[0] == 'l') {
         I->opc = SGX_ISA_OPC_LD;
         pat = nops == 4 ? "D012" : "D01";
         if (nops == 3)
            I->src[2].file = SGX_ISA_FILE_IMM;
      } else {
         I->opc = SGX_ISA_OPC_ST;
         pat = "012";
      }
   } else if (!strcmp(base, "eld")) {
      if (nparts != 2 || (strcmp(parts[1], "d") && strcmp(parts[1], "q")))
         return fail(&c, "bad eld '%s'", mnem);
      I->opc = parts[1][0] == 'd' ? SGX_ISA_OPC_ELD_D : SGX_ISA_OPC_ELD_Q;
      I->size = parts[1][0] == 'q';
      pat = "D10E2";
   } else if (!strcmp(base, "emit")) {
      if (nparts != 2 || (v = LOOKUP(emit_tgt, parts[1])) < 0)
         return fail(&c, "bad emit '%s'", mnem);
      I->opc = SGX_ISA_OPC_EMIT;
      I->tgt = v;
      pat = "012";
   } else if (sgx545_isa_asm_mnemonic_int(I, parts, nparts, nops, &pat)) {
      /* integer / fixed-point / dual-issue classes */
   } else {
      return fail(&c, "unknown mnemonic '%s'", mnem);
   }

   if (*predtext && !has_pred(I->opc))
      return fail(&c, "'%s' has no predicate field", mnem);
   if (*predtext && !parse_pred(predtext, uses_pred2(I->opc), &I->pred))
      return fail(&c, "bad predicate '%s'", predtext);

   if (!parse_operands(&c, I, pat, ops, nops))
      return false;

   /* ldr/str: a 14-bit immediate address is fine; atst8 writes its pdst unless .nw */
   if (items) {
      for (char *t = strtok(items, " "); t; t = strtok(NULL, " ")) {
         if (!parse_item(&c, I, t))
            return false;
      }
   }
   if (has_rcnt(I->opc)) {
      if (I->rpt > 1 && !I->rcnt)
         return fail(&c, "rpt=n needs rcnt here%s", "");
      if (I->rcnt && !I->rpt)
         I->rpt = 1;
   } else if (I->rcnt) {
      return fail(&c, "'%s' has no rcnt", mnem);
   }
   return true;
}
