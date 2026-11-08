/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * SGX545 compiler backend: instruction selection from NIR (after
 * sgx_lower()), register allocation, encoding.
 *
 * Vector operations become one USSE instruction with a repeat where the
 * operands allow it (ISA.md "Repeats"): the float 3-source/dot/min-max
 * classes step each source or hold it (inc bits, or moe when all step);
 * the scalar, move, select and testmask classes step every operand. A NIR
 * vector op is split into runs of components that one instruction covers.
 */
#include <math.h>
#include <stdarg.h>
#include <string.h>

#include "nir.h"
#include "nir_legacy.h"
#include "util/ralloc.h"
#include "util/u_math.h"
#include "util/register_allocate.h"
#include "util/half_float.h"
#include "util/hash_table.h"
#include "util/set.h"

#include "sgx_priv.h"

#define SGX_F_K 20   /* constant bits, resolved to c<n> or limm when legalised */

enum { VAL_NONE, VAL_V, VAL_PA, VAL_U, VAL_CONST, VAL_REFS };

/* tex instrs whose coordinate is v.xy * rcp(v.w) of a vec4 varying v: the
 * PDS's projected iteration does that divide (non-dependent, proj) */
struct proj_tex {
   nir_tex_instr *tex;
   nir_intrinsic_instr *load;
};

struct sgx_val {
   uint8_t kind;
   uint8_t comp;
   uint32_t n;
   nir_load_const_instr *lc;
   struct sgx_ref *refs;   /* VAL_REFS: one ref per component (F16 halves, aliases) */
   struct sgx_ref *shadow; /* a 16-bit register only assembled then read (find_shadow_regs):
                            * what each component was stored from, read as an alias */
};

#define NUM_TEMPS 124

/* ------------------------------------------------------------------------
 * logging */

void
sgx_logf(struct sgx_ctx *c, const char *fmt, ...)
{
   va_list ap;
   char buf[1024];
   va_start(ap, fmt);
   int n = vsnprintf(buf, sizeof buf, fmt, ap);
   va_end(ap);
   if (n < 0)
      return;
   if (n >= (int)sizeof buf)
      n = sizeof buf - 1;
   if (c->log_len + n + 1 > c->log_cap) {
      c->log_cap = MAX2(c->log_cap * 2, c->log_len + n + 4096);
      c->log = realloc(c->log, c->log_cap);
   }
   memcpy(c->log + c->log_len, buf, n + 1);
   c->log_len += n;
}

void
sgx_fail(struct sgx_ctx *c, const char *fmt, ...)
{
   va_list ap;
   char buf[512];
   va_start(ap, fmt);
   vsnprintf(buf, sizeof buf, fmt, ap);
   va_end(ap);
   if (!c->failed)
      sgx_logf(c, "error: %s\n", buf);
   c->failed = true;
}

/* ------------------------------------------------------------------------
 * IR helpers */

static unsigned
new_vreg(struct sgx_ctx *c, unsigned size)
{
   struct sgx_vreg v = { .size = size, .phys = -1, .first = -1, .last = -1 };
   util_dynarray_append_typed(&c->vregs, struct sgx_vreg, v);
   return util_dynarray_num_elements(&c->vregs, struct sgx_vreg) - 1;
}

/* a 16-bit vreg: `halves` F16 values, two per register */
static unsigned
new_vreg16(struct sgx_ctx *c, unsigned halves)
{
   unsigned v = new_vreg(c, halves);
   sgx_vreg(c, v)->half = true;
   return v;
}

static struct sgx_ref
ref(unsigned file, unsigned n)
{
   struct sgx_ref r = { .file = file, .n = n, .step = 1 };
   return r;
}

static struct sgx_ref
vref(unsigned v, unsigned comp)
{
   struct sgx_ref r = { .file = SGX_F_V, .n = v, .comp = comp, .step = 1 };
   return r;
}

static struct sgx_ref
cref(unsigned n)
{
   return ref(SGX_F_C, n);
}

static struct sgx_ref
href(unsigned v, unsigned half)
{
   struct sgx_ref r = vref(v, half);
   r.h16 = 1;
   return r;
}

static struct sgx_ins *
ins_alloc(struct sgx_ctx *c, unsigned kind, enum sgx_form form)
{
   struct sgx_ins *i = rzalloc(c, struct sgx_ins);
   i->kind = kind;
   i->form = form;
   i->rpt = 1;
   i->ndst = 1;
   return i;
}

static void
fld(struct sgx_ins *i, enum sgx_field id, int32_t v)
{
   for (unsigned k = 0; k < i->nfld; k++) {
      if (i->fld[k].id == id) {
         i->fld[k].v = v;
         return;
      }
   }
   assert(i->nfld < SGX_MAX_FLD);
   i->fld[i->nfld].id = id;
   i->fld[i->nfld++].v = v;
}

/* the instruction classes, by how a repeat steps their operands */
enum { CLS_INC, CLS_ALL, CLS_TESTMASK, CLS_ONE };

static unsigned
form_class(unsigned form)
{
   switch (form) {
   case SGX_OP_FMAD: case SGX_OP_FADM: case SGX_OP_FMSA: case SGX_OP_FSUBFLR:
   case SGX_OP_FMINMAX: case SGX_OP_FMAXMIN: case SGX_OP_FDP: case SGX_OP_FDSX: case SGX_OP_FDSY:
      return CLS_INC;
   case SGX_OP_FRCP: case SGX_OP_FRSQ: case SGX_OP_FLOG2: case SGX_OP_FEXP2: case SGX_OP_FSQRT:
   case SGX_OP_FSIN: case SGX_OP_FCOS: case SGX_OP_MOV:
   case SGX_OP_MOVC_F32: case SGX_OP_MOVC_I32:
      return CLS_ALL;
   case SGX_OP_TESTMASK:
      return CLS_TESTMASK;
   default:
      return CLS_ONE;
   }
}

static unsigned
max_rpt(unsigned cls)
{
   return cls == CLS_INC || cls == CLS_TESTMASK ? 4 : cls == CLS_ALL ? 16 : 1;
}

/* the encoder's register file of an IR file, -1 for none */
static int
file_kind(unsigned file)
{
   switch (file) {
   case SGX_F_V: case SGX_F_R: return SGX_ISA_FILE_R;
   case SGX_F_PA: return SGX_ISA_FILE_PA;
   case SGX_F_SA: case SGX_F_U: return SGX_ISA_FILE_SA;
   case SGX_F_O: return SGX_ISA_FILE_O;
   case SGX_F_C: return SGX_ISA_FILE_C;
   case SGX_F_G: return SGX_ISA_FILE_G;
   case SGX_F_I: return SGX_ISA_FILE_I;
   case SGX_F_IMM: return SGX_ISA_FILE_IMM;
   case SGX_F_P: return SGX_FILE_PRED;
   case SGX_F_LIT: return SGX_ISA_FILE_SA;
   default: return -1;
   }
}

static const struct sgx_slot *
slot_of(unsigned form, unsigned k)
{
   return sgx_form_slot(form, k);
}

/* can this slot of the form hold an operand of this file? */
static bool
slot_ok(unsigned form, unsigned k, const struct sgx_ref *r)
{
   const struct sgx_slot *s = slot_of(form, k);
   int kind = file_kind(r->file);
   if (!s || !s->role || kind < 0 || !(s->files & SGX_FILE_BIT(kind)))
      return false;
   if (r->neg && !s->neg)
      return false;
   if (r->abs && !s->abs)
      return false;
   return true;
}

/* hardware constants (ISA.md table, hardware-checked) as f32 bits */
static int
hw_const(uint32_t bits)
{
   static uint32_t tab[64];
   static bool init;
   if (!init) {
      for (int i = 0; i < 64; i++)
         tab[i] = 0xdeadbeef;
      union { float f; uint32_t u; } x;
#define SETF(i, v) (x.f = (float)(v), tab[i] = x.u)
      SETF(48, 0.0);
      SETF(52, 1.0);
      for (int i = 0; i < 10; i++)
         SETF(4 + i, ldexp(1.0, i + 1));
      for (int i = 0; i < 14; i++)
         SETF(14 + i, ldexp(1.0, -(i + 1)));
      SETF(28, M_E);
      SETF(29, 1.0 / M_E);
      SETF(30, M_SQRT2);
      SETF(31, M_SQRT1_2);
      SETF(32, M_PI / 4);
      SETF(33, M_PI / 2);
      SETF(34, M_PI);
      SETF(35, 1 / M_PI);
      SETF(36, 2 / M_PI);
      SETF(37, 4 / M_PI);
      SETF(38, 2 * M_PI);
      SETF(39, ldexp(1.0, -16));
      tab[56] = 0xffffffff;
#undef SETF
      init = true;
   }
   for (int i = 0; i < 64; i++)
      if (tab[i] == bits)
         return i;
   return -1;
}

static void
insert_before(struct sgx_ins *at, struct sgx_ins *n)
{
   list_addtail(&n->link, &at->link);
}

static void legalize(struct sgx_ctx *c, struct sgx_ins *i);
static int lit_run(struct sgx_ctx *c, const uint32_t *w, unsigned n);

/* append, legalising its operands (copies go in front of it) */
static struct sgx_ins *
emit(struct sgx_ctx *c, struct sgx_ins *i)
{
   list_addtail(&i->link, &c->ins);
   if (i->kind == SGX_INS_ALU)
      legalize(c, i);
   return i;
}

/* a copy of src (rpt iterations, stepping if src steps) into a new vreg */
static struct sgx_ref
materialize(struct sgx_ctx *c, struct sgx_ins *at, struct sgx_ref src, unsigned rpt)
{
   unsigned n = src.step ? rpt : 1;
   unsigned v = new_vreg(c, n);
   struct sgx_ins *m;
   if (src.file == SGX_F_K) {
      /* one limm per component (a stepping constant only has 0.0 / 1.0) */
      assert(n == 1 || !src.step);
      m = ins_alloc(c, SGX_INS_ALU, SGX_OP_LIMM);
      m->nops = 2;
      m->op[0] = vref(v, 0);
      m->op[1] = ref(SGX_F_IMM, src.neg ? src.n ^ 0x80000000u : src.n);
      if (src.abs)
         m->op[1].n &= 0x7fffffff;
      insert_before(at, m);
   } else if (src.h16) {
      /* F16 -> f32: x.f16 - floor(0) under format select (src1 takes any bank
       * and the modifiers) */
      m = ins_alloc(c, SGX_INS_ALU, SGX_OP_FSUBFLR);
      m->nops = 3;
      m->rpt = n;
      m->op[0] = vref(v, 0);
      m->op[1] = src;
      m->op[1].step = n > 1;
      m->op[2] = cref(48);
      m->op[2].step = 0;
      insert_before(at, m);
   } else if (src.neg || src.abs) {
      /* x - floor(0): a move that applies the source modifiers */
      m = ins_alloc(c, SGX_INS_ALU, SGX_OP_FSUBFLR);
      m->nops = 3;
      m->rpt = n;
      m->op[0] = vref(v, 0);
      m->op[1] = src;
      m->op[1].step = n > 1;
      m->op[2] = cref(48);
      m->op[2].step = 0;
      insert_before(at, m);
      legalize(c, m);
   } else {
      m = ins_alloc(c, SGX_INS_ALU, SGX_OP_MOV);
      m->nops = 2;
      m->rpt = n;
      m->op[0] = vref(v, 0);
      m->op[1] = src;
      insert_before(at, m);
      legalize(c, m);
   }
   struct sgx_ref r = vref(v, 0);
   r.step = src.step;
   return r;
}

static void
legalize(struct sgx_ctx *c, struct sgx_ins *i)
{
   unsigned cls = form_class(i->form);
   for (unsigned k = i->ndst; k < i->nops; k++) {
      struct sgx_ref *r = &i->op[k];
      if (r->file == SGX_F_K && r->h16) {
         sgx_fail(c, "16-bit constant in a 32-bit instruction");
         continue;
      }
      if (r->file == SGX_F_K) {
         const struct sgx_slot *s = slot_of(i->form, k);
         int h = hw_const(r->n);
         bool negok = s && s->neg;
         if (r->step && i->rpt > 1) {
            /* only 0.0 / 1.0 have four in a row */
            assert(h == 48 || h == 52);
            if (cls != CLS_INC && cls != CLS_ALL && cls != CLS_TESTMASK)
               h = -1;
         }
         uint32_t want = r->n;
         if (r->abs)
            want &= 0x7fffffff;
         if (r->neg)
            want ^= 0x80000000u;
         if ((h = hw_const(want)) >= 0) {
            *r = cref(h);
            r->step = i->rpt > 1 && (h == 48 || h == 52) ? 1 : 0;
         } else if (negok && (h = hw_const(want ^ 0x80000000u)) >= 0) {
            bool st = r->step;
            *r = cref(h);
            r->neg = 1;
            r->step = st && i->rpt > 1 && (h == 48 || h == 52);
         } else {
            /* a literal in SA where the slot takes one (driver_consts), else limm */
            struct sgx_ref lit = ref(SGX_F_LIT, 0);
            int ln = c->opts->driver_consts && !c->lit_off && slot_ok(i->form, k, &lit) ?
                     lit_run(c, &want, 1) : -1;
            if (ln >= 0) {
               *r = ref(SGX_F_LIT, ln);
            } else {
               struct sgx_ref k2 = ref(SGX_F_K, want);
               k2.step = 0;
               *r = materialize(c, i, k2, 1);
            }
            r->step = 0;
         }
         if (!r->step && cls != CLS_INC && i->rpt > 1)
            sgx_fail(c, "constant can't step in %s", sgx_form_name(i->form));
         if (r->file == SGX_F_C && !slot_ok(i->form, k, r)) {
            /* a slot without constants (src0): through a register */
            struct sgx_ref m = materialize(c, i, *r, i->rpt);
            *r = m;
         }
         continue;
      }
      if (r->h16) {
         /* an F16 half: a format-select operand of the float classes and
          * test (numbers count halves: registers 0-31), the scalar class's
          * F16 source, or a 16-bit pck source; anything else reads it as f32 */
         const struct sgx_slot *s = slot_of(i->form, k);
         bool scalar = sgx_form_is_scalar(i->form);
         bool pck16 = i->form == SGX_OP_PCK_U8_F16 || i->form == SGX_OP_PCK_F16_F16 || i->form == SGX_OP_PCK_F32_F16;
         unsigned reg = r->file == SGX_F_V ? r->comp / 2 : r->n / 2;
         bool small = r->file == SGX_F_V || r->file == SGX_F_U || reg < 32;
         struct sgx_ref plain = *r;
         plain.h16 = 0;
         bool kind_ok = slot_ok(i->form, k, &plain);
         if (((s && s->fmt && small && i->form != SGX_OP_TESTMASK) || scalar || pck16 ||
              i->form == SGX_OP_FMAD16) && kind_ok)
            continue;
         /* (a slot with no format select can't read a half at all: mov would
          * take the whole register) */
         if (s && s->fmt && slot_ok(i->form, k, r))
            continue;
      } else if (slot_ok(i->form, k, r)) {
         continue;
      }
      struct sgx_ref m = materialize(c, i, *r, i->rpt);
      r->h16 = 0;
      r->file = m.file;
      r->n = m.n;
      r->comp = m.comp;
      r->neg = r->abs = 0;
   }
}

/* ------------------------------------------------------------------------
 * NIR values -> refs */

static struct sgx_ref
src_comp(struct sgx_ctx *c, nir_legacy_src *ls, unsigned comp)
{
   if (!ls->is_ssa) {
      struct sgx_val *v = &c->vals[ls->reg.handle->index];
      if (v->shadow)
         return v->shadow[comp + ls->reg.base_offset * 4];
      if (sgx_vreg(c, v->n)->half)
         return href(v->n, comp + ls->reg.base_offset * 4);
      return vref(v->n, comp + ls->reg.base_offset * 4);
   }
   struct sgx_val *v = &c->vals[ls->ssa->index];
   switch (v->kind) {
   case VAL_REFS:
      return v->refs[comp];
   case VAL_V:
      if (sgx_vreg(c, v->n)->half)
         return href(v->n, v->comp + comp);
      return vref(v->n, v->comp + comp);
   case VAL_PA:
      return ref(SGX_F_PA, v->n + comp);
   case VAL_U: {
      c->const_used[v->n + comp] = 1;
      struct sgx_ref r = ref(SGX_F_U, v->n + comp);
      r.h16 = c->const_fmt[v->n + comp] == 2 || c->const_fmt[v->n + comp] == 5;
      return r;
   }
   case VAL_CONST:
      if (v->lc && v->lc->def.bit_size == 16) {
         struct sgx_ref r = ref(SGX_F_K, v->lc->value[comp].u16);
         r.h16 = 1;
         return r;
      }
      return ref(SGX_F_K, v->lc ? v->lc->value[comp].u32 : 0);
   default:
      sgx_fail(c, "use of a value with no register (ssa %u)", ls->ssa->index);
      return ref(SGX_F_C, 48);
   }
}

static struct sgx_ref
alu_src(struct sgx_ctx *c, nir_alu_instr *alu, unsigned s, unsigned comp)
{
   nir_legacy_alu_src ls = nir_legacy_chase_alu_src(&alu->src[s], true);
   struct sgx_ref r = src_comp(c, &ls.src, ls.swizzle[comp]);
   /* on top of any modifiers the value's refs already carry (an F16 view of
    * an fneg folded into f2f32) */
   if (ls.fabs)
      r.abs = 1, r.neg = 0;
   r.neg ^= ls.fneg;
   return r;
}

/* an F16 constant in f32 math: its f32 bits */
static struct sgx_ref
const_f32(struct sgx_ref r)
{
   if (r.file == SGX_F_K && r.h16) {
      union { float f; uint32_t u; } x = { .f = _mesa_half_to_float(r.n & 0xffff) };
      r.n = x.u;
      r.h16 = 0;
   }
   return r;
}

/* the destination of an instruction writing def: a vreg (new for SSA) */
static void
def_dest(struct sgx_ctx *c, nir_def *def, nir_legacy_dest *d, unsigned *vreg, unsigned *base)
{
   if (!d->is_ssa) {
      *vreg = c->vals[d->reg.handle->index].n;
      *base = d->reg.base_offset * 4;
      return;
   }
   struct sgx_val *v = &c->vals[d->ssa->index];
   if (v->kind != VAL_V) {
      v->kind = VAL_V;
      v->n = new_vreg(c, d->ssa->num_components);
      v->comp = 0;
   }
   *vreg = v->n;
   *base = v->comp;
}

static bool
ref_next(const struct sgx_ref *a, const struct sgx_ref *b)
{
   if (a->file != b->file || a->neg != b->neg || a->abs != b->abs)
      return false;
   if (a->file == SGX_F_V)
      return a->n == b->n && a->comp + 1 == b->comp;
   if (a->file == SGX_F_K)
      return a->n == b->n && (a->n == 0 || a->n == 0x3f800000);
   return a->n + 1 == b->n;
}

static bool
ref_same(const struct sgx_ref *a, const struct sgx_ref *b)
{
   return a->file == b->file && a->n == b->n && a->comp == b->comp && a->neg == b->neg &&
          a->abs == b->abs;
}

/*
 * Emit form over the components in wmask: dst[i] <- op(src[j][i]).
 * dsts/srcs are per-component refs; splits into runs one instruction can do.
 * fields are applied to each instruction.
 */
struct fieldset {
   unsigned n;
   struct { uint16_t id; int32_t v; } f[6];
};

/* the register a vreg reference names (h16 refs count halves) */
static bool
same_vreg_reg(const struct sgx_ref *a, const struct sgx_ref *b)
{
   return a->file == SGX_F_V && b->file == SGX_F_V && a->n == b->n &&
          (a->h16 ? a->comp / 2 : a->comp) == (b->h16 ? b->comp / 2 : b->comp);
}

/* two F16 halves of one register */
static bool
same_reg16(const struct sgx_ref *a, const struct sgx_ref *b)
{
   if (!a->h16 || !b->h16 || a->file != b->file || a->lane || b->lane)
      return false;
   if (a->file == SGX_F_V)
      return a->n == b->n && a->comp / 2 == b->comp / 2;
   return (a->file == SGX_F_PA || a->file == SGX_F_R) && a->n / 2 == b->n / 2;
}

static void emit_runs_raw(struct sgx_ctx *c, unsigned form, unsigned wmask, struct sgx_ref dst[4],
                          unsigned nsrc, struct sgx_ref src[][4], const struct fieldset *fs, uint8_t pred);

/* components are written in order, so a source naming a register an earlier
 * component already wrote (r = r.yzwx in a loop) would read the new value:
 * compute into a temporary, then copy */
static void
emit_runs(struct sgx_ctx *c, unsigned form, unsigned wmask, struct sgx_ref dst[4],
          unsigned nsrc, struct sgx_ref src[][4], const struct fieldset *fs, uint8_t pred)
{
   bool clash = false;
   for (unsigned w = 0; w < 4; w++)
      for (unsigned k = w + 1; k < 4; k++)
         for (unsigned s = 0; s < nsrc; s++)
            if ((wmask >> w & 1) && (wmask >> k & 1) && same_vreg_reg(&dst[w], &src[s][k]))
               clash = true;
   if (!clash || dst[ffs(wmask) - 1].h16) {
      emit_runs_raw(c, form, wmask, dst, nsrc, src, fs, pred);
      return;
   }
   unsigned t = new_vreg(c, 4);
   struct sgx_ref td[4], ts[1][4];
   for (unsigned k = 0; k < 4; k++) {
      td[k] = vref(t, k);
      ts[0][k] = td[k];
   }
   emit_runs_raw(c, form, wmask, td, nsrc, src, fs, pred);
   emit_runs_raw(c, SGX_OP_MOV, wmask, dst, 1, ts, &(struct fieldset){ 0 }, pred);
}

static void
emit_runs_raw(struct sgx_ctx *c, unsigned form, unsigned wmask, struct sgx_ref dst[4],
              unsigned nsrc, struct sgx_ref src[][4], const struct fieldset *fs, uint8_t pred)
{
   unsigned cls = form_class(form);
   unsigned mr = max_rpt(cls);
   bool scalar = sgx_form_is_scalar(form);
   unsigned i = 0;
   while (i < 4) {
      if (!(wmask & (1u << i))) {
         i++;
         continue;
      }
      unsigned n = 1;
      int mode[3] = { -1, -1, -1 };   /* 1 step, 0 hold */
      while (n < mr && i + n < 4 && (wmask & (1u << (i + n)))) {
         unsigned j = i + n;
         struct sgx_ref dp = dst[j - 1];
         if (scalar && dp.h16) {
            if (dp.file == SGX_F_V)
               dp.comp += 1;
            else
               dp.n += 1;
         }
         if (!ref_next(&dp, &dst[j]) || dst[j].file == SGX_F_K)
            break;
         bool ok = true;
         int nm[3];
         for (unsigned s = 0; s < nsrc; s++) {
            nm[s] = mode[s];
            bool st = ref_next(&src[s][j - 1], &src[s][j]);
            if (scalar && src[s][j].h16) {
               /* a scalar op's F16 source steps by register, its half fixed
                * (ISA.md hw1 source format) */
               struct sgx_ref a = src[s][j - 1];
               if (a.file == SGX_F_V)
                  a.comp += 1;
               else
                  a.n += 1;
               st = ref_next(&a, &src[s][j]);
            }
            bool hd = ref_same(&src[s][j - 1], &src[s][j]) && cls == CLS_INC;
            if (nm[s] < 0)
               nm[s] = st ? 1 : hd ? 0 : -2;
            else if ((nm[s] == 1 && !st) || (nm[s] == 0 && !hd))
               nm[s] = -2;
            if (nm[s] == -2)
               ok = false;
         }
         if (!ok)
            break;
         for (unsigned s = 0; s < nsrc; s++)
            mode[s] = nm[s];
         n++;
      }
      struct sgx_ins *in = ins_alloc(c, SGX_INS_ALU, form);
      in->rpt = n;
      in->pred = pred;
      in->nops = 1 + nsrc;
      in->op[0] = dst[i];
      for (unsigned s = 0; s < nsrc; s++) {
         in->op[1 + s] = src[s][i];
         in->op[1 + s].step = n > 1 ? mode[s] == 1 : 1;
      }
      for (unsigned k = 0; k < fs->n; k++)
         fld(in, fs->f[k].id, fs->f[k].v);
      emit(c, in);
      i += n;
   }
}

static bool
is_rpa(const struct sgx_ref *r)
{
   return r->file == SGX_F_V || r->file == SGX_F_R || r->file == SGX_F_PA;
}

/* ------------------------------------------------------------------------
 * ALU */

static const struct fieldset NOF = { 0 };

/* ------------------------------------------------------------------------
 * float16 (mediump, as the DDK computes it): values are F16 halves, two per
 * register; arithmetic is fmad16 on lane pairs (ISA.md: per-source swizzle
 * lohi / ll / hh / f32-converted, rounds to nearest), packing is
 * pck.f16.f16, conversions pck/fmad16. */

enum { SWZ_LOHI = 0, SWZ_LL = 1, SWZ_HH = 2, SWZ_F32 = 3 };

static bool
same_reg_pair(const struct sgx_ref *a, const struct sgx_ref *b)
{
   if (!a->h16 || !b->h16 || a->file != b->file || a->neg != b->neg || a->abs != b->abs)
      return false;
   if (a->file == SGX_F_V)
      return a->n == b->n && !(a->comp & 1) && b->comp == a->comp + 1;
   if (a->file == SGX_F_U)
      return !(a->n & 1) && b->n == a->n + 1;
   return a->file != SGX_F_K && !(a->n & 1) && b->n == a->n + 1;
}

static struct sgx_ins *
add(struct sgx_ctx *c, struct sgx_ins *i)
{
   list_addtail(&i->link, &c->ins);
   return i;
}

/* lo <- a, hi <- b (F16 halves) into a new register (a 2-half vreg) */
static struct sgx_ref pair_reg(struct sgx_ctx *c, struct sgx_ref a, struct sgx_ref b);
static struct sgx_ref pair_reg_plain(struct sgx_ctx *c, struct sgx_ref a, struct sgx_ref b);

/* an F16 value of a 32-bit ref: fmad16 t, x(f32), c45, c48 -> both lanes */
static struct sgx_ref
f32_to_h(struct sgx_ctx *c, struct sgx_ref x)
{
   unsigned t = new_vreg16(c, 2);
   struct sgx_ins *i = ins_alloc(c, SGX_INS_ALU, SGX_OP_FMAD16);
   i->nops = 4;
   i->op[0] = href(t, 0);
   /* (an F16 constant: its f32 value, not its bits read as f32; dEQP random
    * exponential.14's log(vec4(c, 2.0, ...)) took log2(denormal)) */
   x = const_f32(x);
   if (x.file == SGX_F_K) {
      int h = hw_const(x.n);
      if (h < 0) {
         unsigned l = new_vreg(c, 1);
         struct sgx_ins *m = ins_alloc(c, SGX_INS_ALU, SGX_OP_LIMM);
         m->nops = 2;
         m->op[0] = vref(l, 0);
         m->op[1] = ref(SGX_F_IMM, x.n);
         add(c, m);
         x = vref(l, 0);
      } else {
         x = cref(h);
      }
   }
   if (!is_rpa(&x) || x.neg || x.abs) {
      unsigned v = new_vreg(c, 1);
      struct sgx_ref dd[4] = { vref(v, 0) }, ss[2][4] = { { x }, { cref(48) } };
      emit_runs(c, x.neg || x.abs ? SGX_OP_FSUBFLR : SGX_OP_MOV, 1, dd, x.neg || x.abs ? 2 : 1, ss, &NOF, 0);
      x = vref(v, 0);
   }
   i->op[1] = x;
   i->op[2] = cref(45);
   i->op[3] = cref(48);
   fld(i, SGX_FLD_SWZ0, SWZ_F32);
   add(c, i);
   return href(t, 0);
}

static struct sgx_ref pair_src(struct sgx_ctx *c, struct sgx_ref a, struct sgx_ref b, int *swz);

/* a pck.f16.f16 source for an F16 value: a constant's half bits go in as a
 * register pair (an F16 constant's bits are not an f32), f32 is converted */
static struct sgx_ref
store_h(struct sgx_ctx *c, struct sgx_ref x)
{
   if (x.file == SGX_F_K && x.h16) {
      int sw;
      struct sgx_ref r = pair_src(c, x, x, &sw);
      r.h16 = 1;
      r.lane = 0;
      return r;
   }
   if (!x.h16 || x.file == SGX_F_K)
      return f32_to_h(c, x);
   return x;
}

/* n literal words in consecutive SA slots (driver_consts): an existing run
 * of them or new ones; -1 when there is no room */
static int
lit_run(struct sgx_ctx *c, const uint32_t *w, unsigned n)
{
   for (unsigned s = 0; s + n <= c->n_lits; s++) {
      unsigned j = 0;
      while (j < n && c->lit_bits[s + j] == w[j])
         j++;
      if (j == n)
         return s;
   }
   if (c->n_lits + n > ARRAY_SIZE(c->lit_bits))
      return -1;
   unsigned s = c->n_lits;
   for (unsigned j = 0; j < n; j++)
      c->lit_bits[c->n_lits++] = w[j];
   return s;
}

/* a register operand for a fmad16 source reading lanes (a, b), and its swizzle */
static struct sgx_ref
pair_src(struct sgx_ctx *c, struct sgx_ref a, struct sgx_ref b, int *swz)
{
   if (a.file == SGX_F_K && a.h16 && b.file == SGX_F_K && b.h16) {
      /* the modifiers go into the bits (abs, then negate) */
      uint32_t ha = a.n & 0xffff, hb = b.n & 0xffff;
      if (a.abs) ha &= 0x7fff;
      if (a.neg) ha ^= 0x8000;
      if (b.abs) hb &= 0x7fff;
      if (b.neg) hb ^= 0x8000;
      uint32_t w = ha | hb << 16;
      *swz = SWZ_LOHI;
      if (w == 0)
         return cref(48);
      if (w == 0x3c003c00)
         return cref(45);
      if (c->opts->driver_consts && !c->lit_off) {
         /* a literal in SA, as libusc keeps them beside the uniforms */
         unsigned n;
         for (n = 0; n < c->n_lits && c->lit_bits[n] != w; n++)
            ;
         if (n < ARRAY_SIZE(c->lit_bits)) {
            if (n == c->n_lits)
               c->lit_bits[c->n_lits++] = w;
            struct sgx_ref r = ref(SGX_F_LIT, n);
            r.h16 = 1;
            return r;
         }
      }
      unsigned l = new_vreg16(c, 2);
      struct sgx_ins *m = ins_alloc(c, SGX_INS_ALU, SGX_OP_LIMM);
      m->nops = 2;
      m->op[0] = href(l, 0);
      m->op[1] = ref(SGX_F_IMM, w);
      add(c, m);
      return href(l, 0);
   }
   if (same_reg_pair(&a, &b)) {
      *swz = SWZ_LOHI;
      return a;
   }
   if (ref_same(&a, &b) && a.h16 && a.file != SGX_F_K) {
      bool hi = a.file == SGX_F_V ? (a.comp & 1) : (a.n & 1);
      *swz = hi ? SWZ_HH : SWZ_LL;
      struct sgx_ref r = a;
      if (r.file == SGX_F_V)
         r.comp &= ~1u;
      else if (r.file != SGX_F_U)
         r.n &= ~1u;
      /* (a uniform keeps its own number: the encoder finds its SA register,
       * and constant 2m may have none when only 2m+1 is read) */
      return r;
   }
   if (ref_same(&a, &b) && !a.h16 && a.file != SGX_F_K) {
      *swz = SWZ_F32;
      return a;
   }
   *swz = SWZ_LOHI;
   return pair_reg(c, a, b);
}

static struct sgx_ref
pair_reg(struct sgx_ctx *c, struct sgx_ref a, struct sgx_ref b)
{
   /* pck has no modifiers: pack the plain halves, put the (common) modifiers
    * on the packed register */
   if (a.neg != b.neg || a.abs != b.abs) {
      sgx_fail(c, "F16 pair with different modifiers per lane");
      return a;
   }
   bool ng = a.neg, ab = a.abs;
   a.neg = a.abs = b.neg = b.abs = 0;
   struct sgx_ref r = pair_reg_plain(c, a, b);
   r.neg = ng;
   r.abs = ab;
   return r;
}

static struct sgx_ref
pair_reg_plain(struct sgx_ctx *c, struct sgx_ref a, struct sgx_ref b)
{
   if (!a.h16)
      a = f32_to_h(c, a);
   if (!b.h16)
      b = f32_to_h(c, b);
   if (a.file == SGX_F_K && b.file == SGX_F_K) {
      int sw;
      return pair_src(c, a, b, &sw);
   }
   /* a constant beside a half (vec2(v.y, 0.5)): its half bits from a register */
   if (a.file == SGX_F_K)
      a = store_h(c, a);
   if (b.file == SGX_F_K)
      b = store_h(c, b);
   unsigned t = new_vreg16(c, 2);
   struct sgx_ins *i = ins_alloc(c, SGX_INS_ALU, SGX_OP_PCK_F16_F16);
   i->nops = 3;
   i->op[0] = href(t, 0);
   i->op[1] = a;
   i->op[2] = b;
   add(c, i);
   return href(t, 0);
}

/* d.pair <- s0 * s1 + s2 on lanes (2p, 2p+1); src[j][lane] */
static void
fmad16(struct sgx_ctx *c, struct sgx_ref dpair, struct sgx_ref s[3][2])
{
   struct sgx_ref (*sp)[2] = s;
   int swz[3];
   struct sgx_ref r[3];
   /* src0 is r/pa only and has no negate: swap the factors, fold the negate */
   if ((!is_rpa(&sp[0][0]) || sp[0][0].neg) && is_rpa(&sp[1][0]) && !sp[1][0].neg) {
      struct sgx_ref t0 = sp[0][0], t1 = sp[0][1];
      sp[0][0] = sp[1][0]; sp[0][1] = sp[1][1];
      sp[1][0] = t0; sp[1][1] = t1;
   }
   /* x * 1 + y with x a constant: y * 1 + x (exact either way) */
   bool one = true;
   for (unsigned l = 0; l < 2; l++)
      one &= sp[1][l].file == SGX_F_K && sp[1][l].h16 && !sp[1][l].neg && !sp[1][l].abs &&
             (sp[1][l].n & 0xffff) == 0x3c00;
   if (one && !is_rpa(&sp[0][0]) && is_rpa(&sp[2][0]) && !sp[2][0].neg && is_rpa(&sp[2][1]) && !sp[2][1].neg) {
      struct sgx_ref t0 = sp[0][0], t1 = sp[0][1];
      sp[0][0] = sp[2][0]; sp[0][1] = sp[2][1];
      sp[2][0] = t0; sp[2][1] = t1;
   }
   bool neg0 = sp[0][0].neg;
   sp[0][0].neg = sp[0][1].neg = 0;
   for (unsigned j = 0; j < 3; j++) {
      if (j && sp[j][0].file == SGX_F_K && sp[j][0].h16 && sp[j][1].file == SGX_F_K && sp[j][1].h16 &&
          ((sp[j][0].n ^ (sp[j][0].neg ? 0x8000 : 0)) & 0xffff) == 0xbc00 &&
          ((sp[j][1].n ^ (sp[j][1].neg ? 0x8000 : 0)) & 0xffff) == 0xbc00) {
         /* (-1, -1): -c45, as libusc writes it (no limm) */
         r[j] = cref(45);
         r[j].neg = 1;
         swz[j] = SWZ_LOHI;
         continue;
      }
      r[j] = pair_src(c, sp[j][0], sp[j][1], &swz[j]);
   }
   if (!is_rpa(&r[0])) {
      c->lit_off = true;
      r[0] = pair_reg(c, sp[0][0], sp[0][1]);
      c->lit_off = false;
      swz[0] = SWZ_LOHI;
   }
   if (!is_rpa(&r[0]) && sp[0][0].file == SGX_F_K && sp[0][1].file == SGX_F_K) {
      /* a constant pair with a hardware constant's value (0, 1.0): src0
       * takes registers only, so limm it */
      struct sgx_ref a = sp[0][0], b = sp[0][1];
      uint32_t ha = a.n & 0xffff, hb = b.n & 0xffff;
      if (a.abs) ha &= 0x7fff;
      if (a.neg) ha ^= 0x8000;
      if (b.abs) hb &= 0x7fff;
      if (b.neg) hb ^= 0x8000;
      unsigned l = new_vreg16(c, 2);
      struct sgx_ins *m = ins_alloc(c, SGX_INS_ALU, SGX_OP_LIMM);
      m->nops = 2;
      m->op[0] = href(l, 0);
      m->op[1] = ref(SGX_F_IMM, ha | hb << 16);
      add(c, m);
      r[0] = href(l, 0);
      swz[0] = SWZ_LOHI;
   }
   if (neg0)
      r[1].neg ^= 1;
   struct sgx_ins *i = ins_alloc(c, SGX_INS_ALU, SGX_OP_FMAD16);
   i->nops = 4;
   i->op[0] = dpair;
   for (unsigned j = 0; j < 3; j++)
      i->op[1 + j] = r[j];
   fld(i, SGX_FLD_SWZ0, swz[0]);
   fld(i, SGX_FLD_SWZ1, swz[1]);
   fld(i, SGX_FLD_SWZ2, swz[2]);
   add(c, i);
}

static void
set_refs(struct sgx_ctx *c, nir_def *def, const struct sgx_ref *r, unsigned n)
{
   struct sgx_val *v = &c->vals[def->index];
   v->kind = VAL_REFS;
   v->refs = ralloc_array(c, struct sgx_ref, 4);
   for (unsigned k = 0; k < 4; k++)
      v->refs[k] = r[k < n ? k : 0];
}

static void emit_alu(struct sgx_ctx *c, nir_alu_instr *alu);
static struct sgx_ref cond_f32(struct sgx_ctx *c, struct sgx_ref r);

/* a 16-bit op done in f32: the instruction selected as if the def were
 * 32-bit (its F16 sources are format-select operands), then each component
 * rounded to F16 */
static void
alu_as_f32(struct sgx_ctx *c, nir_alu_instr *alu)
{
   unsigned nc = alu->def.num_components;
   alu->def.bit_size = 32;
   struct sgx_val save = c->vals[alu->def.index];
   /* (into the def: a 16-bit register it's stored to gets it by the store's
    * pck, not as f32) */
   c->def_dest = true;
   emit_alu(c, alu);
   c->def_dest = false;
   alu->def.bit_size = 16;
   struct sgx_val *v = &c->vals[alu->def.index];
   struct sgx_ref r[4];
   for (unsigned k = 0; k < nc; k++) {
      nir_legacy_src ls = { .is_ssa = true, .ssa = &alu->def };
      r[k] = f32_to_h(c, src_comp(c, &ls, k));
   }
   (void)save;
   set_refs(c, &alu->def, r, nc);
   (void)v;
}

/* a register holding the F16 pair (x, y) as is: the pair itself, a constant's
 * bits, or a pck.f16.f16 */
static struct sgx_ref
whole_pair(struct sgx_ctx *c, struct sgx_ref x, struct sgx_ref y)
{
   int sw;
   if (x.file == SGX_F_K && y.file == SGX_F_K)
      return pair_src(c, x, y, &sw);
   if (same_reg_pair(&x, &y))
      return x;
   return pair_reg_plain(c, x, y);
}

/* the half (0, 1) a plain F16 register operand names, -1 for anything else */
static int
half_of(const struct sgx_ref *x)
{
   if (!x->h16 || x->neg || x->abs || x->lane ||
       (x->file != SGX_F_V && x->file != SGX_F_PA && x->file != SGX_F_R))
      return -1;
   return (x->file == SGX_F_V ? x->comp : x->n) & 1;
}

/* the whole register holding that half */
static struct sgx_ref
whole_reg(struct sgx_ref x)
{
   if (x.file == SGX_F_V)
      x.comp &= ~1u;
   else
      x.n &= ~1u;
   return x;
}

/* An F16 select on one 32-bit condition for every channel (gl4es' gl_Color =
 * gl_FrontFacing ? gl_FrontColor : gl_BackColor): movc.nz.i32 on whole
 * register pairs, exact and without widening. false: not that shape. */
static bool
csel16(struct sgx_ctx *c, nir_alu_instr *alu, struct sgx_ref s[3][4])
{
   unsigned nc = alu->def.num_components;
   struct sgx_ref cond = s[0][0];
   if (cond.h16 || cond.neg || cond.abs || !is_rpa(&cond))
      return false;
   for (unsigned k = 0; k < nc; k++) {
      if (!ref_same(&s[0][k], &cond))
         return false;
      for (unsigned j = 1; j < 3; j++)
         if (s[j][k].neg || s[j][k].abs)
            return false;
   }
   unsigned t = new_vreg16(c, ALIGN_POT(nc, 2));
   struct sgx_ref r[4];
   for (unsigned p = 0; 2 * p < nc; p++) {
      unsigned l1 = 2 * p + 1 < nc ? 2 * p + 1 : 2 * p;
      struct sgx_ins *i = ins_alloc(c, SGX_INS_ALU, SGX_OP_MOVC_I32);
      i->nops = 4;
      i->op[0] = href(t, 2 * p);
      i->op[1] = cond;
      int h1 = half_of(&s[1][l1]), h2 = half_of(&s[2][l1]);
      if (l1 == 2 * p && h1 >= 0 && h1 == h2) {
         /* one lane, in the same half of both: their registers as they are,
          * the result in that half */
         i->op[2] = whole_reg(s[1][l1]);
         i->op[3] = whole_reg(s[2][l1]);
         r[l1] = href(t, 2 * p + h1);
      } else {
         i->op[2] = whole_pair(c, s[1][2 * p], s[1][l1]);
         i->op[3] = whole_pair(c, s[2][2 * p], s[2][l1]);
         r[2 * p] = href(t, 2 * p);
         r[l1] = href(t, l1);
      }
      fld(i, SGX_FLD_COND, 1);
      add(c, i);
   }
   set_refs(c, &alu->def, r, nc);
   return true;
}

/* every use of a 32-bit bool is a select's condition */
static bool
cond_only(nir_def *def)
{
   nir_foreach_use_including_if(u, def) {
      nir_instr *ui = nir_src_is_if(u) ? NULL : nir_src_use_instr(u);
      if (!ui || ui->type != nir_instr_type_alu ||
          (nir_instr_as_alu(ui)->op != nir_op_fcsel && nir_instr_as_alu(ui)->op != nir_op_b32csel) ||
          u != &nir_instr_as_alu(ui)->src[0].src)
         return false;
   }
   return true;
}

/* a 32-bit select of values held as F16 halves (mediump varyings through
 * f2f32): csel16's whole-pair movc, the result still F16 halves */
static bool
f16_pairs(struct sgx_ctx *c, nir_alu_instr *alu)
{
   for (unsigned j = 1; j < 3; j++)
      for (unsigned k = 0; k < alu->def.num_components; k++)
         if (!alu_src(c, alu, j, k).h16)
            return false;
   return true;
}

/* the value of a 16-bit def lives in a new 16-bit vreg; pairs of it */
static void
emit_alu16(struct sgx_ctx *c, nir_alu_instr *alu)
{
   unsigned nc = alu->def.num_components, nin = nir_op_infos[alu->op].num_inputs;
   struct sgx_ref s[3][4];
   for (unsigned j = 0; j < nin && j < 3; j++)
      for (unsigned k = 0; k < nc; k++)
         s[j][k] = alu_src(c, alu, j, k);
   if ((alu->op == nir_op_fcsel || alu->op == nir_op_b32csel) && csel16(c, alu, s))
      return;
   if (alu->op == nir_op_fmin || alu->op == nir_op_fmax || alu->op == nir_op_fdot2 ||
       alu->op == nir_op_fdot3 || alu->op == nir_op_fdot4 || alu->op == nir_op_fsat || alu->op == nir_op_fcsel ||
       alu->op == nir_op_b32csel || alu->op == nir_op_fsqrt || alu->op == nir_op_fsin_agx) {
      /* no F16 form: the f32 instruction reading the F16 halves (format
       * select), result converted back with fmad16's rounding */
      alu_as_f32(c, alu);
      return;
   }
   if (alu->op == nir_op_mov || alu->op == nir_op_vec2 || alu->op == nir_op_vec3 || alu->op == nir_op_vec4 ||
       alu->op == nir_op_f2f16 || alu->op == nir_op_b2f16) {
      /* an alias of the halves it names (f2f16 of an F16-held 32-bit value is
       * a no-op; of a real f32 value, a rounding fmad16) */
      struct sgx_ref r[4];
      if (alu->op == nir_op_f2f16) {
         /* an f32 uniform read as F16: pck.f16.f32, two halves at a time,
          * truncating as libusc's own conversion of one does (hwt_p4) */
         for (unsigned k = 0; k < nc; k++) {
            r[k] = s[0][k];
            if (r[k].file != SGX_F_U || r[k].h16 || r[k].neg || r[k].abs)
               continue;
            bool two = k + 1 < nc && s[0][k + 1].file == SGX_F_U && !s[0][k + 1].h16 && !s[0][k + 1].neg &&
                       !s[0][k + 1].abs;
            unsigned t = new_vreg16(c, 2);
            struct sgx_ins *pk = ins_alloc(c, SGX_INS_ALU, SGX_OP_PCK_F16_F32);
            pk->nops = 3;
            pk->op[0] = href(t, 0);
            pk->op[1] = s[0][k];
            pk->op[2] = two ? s[0][k + 1] : s[0][k];
            add(c, pk);
            r[k] = href(t, 0);
            if (two)
               r[++k] = href(t, 1);
         }
         for (unsigned k = 0; k < nc; k++) {
            if (r[k].h16)
               continue;
            if (r[k].file == SGX_F_K) {
               r[k].n = _mesa_float_to_half(uif(r[k].n));
               r[k].h16 = 1;
            } else {
               r[k] = f32_to_h(c, r[k]);
            }
         }
         set_refs(c, &alu->def, r, nc);
         return;
      }
      for (unsigned k = 0; k < nc; k++) {
         r[k] = alu->op == nir_op_mov || alu->op == nir_op_f2f16 || alu->op == nir_op_b2f16 ? s[0][k]
                                                                                            : alu_src(c, alu, k, 0);
         if (r[k].file == SGX_F_K && !r[k].h16) {
            /* a constant: its F16 bits (round to nearest) */
            r[k].n = _mesa_float_to_half(uif(r[k].n));
            r[k].h16 = 1;
         } else if (!r[k].h16 && alu->op == nir_op_f2f16) {
            r[k] = f32_to_h(c, r[k]);
         }
         /* (a vecN or mov of an f32 value, a fract's: it stays f32; an F16
          * reader converts it, the colour packs it from f32) */
      }
      set_refs(c, &alu->def, r, nc);
      return;
   }
   if (alu->op == nir_op_ffract) {
      /* fsubflr's f32 result as is (libusc packs it to U8 straight from
       * f32); an F16 reader converts where it reads it */
      struct sgx_ref r[4];
      for (unsigned k = 0; k < nc; k++) {
         unsigned f = new_vreg(c, 1);
         struct sgx_ins *i = ins_alloc(c, SGX_INS_ALU, SGX_OP_FSUBFLR);
         i->nops = 3;
         i->op[0] = vref(f, 0);
         i->op[1] = s[0][k];
         i->op[2] = s[0][k];
         emit(c, i);
         r[k] = vref(f, 0);
      }
      set_refs(c, &alu->def, r, nc);
      return;
   }
   unsigned d = new_vreg16(c, ALIGN_POT(nc, 2));
   c->vals[alu->def.index].kind = VAL_V;
   c->vals[alu->def.index].n = d;
   c->vals[alu->def.index].comp = 0;
   for (unsigned p = 0; p < (nc + 1) / 2; p++) {
      unsigned l0 = 2 * p, l1 = MIN2(2 * p + 1, nc - 1);
      struct sgx_ref m[3][2];
      struct sgx_ref dp = href(d, 2 * p);
      switch (alu->op) {
      case nir_op_fmul:
         m[0][0] = s[0][l0]; m[0][1] = s[0][l1];
         m[1][0] = s[1][l0]; m[1][1] = s[1][l1];
         m[2][0] = ref(SGX_F_K, 0); m[2][0].h16 = 1; m[2][1] = m[2][0];
         fmad16(c, dp, m);
         break;
      case nir_op_fadd:
         m[0][0] = s[0][l0]; m[0][1] = s[0][l1];
         m[1][0] = ref(SGX_F_K, 0x3c00); m[1][0].h16 = 1; m[1][1] = m[1][0];
         m[2][0] = s[1][l0]; m[2][1] = s[1][l1];
         fmad16(c, dp, m);
         break;
      case nir_op_fmad:
      case nir_op_ffma:
      case nir_op_ffma_weak:
         for (unsigned j = 0; j < 3; j++) {
            m[j][0] = s[j][l0];
            m[j][1] = s[j][l1];
         }
         fmad16(c, dp, m);
         break;
      case nir_op_fneg:
      case nir_op_fabs:
         m[0][0] = s[0][l0]; m[0][1] = s[0][l1];
         if (alu->op == nir_op_fneg) {
            m[0][0].neg ^= 1; m[0][1].neg ^= 1;
         } else {
            m[0][0].abs = m[0][1].abs = 1; m[0][0].neg = m[0][1].neg = 0;
         }
         m[1][0] = ref(SGX_F_K, 0x3c00); m[1][0].h16 = 1; m[1][1] = m[1][0];
         m[2][0] = ref(SGX_F_K, 0); m[2][0].h16 = 1; m[2][1] = m[2][0];
         fmad16(c, dp, m);
         break;
      case nir_op_flrp: {
         /* the DDK's mix: b * t + a, then t * -a + that */
         unsigned t = new_vreg16(c, 2);
         m[0][0] = s[1][l0]; m[0][1] = s[1][l1];
         m[1][0] = s[2][l0]; m[1][1] = s[2][l1];
         m[2][0] = s[0][l0]; m[2][1] = s[0][l1];
         fmad16(c, href(t, 0), m);
         m[0][0] = s[2][l0]; m[0][1] = s[2][l1];
         m[1][0] = s[0][l0]; m[1][1] = s[0][l1];
         m[1][0].neg ^= 1; m[1][1].neg ^= 1;
         m[2][0] = href(t, 0); m[2][1] = href(t, 1);
         fmad16(c, dp, m);
         break;
      }
      case nir_op_ffloor:
      case nir_op_ffract: {
         /* per lane: fract in f32 from the F16 half (fsubflr under format
          * select), then x - fract as fmad16 (the DDK's floor) */
         struct sgx_ref lane[2];
         for (unsigned q = 0; q < 2; q++) {
            unsigned li = q ? l1 : l0;
            unsigned f = new_vreg(c, 1);
            struct sgx_ins *i = ins_alloc(c, SGX_INS_ALU, SGX_OP_FSUBFLR);
            i->nops = 3;
            i->op[0] = vref(f, 0);
            i->op[1] = s[0][li];
            i->op[2] = s[0][li];
            emit(c, i);
            unsigned t = new_vreg16(c, 2);
            struct sgx_ref mm[3][2];
            if (alu->op == nir_op_ffloor) {
               mm[0][0] = mm[0][1] = vref(f, 0);
               mm[1][0] = ref(SGX_F_K, 0xbc00); mm[1][0].h16 = 1; mm[1][1] = mm[1][0];
               mm[2][0] = mm[2][1] = s[0][li];
            } else {
               mm[0][0] = mm[0][1] = vref(f, 0);
               mm[1][0] = ref(SGX_F_K, 0x3c00); mm[1][0].h16 = 1; mm[1][1] = mm[1][0];
               mm[2][0] = ref(SGX_F_K, 0); mm[2][0].h16 = 1; mm[2][1] = mm[2][0];
            }
            if (l1 == l0) {
               /* one lane: fmad16 writes both halves of the destination */
               fmad16(c, dp, mm);
               break;
            }
            fmad16(c, href(t, 0), mm);
            lane[q] = href(t, 0);
         }
         if (l1 == l0)
            break;
         struct sgx_ins *i = ins_alloc(c, SGX_INS_ALU, SGX_OP_PCK_F16_F16);
         i->nops = 3;
         i->op[0] = dp;
         i->op[1] = lane[0];
         i->op[2] = lane[1];
         add(c, i);
         break;
      }
      case nir_op_frcp: case nir_op_frsq: case nir_op_fsqrt: case nir_op_fexp2: case nir_op_flog2: {
         /* F16 source and destination: the result lands in the source's half
          * of a temporary (the other half zeroed), then packed */
         unsigned form = alu->op == nir_op_frcp ? SGX_OP_FRCP : alu->op == nir_op_frsq ? SGX_OP_FRSQ :
                         alu->op == nir_op_fsqrt ? SGX_OP_FSQRT : alu->op == nir_op_fexp2 ? SGX_OP_FEXP2 : SGX_OP_FLOG2;
         struct sgx_ref lane[2];
         for (unsigned q = 0; q < 2; q++) {
            unsigned li = q ? l1 : l0;
            struct sgx_ref x = s[0][li];
            if (!x.h16 || x.file == SGX_F_K)
               x = f32_to_h(c, x);
            bool hi = x.file == SGX_F_V ? (x.comp & 1) : (x.n & 1);
            unsigned t = new_vreg16(c, 2);
            struct sgx_ins *i = ins_alloc(c, SGX_INS_ALU, form);
            i->nops = 2;
            i->op[0] = href(t, 0);
            i->op[1] = x;
            fld(i, SGX_FLD_F16D, 1);
            emit(c, i);
            lane[q] = href(t, hi);
            if (nc == 1) {
               /* a scalar: the half it landed in is the value */
               c->vals[alu->def.index].n = t;
               c->vals[alu->def.index].comp = hi;
               break;
            }
         }
         if (nc == 1)
            break;
         struct sgx_ins *i = ins_alloc(c, SGX_INS_ALU, SGX_OP_PCK_F16_F16);
         i->nops = 3;
         i->op[0] = dp;
         i->op[1] = lane[0];
         i->op[2] = lane[1];
         add(c, i);
         break;
      }
      default:
         sgx_fail(c, "unsupported 16-bit ALU op %s", nir_op_infos[alu->op].name);
         return;
      }
   }
}


static void
emit_alu(struct sgx_ctx *c, nir_alu_instr *alu)
{
   if ((alu->op == nir_op_fneg || alu->op == nir_op_fabs) && nir_legacy_float_mod_folds(alu))
      return;
   /* (the F16 path doesn't take a folded fsat on its destination) */
   if (alu->op == nir_op_fsat && alu->def.bit_size == 32 && !c->def_dest && nir_legacy_fsat_folds(alu))
      return;

   if (alu->def.bit_size == 16) {
      emit_alu16(c, alu);
      return;
   }
   if (alu->op == nir_op_f2f32) {
      /* a 32-bit view of F16 halves: the users read them as F16 operands */
      struct sgx_ref r[4];
      for (unsigned k = 0; k < alu->def.num_components; k++)
         r[k] = alu_src(c, alu, 0, k);
      nir_legacy_alu_dest fd = nir_legacy_chase_alu_dest(&alu->def);
      if (!fd.dest.is_ssa && !c->def_dest) {
         /* stored to a 32-bit register (the colour, dEQP random
          * trigonometric.72): widen into it, x.f16 - floor(0) */
         unsigned vreg, base;
         def_dest(c, &alu->def, &fd.dest, &vreg, &base);
         struct sgx_ref d[4], ss[2][4];
         for (unsigned k = 0; k < 4; k++) {
            d[k] = vref(vreg, base + k);
            ss[0][k] = (fd.write_mask >> k & 1) ? const_f32(r[k]) : ref(SGX_F_C, 48);
            ss[1][k] = ref(SGX_F_C, 48);
         }
         emit_runs(c, SGX_OP_FSUBFLR, fd.write_mask, d, 2, ss, &NOF, 0);
         return;
      }
      set_refs(c, &alu->def, r, alu->def.num_components);
      return;
   }
   nir_legacy_alu_dest ld = nir_legacy_chase_alu_dest(&alu->def);
   if (c->def_dest) {
      /* (an F16 op through f32: its own def, not a chased register or fsat) */
      ld.dest.is_ssa = true;
      ld.dest.ssa = &alu->def;
      ld.write_mask = nir_component_mask(alu->def.num_components);
      ld.fsat = false;
   }
   unsigned vreg, base;
   unsigned nc = alu->def.num_components;
   unsigned wmask = ld.dest.is_ssa ? nir_component_mask(nc) : ld.write_mask;
   unsigned nin = nir_op_infos[alu->op].num_inputs;

   if (alu->op == nir_op_mov && ld.dest.is_ssa && !ld.fsat) {
      /* a swizzle of an SSA value (nir_lower_vec_to_regs' vec4(x)): an
       * alias of its components, not a register copy */
      struct sgx_ref r[4];
      for (unsigned k = 0; k < nc; k++)
         r[k] = alu_src(c, alu, 0, k);
      set_refs(c, &alu->def, r, nc);
      return;
   }
   if (alu->op == nir_op_mov && ld.dest.is_ssa && !ld.fsat) {
      /* a swizzle of F16 halves (a mediump varying's v.yx as a coordinate) is
       * the same 32-bit view */
      struct sgx_ref r[4];
      bool halves = true;
      for (unsigned k = 0; k < nc; k++) {
         r[k] = alu_src(c, alu, 0, k);
         halves &= r[k].h16;
      }
      if (halves) {
         set_refs(c, &alu->def, r, nc);
         return;
      }
   }

   /* dot products: one fdp over all components, result in the last iteration's
    * destination: give it a vreg as wide as the inputs */
   if (alu->op == nir_op_fdot2 || alu->op == nir_op_fdot3 || alu->op == nir_op_fdot4) {
      unsigned n = alu->op == nir_op_fdot2 ? 2 : alu->op == nir_op_fdot3 ? 3 : 4;
      unsigned w = new_vreg(c, n);
      struct sgx_ref s[2][4], d[4];
      for (unsigned k = 0; k < n; k++) {
         d[k] = vref(w, k);
         s[0][k] = const_f32(alu_src(c, alu, 0, k));
         s[1][k] = const_f32(alu_src(c, alu, 1, k));
      }
      /* both sources must be one run: copy any that isn't */
      for (unsigned j = 0; j < 2; j++) {
         bool st = true, hd = true;
         for (unsigned k = 1; k < n; k++) {
            st &= ref_next(&s[j][k - 1], &s[j][k]);
            hd &= ref_same(&s[j][k - 1], &s[j][k]);
         }
         /* constants: a run of literals in SA */
         bool kk = c->opts->driver_consts && !st && !hd;
         uint32_t kw[4];
         for (unsigned k = 0; k < n && kk; k++) {
            kk = s[j][k].file == SGX_F_K && !s[j][k].h16;
            kw[k] = s[j][k].n;
            if (s[j][k].abs)
               kw[k] &= 0x7fffffff;
            if (s[j][k].neg)
               kw[k] ^= 0x80000000u;
         }
         int ln = kk ? lit_run(c, kw, n) : -1;
         if (ln >= 0) {
            for (unsigned k = 0; k < n; k++)
               s[j][k] = ref(SGX_F_LIT, ln + k);
            continue;
         }
         if (!st && !hd) {
            unsigned t = new_vreg(c, n);
            struct sgx_ref td[4], ts[1][4];
            for (unsigned k = 0; k < n; k++) {
               td[k] = vref(t, k);
               ts[0][k] = s[j][k];
               ts[0][k].neg = ts[0][k].abs = 0;
            }
            /* keep the modifiers (all components alike in NIR) on the fdp */
            bool ng = s[j][0].neg, ab = s[j][0].abs;
            emit_runs(c, SGX_OP_MOV, nir_component_mask(n), td, 1, ts, &NOF, 0);
            for (unsigned k = 0; k < n; k++) {
               s[j][k] = vref(t, k);
               s[j][k].neg = ng;
               s[j][k].abs = ab;
            }
         }
      }
      emit_runs(c, SGX_OP_FDP, nir_component_mask(n), d, 2, s, &NOF, 0);
      /* fdp writes only the last iteration's destination (ISA.md hwt_p7): its
       * operand names that register alone, the encoder takes rpt - 1 off,
       * and nothing holds the ones below it */
      unsigned w1 = new_vreg(c, 1);
      struct sgx_ins *dp = list_last_entry(&c->ins, struct sgx_ins, link);
      assert(dp->form == SGX_OP_FDP);
      dp->op[0] = vref(w1, 0);
      dp->op[0].step = 0;
      /* the result (clamped to [0, 1] when NIR folded an fsat into it),
       * moved where NIR wants it */
      struct sgx_ref res = vref(w1, 0);
      if (ld.fsat) {
         unsigned t = new_vreg(c, 1);
         struct sgx_ref td[4] = { vref(t, 0) }, ts[3][4] = { { res }, { ref(SGX_F_K, 0) }, { ref(SGX_F_K, 0x3f800000) } };
         emit_runs(c, SGX_OP_FMINMAX, 1, td, 3, ts, &NOF, 0);
         res = vref(t, 0);
      }
      if (ld.dest.is_ssa) {
         /* (the chased def: the fsat's, when one was folded) */
         struct sgx_val *v = &c->vals[ld.dest.ssa->index];
         v->kind = VAL_V;
         v->n = res.n;
         v->comp = res.comp;
         if (nc > 1)
            sgx_fail(c, "vector dot product result");
      } else {
         def_dest(c, &alu->def, &ld.dest, &vreg, &base);
         struct sgx_ref dd[4], ss[1][4];
         for (unsigned k = 0; k < 4; k++) {
            dd[k] = vref(vreg, base + k);
            ss[0][k] = res;
         }
         for (unsigned k = 0; k < 4; k++)
            if (wmask & (1u << k))
               emit_runs(c, SGX_OP_MOV, 1u << k, dd, 1, ss, &NOF, 0);
      }
      return;
   }

   def_dest(c, &alu->def, &ld.dest, &vreg, &base);
   struct sgx_ref d[4], s[3][4];
   for (unsigned k = 0; k < 4; k++) {
      d[k] = vref(vreg, base + k);
      for (unsigned j = 0; j < nin && j < 3; j++)
         s[j][k] = (wmask & (1u << k)) ? const_f32(alu_src(c, alu, j, k)) : ref(SGX_F_C, 48);
   }

   struct sgx_ref ops[3][4];
   struct fieldset fs = { 0 };
   unsigned form, n = 0;

#define SRC(j, x) do { for (unsigned k = 0; k < 4; k++) ops[j][k] = (x); } while (0)
   switch (alu->op) {
   case nir_op_mov:
      form = SGX_OP_MOV;
      n = 1;
      SRC(0, s[0][k]);
      /* (mov has no format select: an F16 half widens through fsubflr too) */
      for (unsigned k = 0; k < 4; k++)
         if ((wmask & (1u << k)) && (s[0][k].neg || s[0][k].abs || s[0][k].h16))
            form = SGX_OP_FSUBFLR;
      if (form == SGX_OP_FSUBFLR) {
         n = 2;
         SRC(1, ref(SGX_F_C, 48));
      }
      break;
   case nir_op_fneg:
   case nir_op_fabs:
      form = SGX_OP_FSUBFLR;
      n = 2;
      SRC(0, s[0][k]);
      for (unsigned k = 0; k < 4; k++) {
         if (alu->op == nir_op_fneg)
            ops[0][k].neg ^= 1;
         else
            ops[0][k].abs = 1, ops[0][k].neg = 0;
      }
      SRC(1, ref(SGX_F_C, 48));
      break;
   case nir_op_fadd:
   case nir_op_fmul:
   case nir_op_fmad:
   case nir_op_ffma:
   case nir_op_ffma_weak: {
      /* fmad s0 * s1 + s2; src0 must be r/pa and has no negate */
      struct sgx_ref (*a)[4] = &s[0], (*b)[4] = &s[1];
      if (alu->op == nir_op_fadd) {
         SRC(0, s[0][k]);
         SRC(1, ref(SGX_F_K, 0x3f800000));
         SRC(2, s[1][k]);
         if (!is_rpa(&s[0][ffs(wmask) - 1]) && is_rpa(&s[1][ffs(wmask) - 1])) {
            SRC(0, s[1][k]);
            SRC(2, s[0][k]);
         }
      } else {
         if (!is_rpa(&(*a)[ffs(wmask) - 1]) && is_rpa(&(*b)[ffs(wmask) - 1])) {
            struct sgx_ref (*t)[4] = a;
            a = b;
            b = t;
         }
         SRC(0, (*a)[k]);
         SRC(1, (*b)[k]);
         if (alu->op == nir_op_fmul) {
            SRC(2, ref(SGX_F_K, 0x80000000));
         } else {
            SRC(2, s[2][k]);
         }
      }
      for (unsigned k = 0; k < 4; k++) {
         if (ops[0][k].neg) {
            ops[0][k].neg = 0;
            ops[1][k].neg ^= 1;
         }
      }
      form = SGX_OP_FMAD;
      n = 3;
      break;
   }
   case nir_op_fmin:
   case nir_op_fmax: {
      /* fminmax (a, a, b) = min; fmaxmin (a, b, a) = max */
      struct sgx_ref (*a)[4] = &s[0], (*b)[4] = &s[1];
      unsigned f0 = ffs(wmask) - 1;
      if ((!is_rpa(&(*a)[f0]) || (*a)[f0].neg) && is_rpa(&(*b)[f0]) && !(*b)[f0].neg) {
         a = &s[1];
         b = &s[0];
      }
      if (alu->op == nir_op_fmin) {
         form = SGX_OP_FMINMAX;
         SRC(0, (*a)[k]);
         SRC(1, (*a)[k]);
         SRC(2, (*b)[k]);
      } else {
         form = SGX_OP_FMAXMIN;
         SRC(0, (*a)[k]);
         SRC(1, (*b)[k]);
         SRC(2, (*a)[k]);
      }
      n = 3;
      break;
   }
   case nir_op_fsat:
      form = SGX_OP_FMINMAX;
      SRC(0, s[0][k]);
      SRC(1, ref(SGX_F_K, 0));
      SRC(2, ref(SGX_F_K, 0x3f800000));
      n = 3;
      break;
   case nir_op_frcp: form = SGX_OP_FRCP; goto scalar;
   case nir_op_frsq: form = SGX_OP_FRSQ; goto scalar;
   case nir_op_fsqrt: form = SGX_OP_FSQRT; goto scalar;
   case nir_op_fexp2: form = SGX_OP_FEXP2; goto scalar;
   case nir_op_flog2: form = SGX_OP_FLOG2; goto scalar;
   case nir_op_fsin_agx: form = SGX_OP_FSIN; goto scalar;
   scalar:
      SRC(0, s[0][k]);
      n = 1;
      break;
   case nir_op_ffract:
      form = SGX_OP_FSUBFLR;
      SRC(0, s[0][k]);
      SRC(1, s[0][k]);
      n = 2;
      break;
   case nir_op_ffloor: {
      /* x - fract(x) */
      unsigned t = new_vreg(c, 4);
      struct sgx_ref td[4], ts[2][4];
      for (unsigned k = 0; k < 4; k++) {
         td[k] = vref(t, k);
         ts[0][k] = ts[1][k] = s[0][k];
      }
      emit_runs(c, SGX_OP_FSUBFLR, wmask, td, 2, ts, &NOF, 0);
      form = SGX_OP_FMAD;
      SRC(0, td[k]);
      SRC(1, ref(SGX_F_K, 0xbf800000));
      SRC(2, s[0][k]);
      n = 3;
      break;
   }
   case nir_op_ftrunc: {
      /* sign(x) * floor(|x|): f = fract(|x|); a = |x| - f; x < 0 ? -a : a */
      unsigned f = new_vreg(c, 4), a = new_vreg(c, 4), na = new_vreg(c, 4);
      struct sgx_ref fd[4], ad[4], nd[4], ts[3][4];
      for (unsigned k = 0; k < 4; k++) {
         fd[k] = vref(f, k);
         ad[k] = vref(a, k);
         nd[k] = vref(na, k);
         ts[0][k] = ts[1][k] = s[0][k];
         ts[0][k].abs = ts[1][k].abs = 1;
         ts[0][k].neg = ts[1][k].neg = 0;
      }
      emit_runs(c, SGX_OP_FSUBFLR, wmask, fd, 2, ts, &NOF, 0);
      for (unsigned k = 0; k < 4; k++) {
         ts[0][k] = fd[k];
         ts[1][k] = ref(SGX_F_K, 0xbf800000);
         ts[2][k] = s[0][k];
         ts[2][k].abs = 1;
         ts[2][k].neg = 0;
      }
      emit_runs(c, SGX_OP_FMAD, wmask, ad, 3, ts, &NOF, 0);
      for (unsigned k = 0; k < 4; k++) {
         ts[0][k] = ad[k];
         ts[1][k] = ref(SGX_F_K, 0xbf800000);
         ts[2][k] = ref(SGX_F_K, 0x80000000);
      }
      emit_runs(c, SGX_OP_FMAD, wmask, nd, 3, ts, &NOF, 0);
      form = SGX_OP_MOVC_F32;
      fs.n = 1;
      fs.f[0].id = SGX_FLD_COND;
      fs.f[0].v = 2;   /* ltz */
      SRC(0, s[0][k]);
      SRC(1, nd[k]);
      SRC(2, ad[k]);
      n = 3;
      break;
   }
   case nir_op_slt:
   case nir_op_sge:
   case nir_op_seq:
   case nir_op_sne: {
      /* testmask.fsub.<cond>.dword m, a, b; movc.nz.i32 d, m, 1.0, 0.0 */
      static const int cond[] = { [nir_op_slt] = 25 };
      int cd = alu->op == nir_op_slt ? 25 : alu->op == nir_op_sge ? 6 : alu->op == nir_op_seq ? 20 : 24;
      (void)cond;
      unsigned t = new_vreg(c, 4);
      struct sgx_ref td[4], ts[2][4];
      for (unsigned k = 0; k < 4; k++) {
         td[k] = vref(t, k);
         ts[0][k] = s[0][k];
         ts[1][k] = s[1][k];
      }
      struct fieldset tf = { 3, { { SGX_FLD_ALU, 14 }, { SGX_FLD_COND, cd }, { SGX_FLD_TYPE, 2 } } };
      emit_runs(c, SGX_OP_TESTMASK, wmask, td, 2, ts, &tf, 0);
      if (ld.dest.is_ssa && !ld.fsat && cond_only(&alu->def)) {
         /* only selects read it: the mask itself (a passing dword test
          * writes 0xffffffff, which movc.nz takes as true: ISA.md hwt_p2/p7) */
         c->vals[alu->def.index].kind = VAL_V;
         c->vals[alu->def.index].n = t;
         c->vals[alu->def.index].comp = 0;
         return;
      }
      form = SGX_OP_MOVC_I32;
      fs.n = 1;
      fs.f[0].id = SGX_FLD_COND;
      fs.f[0].v = 1;
      SRC(0, td[k]);
      SRC(1, ref(SGX_F_K, 0x3f800000));
      SRC(2, ref(SGX_F_K, 0));
      n = 3;
      break;
   }
   case nir_op_fcsel:
   case nir_op_b32csel:
      if (ld.dest.is_ssa && !ld.fsat && f16_pairs(c, alu) && csel16(c, alu, s))
         return;
      form = SGX_OP_MOVC_F32;
      fs.n = 1;
      fs.f[0].id = SGX_FLD_COND;
      fs.f[0].v = 1;
      SRC(0, cond_f32(c, s[0][k]));
      SRC(1, s[1][k]);
      SRC(2, s[2][k]);
      n = 3;
      break;
   default:
      sgx_fail(c, "unsupported ALU op %s", nir_op_infos[alu->op].name);
      return;
   }
#undef SRC
   emit_runs(c, form, wmask, d, n, ops, &fs, 0);
   if (ld.fsat) {
      struct sgx_ref ss[3][4];
      for (unsigned k = 0; k < 4; k++) {
         ss[0][k] = d[k];
         ss[1][k] = ref(SGX_F_K, 0);
         ss[2][k] = ref(SGX_F_K, 0x3f800000);
      }
      emit_runs(c, SGX_OP_FMINMAX, wmask, d, 3, ss, &NOF, 0);
   }
}

/* ------------------------------------------------------------------------
 * intrinsics */

static void
copy_out(struct sgx_ctx *c, struct sgx_ref dst0, nir_src *src, unsigned wmask)
{
   nir_legacy_src ls = nir_legacy_chase_src(src);
   struct sgx_ref d[4], s[1][4];
   for (unsigned k = 0; k < 4; k++) {
      d[k] = dst0;
      if (d[k].file == SGX_F_V)
         d[k].comp += k;
      else
         d[k].n += k;
      s[0][k] = (wmask & (1u << k)) ? src_comp(c, &ls, k) : ref(SGX_F_C, 48);
   }
   emit_runs(c, SGX_OP_MOV, wmask, d, 1, s, &NOF, 0);
}

static struct sgx_ins *
emit_test(struct sgx_ctx *c, unsigned p, struct sgx_ref a, struct sgx_ref b, int alu, int cond)
{
   struct sgx_ins *t = ins_alloc(c, SGX_INS_ALU, SGX_OP_TEST);
   t->nops = 3;
   t->ndst = 0;
   t->op[0] = ref(SGX_F_P, p);
   t->op[1] = a;
   t->op[2] = b;
   t->writes_pred = p + 1;
   fld(t, SGX_FLD_ALU, alu);
   fld(t, SGX_FLD_COND, cond);
   return emit(c, t);
}

static void
discard(struct sgx_ctx *c, nir_intrinsic_instr *in, bool cond)
{
   if (cond && c->discard_cmp) {
      /* the only discard, on one comparison: the keep predicate p1 directly
       * (the inverse condition), as libusc writes it */
      nir_alu_instr *cmp = c->discard_cmp;
      int inv = cmp->op == nir_op_sge ? 25 : cmp->op == nir_op_slt ? 6 : cmp->op == nir_op_seq ? 24 : 20;
      emit_test(c, 1, alu_src(c, cmp, 0, 0), alu_src(c, cmp, 1, 0), 14, inv);
      c->discards = true;
      c->keep_pred = true;
      return;
   }
   if (c->keep_vreg < 0) {
      c->keep_vreg = new_vreg(c, 1);
      struct sgx_ins *m = ins_alloc(c, SGX_INS_ALU, SGX_OP_MOV);
      m->nops = 2;
      m->op[0] = vref(c->keep_vreg, 0);
      m->op[1] = cref(52);
      list_add(&m->link, &c->ins);   /* at the start of the program */
   }
   c->discards = true;
   struct sgx_ref d[4], s[3][4];
   struct fieldset fs = { 1, { { SGX_FLD_COND, 1 } } };
   d[0] = vref(c->keep_vreg, 0);
   if (cond) {
      nir_legacy_src ls = nir_legacy_chase_src(&in->src[0]);
      s[0][0] = src_comp(c, &ls, 0);
      s[1][0] = ref(SGX_F_K, 0);
      s[2][0] = vref(c->keep_vreg, 0);
      emit_runs(c, SGX_OP_MOVC_F32, 1, d, 3, s, &fs, 0);
   } else {
      s[0][0] = cref(48);
      emit_runs(c, SGX_OP_MOV, 1, d, 1, s, &NOF, 0);
   }
}

/* gl_FrontFacing as 1.0 / 0.0 from the 0 / 1 in t, put in after `after` */
static unsigned
face_float(struct sgx_ctx *c, unsigned t, struct list_head *after)
{
   unsigned f = new_vreg(c, 1);
   struct sgx_ins *m = ins_alloc(c, SGX_INS_ALU, SGX_OP_MOVC_I32);
   m->nops = 4;
   m->op[0] = vref(f, 0);
   m->op[1] = vref(t, 0);
   m->op[2] = cref(52);
   m->op[3] = cref(48);
   fld(m, SGX_FLD_COND, 1);
   list_add(&m->link, after);
   return f;
}

/* A select's condition: the integer face value only movc.i32 reads as such;
 * movc.f32 takes 1 for a denormal, so it gets the float. */
static struct sgx_ref
cond_f32(struct sgx_ctx *c, struct sgx_ref r)
{
   if (c->face_int && r.file == SGX_F_V && r.n == (unsigned)c->face_vreg) {
      if (c->face_f32 < 0)
         c->face_f32 = face_float(c, c->face_vreg, &c->face_and->link);
      r.n = c->face_f32;
      r.comp = 0;
   }
   return r;
}

/* gl_FrontFacing = ((g16 ^ sa[front_sa]) & 1) != 0, once, at program entry:
 * 1.0 / 0.0, or the and's 0 / 1 when only selects read it */
static void
front_face(struct sgx_ctx *c, struct sgx_val *v, nir_def *def)
{
   if (c->face_vreg < 0) {
      bool cond_only = true;
      nir_foreach_use_including_if(u, def) {
         nir_instr *ui = nir_src_is_if(u) ? NULL : nir_src_use_instr(u);
         cond_only &= ui && ui->type == nir_instr_type_alu &&
                      (nir_instr_as_alu(ui)->op == nir_op_fcsel || nir_instr_as_alu(ui)->op == nir_op_b32csel) &&
                      u == &nir_instr_as_alu(ui)->src[0].src;
      }
      unsigned t = new_vreg(c, 1);
      struct sgx_ins *x = ins_alloc(c, SGX_INS_ALU, SGX_OP_XOR);
      x->nops = 3;
      x->op[0] = vref(t, 0);
      x->op[1] = ref(SGX_F_G, 16);
      x->op[2] = ref(SGX_F_SA, c->opts->front_sa);
      struct sgx_ins *a = ins_alloc(c, SGX_INS_ALU, SGX_OP_AND);
      a->nops = 3;
      a->op[0] = vref(t, 0);
      a->op[1] = vref(t, 0);
      a->op[2] = ref(SGX_F_IMM, 1);
      list_add(&a->link, &c->ins);
      list_add(&x->link, &c->ins);
      c->face_and = a;
      c->face_int = cond_only;
      c->face_vreg = cond_only ? (int)t : (int)face_float(c, t, &a->link);
   }
   v->kind = VAL_V;
   v->n = c->face_vreg;
   v->comp = 0;
}

/* A vertex attribute in a raw format (opts.vtx_fmt): the components the
 * shader reads, unpacked from PA[4i] into f32; those the format lacks are
 * 0, 0, 1 */
static void
vtx_unpack(struct sgx_ctx *c, nir_intrinsic_instr *in, int r, unsigned fmt)
{
   unsigned base = r & ~3, nc = in->def.num_components;
   unsigned held = fmt == SGX_VF_F32_1 ? 1 : fmt == SGX_VF_F32_2 || fmt == SGX_VF_SSCALED16_2 ? 2 :
                   fmt == SGX_VF_F32_3 ? 3 : 4;
   nir_component_mask_t rd = nir_def_components_read(&in->def);
   unsigned t = fmt >= SGX_VF_UNORM8_4 ? new_vreg(c, nc) : 0;
   struct sgx_ref x[4];
   for (unsigned k = 0; k < nc; k++) {
      unsigned comp = (r & 3) + k;
      if (comp >= held) {
         x[k] = ref(SGX_F_K, comp == 3 ? 0x3f800000 : 0);
         continue;
      }
      if (fmt < SGX_VF_UNORM8_4) {
         x[k] = ref(SGX_F_PA, base + comp);
         continue;
      }
      x[k] = vref(t, k);
      if (!(rd >> k & 1))
         continue;
      /* pck.f32.u8 [scale] takes byte n (/255), pck.f32.s16 half n / 2 */
      struct sgx_ins *p = ins_alloc(c, SGX_INS_ALU, fmt == SGX_VF_UNORM8_4 ? SGX_OP_PCK_F32_U8 : SGX_OP_PCK_F32_S16);
      p->nops = 3;
      p->op[0] = vref(t, k);
      p->op[1] = ref(SGX_F_PA, base);
      p->op[1].lane = fmt == SGX_VF_UNORM8_4 ? comp : 2 * comp;
      p->op[2] = ref(SGX_F_IMM, 0);
      if (fmt == SGX_VF_UNORM8_4)
         fld(p, SGX_FLD_SCALE, 1);
      emit(c, p);
   }
   set_refs(c, &in->def, x, nc);
}

static void
emit_intrinsic(struct sgx_ctx *c, nir_intrinsic_instr *in)
{
   struct sgx_val *v = nir_intrinsic_infos[in->intrinsic].has_dest ? &c->vals[in->def.index] : NULL;
   switch (in->intrinsic) {
   case nir_intrinsic_decl_reg: {
      unsigned n = nir_intrinsic_num_components(in) * MAX2(nir_intrinsic_num_array_elems(in), 1);
      v->kind = VAL_V;
      if (_mesa_set_search(c->shadow_regs, in)) {
         v->shadow = ralloc_array(c, struct sgx_ref, 4);
         for (unsigned k = 0; k < 4; k++) {
            v->shadow[k] = ref(SGX_F_K, 0);
            v->shadow[k].h16 = 1;
         }
      }
      v->n = nir_intrinsic_bit_size(in) == 16 ? new_vreg16(c, ALIGN_POT(n, 2)) : new_vreg(c, n);
      return;
   }
   case nir_intrinsic_load_reg:
   case nir_intrinsic_store_reg: {
      if (in->intrinsic == nir_intrinsic_load_reg)
         return;     /* chased by the users */
      /* a store whose value no instruction wrote into the register: copy */
      nir_def *val = in->src[0].ssa;
      nir_instr *p = nir_def_instr(val);
      if (val->bit_size == 16) {
         /* F16 halves: pck.f16.f16 per register, writing only the halves the
          * mask names (dmask 3 low, 0xc high: the first enabled half takes src1) */
         struct sgx_val *rv = &c->vals[in->src[1].ssa->index];
         nir_legacy_src ls = nir_legacy_chase_src(&in->src[0]);
         unsigned wm = nir_intrinsic_write_mask(in), base = nir_intrinsic_base(in) * 4;
         if (rv->shadow) {
            for (unsigned k = 0; k < 4; k++)
               if (wm >> k & 1)
                  rv->shadow[k] = src_comp(c, &ls, k);
            return;
         }
         /* the second pair reading the first after it was written (r = r.yzwx
          * in a loop): read the first pair from a copy */
         struct sgx_ref sr[4];
         for (unsigned k = 0; k < 4; k++)
            sr[k] = (wm >> k & 1) ? src_comp(c, &ls, k) : ref(SGX_F_IMM, 0);
         struct sgx_ref first = href(rv->n, base);
         if ((wm & 3) && (wm & 0xc) &&
             (same_vreg_reg(&sr[2], &first) || same_vreg_reg(&sr[3], &first))) {
            unsigned t = new_vreg16(c, 2);
            struct sgx_ins *pk = ins_alloc(c, SGX_INS_ALU, SGX_OP_PCK_F16_F16);
            pk->nops = 3;
            pk->op[0] = href(t, 0);
            pk->op[1] = href(rv->n, base);
            pk->op[2] = href(rv->n, base + 1);
            add(c, pk);
            for (unsigned k = 2; k < 4; k++)
               if (same_vreg_reg(&sr[k], &first))
                  sr[k].n = t, sr[k].comp -= base;
         }
         for (unsigned pr = 0; pr < 2; pr++) {
            unsigned m = wm >> (2 * pr) & 3;
            if (!m)
               continue;
            struct sgx_ins *pk = ins_alloc(c, SGX_INS_ALU, SGX_OP_PCK_F16_F16);
            pk->nops = 3;
            pk->op[0] = href(rv->n, base + 2 * pr);
            struct sgx_ref lo = sr[2 * pr], hi = sr[2 * pr + 1];
            if (m & 1)
               lo = store_h(c, lo);
            if (m & 2)
               hi = store_h(c, hi);
            pk->op[1] = m == 2 ? hi : lo;
            pk->op[2] = m == 3 ? hi : ref(SGX_F_IMM, 0);
            pk->op[1].neg = pk->op[1].abs = pk->op[2].neg = pk->op[2].abs = 0;
            if (m != 3)
               fld(pk, SGX_FLD_DMASK, m == 1 ? 3 : 0xc);
            add(c, pk);
         }
         return;
      }
      if (p->type == nir_instr_type_alu || p->type == nir_instr_type_tex) {
         nir_legacy_dest ld = p->type == nir_instr_type_alu ? nir_legacy_chase_alu_dest(val).dest
                                                            : nir_legacy_chase_dest(val);
         /* (only when it really wrote the register: a texture's unpack and
          * the aliasing paths leave the value somewhere of its own, dEQP
          * random all_features.5's texture2DProj colour) */
         if (!ld.is_ssa && c->vals[val->index].kind == VAL_NONE)
            return;
      }
      if (p->type == nir_instr_type_intrinsic && nir_instr_as_intrinsic(p)->intrinsic == nir_intrinsic_load_reg) {
         /* reg to reg */
      }
      struct sgx_val *r = &c->vals[in->src[1].ssa->index];
      copy_out(c, vref(r->n, nir_intrinsic_base(in) * 4), &in->src[0], nir_intrinsic_write_mask(in));
      return;
   }
   case nir_intrinsic_load_uniform: {
      if (!nir_src_is_const(in->src[0])) {
         /* a[i]: pck.s16.f32 t, offset; ima16.sgn idx1, t, #1, #<SA of the
          * array>; then each component from sa[idx1 + j] (libusc's pattern,
          * ISA.md "What GLSL integer code compiles to") */
         unsigned k = nir_intrinsic_base(in), range = nir_intrinsic_range(in);
         for (unsigned j = 0; j < range && k + j < ARRAY_SIZE(c->const_used); j++)
            c->const_used[k + j] = 1;
         unsigned a = 0;
         while (a < c->n_ix_arr && c->ix_arr[a].k != k)
            a++;
         if (a == ARRAY_SIZE(c->ix_arr)) {
            sgx_fail(c, "too many indexed uniform arrays");
            return;
         }
         c->ix_arr[a].k = k;
         c->ix_arr[a].range = MAX2(a < c->n_ix_arr ? c->ix_arr[a].range : 0, range);
         c->n_ix_arr = MAX2(c->n_ix_arr, a + 1);
         nir_legacy_src lo = nir_legacy_chase_src(&in->src[0]);
         unsigned t = new_vreg(c, 1);
         struct sgx_ins *pk = ins_alloc(c, SGX_INS_ALU, SGX_OP_PCK_S16_F32);
         pk->nops = 3;
         pk->op[0] = vref(t, 0);
         pk->op[1] = src_comp(c, &lo, 0);
         pk->op[2] = ref(SGX_F_IMM, 0);
         fld(pk, SGX_FLD_DMASK, 3);
         emit(c, pk);
         struct sgx_ins *ix = ins_alloc(c, SGX_INS_ALU, SGX_OP_IMA16);
         ix->nops = 4;
         ix->op[0] = ref(SGX_F_IDX, 1);
         ix->op[1] = vref(t, 0);
         ix->op[2] = ref(SGX_F_IMM, 1);
         ix->op[3] = ref(SGX_F_USLOT, k);
         fld(ix, SGX_FLD_SGN, 1);
         fld(ix, SGX_FLD_F1, 1);
         fld(ix, SGX_FLD_F2, 1);
         add(c, ix);
         unsigned w = new_vreg(c, in->def.num_components);
         for (unsigned j = 0; j < in->def.num_components; j++) {
            struct sgx_ins *m = ins_alloc(c, SGX_INS_ALU, SGX_OP_MOV);
            m->nops = 2;
            m->op[0] = vref(w, j);
            m->op[1] = ref(SGX_F_IXSA, j);
            add(c, m);
         }
         v->kind = VAL_V;
         v->n = w;
         v->comp = 0;
         return;
      }
      v->kind = VAL_U;
      /* (nir_lower_int_to_float has turned the offset into a float) */
      uint32_t off = nir_src_as_uint(in->src[0]);
      if (off >= 0x10000)
         off = (uint32_t)uif(off);
      v->n = nir_intrinsic_base(in) + off;
      if (v->n < ARRAY_SIZE(c->const_fmt) && c->const_fmt[v->n] == 3) {
         /* C10 in SA (as the DDK loads a lowp vector): to F16 with
          * pck.f16.c10 [scale], channel n at bits 10n holds constant
          * k + 2 - n (corpus mc_texfog) */
         unsigned k = v->n, nc = 0;
         while (nc < in->def.num_components && nc < 3 && c->const_fmt[k + nc] == 3)
            c->const_used[k + nc++] = 1;
         unsigned t = new_vreg16(c, 4);
         struct sgx_ref r[4];
         for (unsigned p = 0; p < (nc + 1) / 2; p++) {
            struct sgx_ins *i = ins_alloc(c, SGX_INS_ALU, SGX_OP_PCK_F16_C10);
            i->nops = 3;
            i->op[0] = href(t, 2 * p);
            i->op[1] = ref(SGX_F_U, k + 2 * p);
            i->op[1].lane = 2 - 2 * p;
            if (2 * p + 1 < nc) {
               i->op[2] = ref(SGX_F_U, k + 2 * p + 1);
               i->op[2].lane = 1 - 2 * p;
            } else {
               i->op[2] = ref(SGX_F_IMM, 0);
               fld(i, SGX_FLD_DMASK, 3);
            }
            fld(i, SGX_FLD_SCALE, 1);
            add(c, i);
         }
         for (unsigned j = 0; j < in->def.num_components; j++)
            r[j] = j < nc ? href(t, j) : ref(SGX_F_K, 0);
         set_refs(c, &in->def, r, in->def.num_components);
      }
      if (v->n + in->def.num_components > ARRAY_SIZE(c->const_used))
         sgx_fail(c, "uniform constant %u out of range", v->n);
      return;
   }
   case nir_intrinsic_load_input:
   case nir_intrinsic_load_interpolated_input: {
      if (c->stage == MESA_SHADER_FRAGMENT && nir_intrinsic_io_semantics(in).location == VARYING_SLOT_FACE) {
         front_face(c, v, &in->def);
         return;
      }
      nir_src *off = &in->src[in->intrinsic == nir_intrinsic_load_input ? 0 : 1];
      if (!nir_src_is_const(*off)) {
         sgx_fail(c, "indirect input load");
         return;
      }
      uint32_t o = nir_src_as_uint(*off);
      unsigned slot = nir_intrinsic_io_semantics(in).location + (o >= 0x10000 ? (uint32_t)uif(o) : o);
      unsigned at = slot * 4 + nir_intrinsic_component(in);
      int r = at < ARRAY_SIZE(c->in_reg) ? c->in_reg[at] : -1;
      for (unsigned j = 1; j < in->def.num_components; j++)
         if (r >= 0 && (at + j >= ARRAY_SIZE(c->in_reg) || c->in_reg[at + j] != r + (int)j))
            r = -1;
      if (r < 0) {
         sgx_fail(c, "input %u.%u has no register", slot, nir_intrinsic_component(in));
         return;
      }
      if (c->stage == MESA_SHADER_VERTEX && r / 4 < ARRAY_SIZE(c->opts->vtx_fmt) && c->opts->vtx_fmt[r / 4]) {
         vtx_unpack(c, in, r, c->opts->vtx_fmt[r / 4]);
         return;
      }
      v->kind = VAL_PA;
      v->n = r;
      if (r & SGX_IN_HALF) {
         /* iterated as F16: component j in half (2 * pa + comp) + j */
         struct sgx_ref hr[4];
         for (unsigned j = 0; j < in->def.num_components; j++) {
            hr[j] = ref(SGX_F_PA, (r & ~SGX_IN_HALF) + j);
            hr[j].h16 = 1;
         }
         set_refs(c, &in->def, hr, in->def.num_components);
      }
      return;
   }
   case nir_intrinsic_load_front_face:
      front_face(c, v, &in->def);
      return;
   case nir_intrinsic_load_barycentric_pixel:
   case nir_intrinsic_load_barycentric_centroid:
      v->kind = VAL_CONST;
      v->lc = NULL;
      return;
   case nir_intrinsic_store_output: {
      unsigned slot = nir_intrinsic_io_semantics(in).location + nir_src_as_uint(in->src[1]);
      unsigned wmask = nir_intrinsic_write_mask(in) << nir_intrinsic_component(in);
      if (c->stage == MESA_SHADER_FRAGMENT) {
         nir_io_semantics sem = nir_intrinsic_io_semantics(in);
         if (sem.location == FRAG_RESULT_DEPTH) {
            nir_legacy_src ls = nir_legacy_chase_src(&in->src[0]);
            c->depth = src_comp(c, &ls, 0);
            c->has_depth = true;
            return;
         }
         if (sem.location != FRAG_RESULT_COLOR && sem.location != FRAG_RESULT_DATA0) {
            sgx_fail(c, "fragment output %u", sem.location);
            return;
         }
         nir_legacy_src ls = nir_legacy_chase_src(&in->src[0]);
         for (unsigned k = 0; k < 4; k++)
            if (wmask & (1u << k))
               c->color[k] = src_comp(c, &ls, k - nir_intrinsic_component(in));
         c->has_color = true;
         return;
      }
      /* per component: a varying the fragment shader doesn't read, or the
       * linker's default point size, has no register */
      nir_legacy_src ls = nir_legacy_chase_src(&in->src[0]);
      struct sgx_ref d[4], ss[1][4];
      unsigned m = 0;
      if (slot == VARYING_SLOT_PSIZ) {
         /* an f32 output, clamped to the largest point size (docs/STATE.md
          * "Lines and points"): min(x, 511) as libusc writes it, a constant
          * at compile time */
         int r = c->out_reg[VARYING_SLOT_PSIZ * 4];
         if (r < 0)
            return;
         const uint32_t max = 0x43ff8000;
         struct sgx_ref x = const_f32(src_comp(c, &ls, 0));
         struct sgx_ref dd[4] = { ref(SGX_F_O, r) };
         if (x.file == SGX_F_K) {
            union { uint32_t u; float f; } v = { .u = x.n };
            struct sgx_ref ks[1][4] = { { ref(SGX_F_K, v.f > 511.0f ? max : x.n) } };
            emit_runs(c, SGX_OP_MOV, 1, dd, 1, ks, &NOF, 0);
         } else {
            struct sgx_ref ts[3][4] = { { x }, { x }, { ref(SGX_F_K, max) } };
            emit_runs(c, SGX_OP_FMINMAX, 1, dd, 3, ts, &NOF, 0);
         }
         return;
      }
      for (unsigned k = 0; k < 4; k++) {
         d[k] = ss[0][k] = ref(SGX_F_C, 48);
         int r = slot * 4 + k < ARRAY_SIZE(c->out_reg) ? c->out_reg[slot * 4 + k] : -1;
         if (!(wmask & (1u << k)) || r < 0)
            continue;
         d[k] = ref(SGX_F_O, r);
         ss[0][k] = src_comp(c, &ls, k - nir_intrinsic_component(in));
         m |= 1u << k;
      }
      emit_runs(c, SGX_OP_MOV, m, d, 1, ss, &NOF, 0);
      return;
   }
   case nir_intrinsic_ddx:
   case nir_intrinsic_ddx_fine:
   case nir_intrinsic_ddx_coarse:
   case nir_intrinsic_ddy:
   case nir_intrinsic_ddy_fine:
   case nir_intrinsic_ddy_coarse: {
      /* fdsx / fdsy d, x, x per component, as libusc writes them (corpus c3
       * dsx, dsy; st has already flipped dFdy's sign for the target). f32
       * only: an F16 source is format-selected, the result rounded back */
      bool y = in->intrinsic == nir_intrinsic_ddy || in->intrinsic == nir_intrinsic_ddy_fine ||
               in->intrinsic == nir_intrinsic_ddy_coarse;
      nir_legacy_src ls = nir_legacy_chase_src(&in->src[0]);
      unsigned nc = in->def.num_components, w = new_vreg(c, nc);
      struct sgx_ref r[4];
      for (unsigned j = 0; j < nc; j++) {
         struct sgx_ins *d = ins_alloc(c, SGX_INS_ALU, y ? SGX_OP_FDSY : SGX_OP_FDSX);
         d->nops = 3;
         d->op[0] = vref(w, j);
         d->op[1] = d->op[2] = src_comp(c, &ls, j);
         emit(c, d);
         r[j] = in->def.bit_size == 16 ? f32_to_h(c, vref(w, j)) : vref(w, j);
      }
      if (in->def.bit_size == 16) {
         set_refs(c, &in->def, r, nc);
      } else {
         v->kind = VAL_V;
         v->n = w;
         v->comp = 0;
      }
      return;
   }
   case nir_intrinsic_terminate_if:
   case nir_intrinsic_demote_if:
      discard(c, in, true);
      return;
   case nir_intrinsic_terminate:
   case nir_intrinsic_demote:
      discard(c, in, false);
      return;
   default:
      sgx_fail(c, "unsupported intrinsic %s", nir_intrinsic_infos[in->intrinsic].name);
   }
}

/* ------------------------------------------------------------------------
 * control flow */

struct loop_labels {
   unsigned head, exit;
};

static unsigned
new_label(struct sgx_ctx *c)
{
   return c->n_labels++;
}

static void
emit_label(struct sgx_ctx *c, unsigned l)
{
   struct sgx_ins *i = ins_alloc(c, SGX_INS_LABEL, 0);
   i->label = l;
   i->ndst = 0;
   emit(c, i);
}

static void
emit_br(struct sgx_ctx *c, unsigned l, unsigned pred)
{
   struct sgx_ins *i = ins_alloc(c, SGX_INS_BR, SGX_OP_BR);
   i->label = l;
   i->pred = pred;
   i->ndst = 0;
   emit(c, i);
}

static void emit_tex(struct sgx_ctx *c, nir_tex_instr *tex);

/* tex(v.xy * rcp(v.w)) on a whole vec4 varying v: the DDK's non-dependent
 * projected read (the PDS iterates v.xy / v.w); the divide isn't emitted */
static nir_alu_instr *
src_alu(nir_src *s, nir_op op)
{
   nir_instr *p = nir_def_instr(s->ssa);
   if (p->type != nir_instr_type_alu || nir_instr_as_alu(p)->op != op)
      return NULL;
   return nir_instr_as_alu(p);
}

/* the varying load behind a mov of it (or the load itself), and the swizzle */
static nir_intrinsic_instr *
varying_load(nir_src *src, const uint8_t **swz, nir_alu_instr **mov)
{
   static const uint8_t ident[4] = { 0, 1, 2, 3 };
   nir_instr *p = nir_def_instr(src->ssa);
   *swz = ident;
   *mov = NULL;
   if (p->type == nir_instr_type_alu && nir_instr_as_alu(p)->op == nir_op_mov) {
      *mov = nir_instr_as_alu(p);
      *swz = (*mov)->src[0].swizzle;
      p = nir_def_instr((*mov)->src[0].src.ssa);
   }
   if (p->type != nir_instr_type_intrinsic)
      return NULL;
   nir_intrinsic_instr *in = nir_instr_as_intrinsic(p);
   /* (load_input: the driver's fragment inputs are lowered without
    * interpolation intrinsics) */
   if ((in->intrinsic != nir_intrinsic_load_interpolated_input && in->intrinsic != nir_intrinsic_load_input) ||
       in->def.num_components != 4 || nir_intrinsic_component(in) != 0)
      return NULL;
   return in;
}

/* a value an input load made: its value's n is the input register even when
 * an F16 iteration turned the value into half references */
static bool
is_input_load(struct sgx_ctx *c, nir_def *d)
{
   nir_instr *p = nir_def_instr(d);

   if (c->vals[d->index].kind == VAL_PA)
      return true;
   if (p->type != nir_instr_type_intrinsic)
      return false;
   nir_intrinsic_op op = nir_instr_as_intrinsic(p)->intrinsic;
   return op == nir_intrinsic_load_input || op == nir_intrinsic_load_interpolated_input;
}

/* the input register a varying's iteration gives its first component: an F16
 * iteration's halves are SGX_IN_HALF | 2 * pa (sgx_nir.c assign_io) */
static unsigned
iter_reg(const struct sgx_iter *it)
{
   return it->f16 ? (SGX_IN_HALF | 2 * it->pa) : it->pa;
}

static void
find_projected(struct sgx_ctx *c, nir_function_impl *impl)
{
   nir_foreach_block(b, impl) {
      nir_foreach_instr(instr, b) {
         if (instr->type != nir_instr_type_tex)
            continue;
         nir_tex_instr *tex = nir_instr_as_tex(instr);
         int ci = nir_tex_instr_src_index(tex, nir_tex_src_coord);
         int pi = nir_tex_instr_src_index(tex, nir_tex_src_projector);
         if (ci < 0 || pi < 0 || tex->coord_components != 2 || tex->op != nir_texop_tex ||
             nir_tex_instr_src_index(tex, nir_tex_src_bias) >= 0)
            continue;
         const uint8_t *cs, *ps;
         nir_alu_instr *cm, *pm;
         nir_intrinsic_instr *l1 = varying_load(&tex->src[ci].src, &cs, &cm);
         nir_intrinsic_instr *l2 = varying_load(&tex->src[pi].src, &ps, &pm);
         if (!l1 || l1 != l2 || cs[0] != 0 || cs[1] != 1 || ps[0] != 3)
            continue;
         _mesa_hash_table_insert(c->proj, tex, l1);
         if (cm && list_is_singular(&cm->def.uses))
            _mesa_set_add(c->skip, &cm->instr);
         if (pm && list_is_singular(&pm->def.uses))
            _mesa_set_add(c->skip, &pm->instr);
      }
   }
}

/* the registers a stored value reads, through movs, vecs and load_reg (and
 * what blk stores into those registers) */
static void
regs_read(nir_def *d, nir_block *blk, struct set *regs, unsigned depth)
{
   nir_instr *p = nir_def_instr(d);
   if (depth > 16)
      return;
   if (p->type == nir_instr_type_alu && nir_op_is_vec_or_mov(nir_instr_as_alu(p)->op)) {
      nir_alu_instr *a = nir_instr_as_alu(p);
      for (unsigned s = 0; s < nir_op_infos[a->op].num_inputs; s++)
         regs_read(a->src[s].src.ssa, blk, regs, depth + 1);
      return;
   }
   if (p->type != nir_instr_type_intrinsic ||
       nir_instr_as_intrinsic(p)->intrinsic != nir_intrinsic_load_reg)
      return;
   nir_def *reg = nir_instr_as_intrinsic(p)->src[0].ssa;
   if (_mesa_set_search(regs, reg))
      return;
   _mesa_set_add(regs, reg);
   nir_foreach_instr(i, blk) {
      if (i->type != nir_instr_type_intrinsic)
         continue;
      nir_intrinsic_instr *st = nir_instr_as_intrinsic(i);
      if (st->intrinsic == nir_intrinsic_store_reg && st->src[1].ssa == reg)
         regs_read(st->src[0].ssa, blk, regs, depth + 1);
   }
}

/* 16-bit registers that one block writes component by component and only
 * then reads (a vecN the vec-to-regs lowering made one): the reads become
 * aliases of what was stored, with no pck.f16.f16 into a register. Not when
 * a register the stored value reads is written before the last read (scalar
 * phis' parallel copy, r = r.yzwx: the alias would read the new value) */
static void
find_shadow_regs(struct sgx_ctx *c, nir_function_impl *impl)
{
   nir_foreach_block(b, impl) {
      nir_foreach_instr(instr, b) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *decl = nir_instr_as_intrinsic(instr);
         if (decl->intrinsic != nir_intrinsic_decl_reg || nir_intrinsic_bit_size(decl) != 16 ||
             nir_intrinsic_num_array_elems(decl) || nir_intrinsic_num_components(decl) > 4)
            continue;
         nir_block *blk = NULL;
         bool ok = true;
         nir_foreach_use_including_if(use, &decl->def) {
            nir_instr *u = nir_src_is_if(use) ? NULL : nir_src_use_instr(use);
            if (!u || u->type != nir_instr_type_intrinsic) {
               ok = false;
               break;
            }
            nir_intrinsic_instr *ui = nir_instr_as_intrinsic(u);
            if ((ui->intrinsic != nir_intrinsic_load_reg && ui->intrinsic != nir_intrinsic_store_reg) ||
                nir_intrinsic_base(ui) != 0 ||
                (ui->intrinsic == nir_intrinsic_store_reg && nir_intrinsic_legacy_fsat(ui)) ||
                (blk && u->block != blk)) {
               ok = false;
               break;
            }
            blk = u->block;
         }
         if (!ok || !blk)
            continue;
         /* in that block: no store after a load */
         bool loaded = false;
         struct set *srcs = _mesa_pointer_set_create(NULL);
         nir_foreach_instr(i2, blk) {
            if (i2->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *ui = nir_instr_as_intrinsic(i2);
            unsigned r = ui->intrinsic == nir_intrinsic_load_reg ? 0 : 1;
            if ((ui->intrinsic != nir_intrinsic_load_reg && ui->intrinsic != nir_intrinsic_store_reg) ||
                ui->src[r].ssa != &decl->def)
               continue;
            if (ui->intrinsic == nir_intrinsic_load_reg)
               loaded = true;
            else if (loaded)
               ok = false;
            else
               regs_read(ui->src[0].ssa, blk, srcs, 0);
         }
         /* ... and none of what it was stored from written between its
          * first store and its last load */
         bool stored = false, between = false;
         nir_instr *last_load = NULL;
         nir_foreach_instr(i2, blk) {
            if (i2->type == nir_instr_type_intrinsic &&
                nir_instr_as_intrinsic(i2)->intrinsic == nir_intrinsic_load_reg &&
                nir_instr_as_intrinsic(i2)->src[0].ssa == &decl->def)
               last_load = i2;
         }
         nir_foreach_instr(i2, blk) {
            if (i2 == last_load)
               break;
            if (i2->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr *ui = nir_instr_as_intrinsic(i2);
            if (ui->intrinsic != nir_intrinsic_store_reg)
               continue;
            if (ui->src[1].ssa == &decl->def)
               stored = true;
            else if (stored && _mesa_set_search(srcs, ui->src[1].ssa))
               between = true;
         }
         _mesa_set_destroy(srcs, NULL);
         if (ok && !between)
            _mesa_set_add(c->shadow_regs, decl);
      }
   }
}

/* one terminate_if on one comparison (its only use): p1 from the test */
static void
find_discard(struct sgx_ctx *c, nir_function_impl *impl)
{
   nir_intrinsic_instr *only = NULL;
   unsigned n = 0;
   nir_foreach_block(b, impl) {
      nir_foreach_instr(instr, b) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
         if (in->intrinsic == nir_intrinsic_terminate_if || in->intrinsic == nir_intrinsic_demote_if ||
             in->intrinsic == nir_intrinsic_terminate || in->intrinsic == nir_intrinsic_demote) {
            n++;
            only = in;
         }
      }
   }
   if (n != 1 || (only->intrinsic != nir_intrinsic_terminate_if && only->intrinsic != nir_intrinsic_demote_if))
      return;
   nir_instr *p = nir_def_instr(only->src[0].ssa);
   if (p->type != nir_instr_type_alu || !list_is_singular(&only->src[0].ssa->uses))
      return;
   nir_alu_instr *cmp = nir_instr_as_alu(p);
   if ((cmp->op != nir_op_sge && cmp->op != nir_op_slt && cmp->op != nir_op_seq && cmp->op != nir_op_sne) ||
       cmp->def.num_components != 1)
      return;
   c->discard_cmp = cmp;
   _mesa_set_add(c->skip, &cmp->instr);
}

static void emit_cf_list(struct sgx_ctx *c, struct exec_list *list, struct loop_labels *loop);

static void
emit_block(struct sgx_ctx *c, nir_block *b, struct loop_labels *loop)
{
   nir_foreach_instr(instr, b) {
      if (c->failed)
         return;
      if (_mesa_set_search(c->skip, instr))
         continue;
      switch (instr->type) {
      case nir_instr_type_alu:
         emit_alu(c, nir_instr_as_alu(instr));
         break;
      case nir_instr_type_intrinsic:
         emit_intrinsic(c, nir_instr_as_intrinsic(instr));
         break;
      case nir_instr_type_load_const: {
         nir_load_const_instr *lc = nir_instr_as_load_const(instr);
         c->vals[lc->def.index].kind = VAL_CONST;
         c->vals[lc->def.index].lc = lc;
         break;
      }
      case nir_instr_type_undef: {
         nir_undef_instr *u = nir_instr_as_undef(instr);
         c->vals[u->def.index].kind = VAL_CONST;
         c->vals[u->def.index].lc = NULL;
         break;
      }
      case nir_instr_type_tex:
         emit_tex(c, nir_instr_as_tex(instr));
         break;
      case nir_instr_type_jump: {
         nir_jump_instr *j = nir_instr_as_jump(instr);
         if (!loop) {
            sgx_fail(c, "jump outside a loop");
            break;
         }
         if (j->type == nir_jump_break)
            emit_br(c, loop->exit, 0);
         else if (j->type == nir_jump_continue)
            emit_br(c, loop->head, 0);
         else
            sgx_fail(c, "unsupported jump");
         break;
      }
      default:
         sgx_fail(c, "unsupported instruction type %d", instr->type);
      }
   }
}

static void
emit_cf_list(struct sgx_ctx *c, struct exec_list *list, struct loop_labels *loop)
{
   foreach_list_typed(nir_cf_node, node, node, list) {
      switch (node->type) {
      case nir_cf_node_block:
         emit_block(c, nir_cf_node_as_block(node), loop);
         break;
      case nir_cf_node_if: {
         nir_if *nif = nir_cf_node_as_if(node);
         nir_legacy_src ls = nir_legacy_chase_src(&nif->condition);
         unsigned l_else = new_label(c), l_end = new_label(c);
         /* p0 = (cond == 0): skip the then-side */
         emit_test(c, 0, src_comp(c, &ls, 0), ref(SGX_F_K, 0), 0, 20);
         emit_br(c, l_else, 1);
         emit_cf_list(c, &nif->then_list, loop);
         bool has_else = !exec_list_is_empty(&nif->else_list) &&
                         !(exec_list_length(&nif->else_list) == 1 &&
                           exec_list_is_empty(&nir_cf_node_as_block(
                              exec_node_data(nir_cf_node, exec_list_get_head(&nif->else_list), node))->instr_list));
         if (has_else)
            emit_br(c, l_end, 0);
         emit_label(c, l_else);
         if (has_else) {
            emit_cf_list(c, &nif->else_list, loop);
            emit_label(c, l_end);
         }
         break;
      }
      case nir_cf_node_loop: {
         nir_loop *nl = nir_cf_node_as_loop(node);
         struct loop_labels ll = { new_label(c), new_label(c) };
         emit_label(c, ll.head);
         emit_cf_list(c, &nl->body, &ll);
         emit_br(c, ll.head, 0);
         emit_label(c, ll.exit);
         break;
      }
      default:
         sgx_fail(c, "unsupported control flow");
      }
   }
}

/* ------------------------------------------------------------------------
 * textures: a USP sample block; the finalizer writes the smp code */

static void
emit_tex(struct sgx_ctx *c, nir_tex_instr *tex)
{
   if (c->stage != MESA_SHADER_FRAGMENT || c->n_samples >= ARRAY_SIZE(c->samples) ||
       c->n_iters >= ARRAY_SIZE(c->iters)) {
      sgx_fail(c, "texture sampling here is not supported yet");
      return;
   }
   int ci = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   int bi = nir_tex_instr_src_index(tex, nir_tex_src_bias);
   int pi = nir_tex_instr_src_index(tex, nir_tex_src_projector);
   if ((tex->op != nir_texop_tex && tex->op != nir_texop_txb) || ci < 0) {
      sgx_fail(c, "texture op %d", tex->op);
      return;
   }
   struct sgx_sample *s = rzalloc(c, struct sgx_sample);
   s->id = c->n_samples;
   s->dim = tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE ? 3 : 2;
   s->coord_pa = -1;
   s->sampler = tex->texture_index;
   /* non-dependent: the coordinate is (the start of) a varying the PDS
    * iterates, no bias; the PDS then samples and leaves the texel in pa */
   nir_legacy_src ls = nir_legacy_chase_src(&tex->src[ci].src);
   unsigned ncoord = tex->coord_components;
   s->dependent = true;
   struct hash_entry *he = _mesa_hash_table_search(c->proj, tex);
   if (he) {
      nir_intrinsic_instr *load = he->data;
      unsigned pa = c->vals[load->def.index].n;
      /* (the PDS samples from the set itself, however it is iterated) */
      for (unsigned i = 0; i < c->n_iters; i++) {
         if (!c->iters[i].sample && iter_reg(&c->iters[i]) == pa && c->iters[i].comps == 4) {
            s->dependent = false;
            s->proj = true;
            s->set = c->iters[i].set;
            s->coord_pa = c->iters[i].pa;
         }
      }
      if (s->dependent) {
         sgx_fail(c, "projected read of a varying that isn't iterated");
         return;
      }
   } else if (bi < 0 && pi < 0) {
      /* (a whole varying, or its first components; directly or through a
       * mov that takes its first components in order) */
      int pa = -1;
      const uint8_t *cs;
      nir_alu_instr *cm;
      nir_intrinsic_instr *l;

      if (ls.is_ssa && is_input_load(c, ls.ssa))
         pa = c->vals[ls.ssa->index].n;
      else if ((l = varying_load(&tex->src[ci].src, &cs, &cm)) && is_input_load(c, &l->def)) {
         bool prefix = true;

         for (unsigned j = 0; j < ncoord; j++)
            prefix &= cs[j] == j;
         if (prefix)
            pa = c->vals[l->def.index].n;
      }
      /* (an F16-iterated varying is matched by its halves' register) */
      for (unsigned i = 0; pa >= 0 && i < c->n_iters; i++) {
         if (!c->iters[i].sample && iter_reg(&c->iters[i]) == (unsigned)pa && c->iters[i].comps >= ncoord) {
            s->dependent = false;
            s->set = c->iters[i].set;
            s->coord_pa = c->iters[i].pa;
         }
      }
   }
   c->samples[c->n_samples++] = s;
   struct sgx_ins *i = ins_alloc(c, SGX_INS_SAMPLE, 0);
   i->smp = s;
   struct sgx_ref raw;
   if (!s->dependent) {
      struct sgx_iter *it = &c->iters[c->n_iters++];
      it->sample = true;
      it->proj = s->proj;
      it->sampler = s->sampler;
      it->set = s->set;
      it->comps = 0;
      it->pa = s->pa = c->n_pa_inputs++;
      i->ndst = 0;
      raw = ref(SGX_F_PA, s->pa);
   } else {
      /* the smp the finalizer writes: raw texel -> a temporary; coordinate
       * in consecutive registers of one bank */
      struct sgx_ref cr[4];
      for (unsigned k = 0; k < ncoord; k++)
         cr[k] = src_comp(c, &ls, k);
      if (pi >= 0) {
         /* projective: no smp bit for it, divide first */
         nir_legacy_src lp = nir_legacy_chase_src(&tex->src[pi].src);
         unsigned q = new_vreg(c, 1), t = new_vreg(c, ncoord);
         struct sgx_ref qd[4] = { vref(q, 0) }, qs[1][4] = { { const_f32(src_comp(c, &lp, 0)) } };
         emit_runs(c, SGX_OP_FRCP, 1, qd, 1, qs, &NOF, 0);
         struct sgx_ref td[4], ts[3][4];
         for (unsigned k = 0; k < 4; k++) {
            td[k] = vref(t, k);
            ts[0][k] = k < ncoord ? const_f32(cr[k]) : cref(48);
            ts[1][k] = vref(q, 0);
            ts[2][k] = ref(SGX_F_K, 0x80000000);
         }
         emit_runs(c, SGX_OP_FMAD, nir_component_mask(ncoord), td, 3, ts, &NOF, 0);
         for (unsigned k = 0; k < ncoord; k++)
            cr[k] = vref(t, k);
      }
      bool h16 = pi < 0;
      for (unsigned k = 0; k < ncoord; k++)
         h16 &= cr[k].h16;
      bool run = !cr[0].neg && !cr[0].abs && cr[0].file != SGX_F_K && cr[0].file != SGX_F_C;
      for (unsigned k = 1; k < ncoord; k++)
         run &= ref_next(&cr[k - 1], &cr[k]);
      if (h16) {
         /* F16 coordinates: smp .f16 (cfmt) reads them two to a register,
          * as libusc samples a mediump coordinate (corpus tests/f16) */
         s->f16coord = true;
         run = ncoord <= 2 && (cr[0].file == SGX_F_V || cr[0].file == SGX_F_PA) &&
               (ncoord == 1 ? !((cr[0].file == SGX_F_V ? cr[0].comp : cr[0].n) & 1) && !cr[0].neg && !cr[0].abs
                            : same_reg_pair(&cr[0], &cr[1]) && !cr[0].neg && !cr[0].abs);
         if (!run && ncoord == 2 && !cr[0].neg && !cr[0].abs && !cr[1].neg && !cr[1].abs &&
             (cr[0].file != SGX_F_K || cr[1].file != SGX_F_K)) {
            /* other lanes, no modifiers: packing them is the whole job */
            cr[0] = pair_reg_plain(c, cr[0], cr[1]);
         } else if (!run) {
            unsigned t = new_vreg16(c, 2 * ((ncoord + 1) / 2));
            for (unsigned p = 0; 2 * p < ncoord; p++) {
               /* x * 1 + 0 per pair: takes any lanes, modifiers, constants */
               struct sgx_ref m[3][2] = { { cr[2 * p], 2 * p + 1 < ncoord ? cr[2 * p + 1] : cr[2 * p] } };
               m[1][0] = ref(SGX_F_K, 0x3c00); m[1][0].h16 = 1; m[1][1] = m[1][0];
               m[2][0] = ref(SGX_F_K, 0); m[2][0].h16 = 1; m[2][1] = m[2][0];
               fmad16(c, href(t, 2 * p), m);
            }
            cr[0] = href(t, 0);
         }
      } else if (!run) {
         unsigned t = new_vreg(c, ncoord);
         struct sgx_ref td[4], ts[2][4];
         unsigned form = SGX_OP_MOV;
         for (unsigned k = 0; k < 4; k++) {
            td[k] = vref(t, k);
            ts[0][k] = k < ncoord ? const_f32(cr[k]) : ref(SGX_F_C, 48);
            ts[1][k] = cref(48);
            if (ts[0][k].h16)
               form = SGX_OP_FSUBFLR;   /* F16 beside f32: widen (format select) */
         }
         emit_runs(c, form, nir_component_mask(ncoord), td, form == SGX_OP_MOV ? 1 : 2, ts, &NOF, 0);
         cr[0] = vref(t, 0);
      }
      unsigned rv = new_vreg(c, 1);
      raw = vref(rv, 0);
      i->ndst = 1;
      i->nops = 2;
      i->op[0] = raw;
      i->op[1] = cr[0];
      i->rpt = s->f16coord ? (ncoord + 1) / 2 : ncoord;   /* the coordinate's registers are read */
      if (bi >= 0) {
         nir_legacy_src lb = nir_legacy_chase_src(&tex->src[bi].src);
         s->lod = 1;
         i->op[2] = const_f32(src_comp(c, &lb, 0));
         i->op[2].step = 0;
         i->nops = 3;
      }
      s->ins = i;
   }
   emit(c, i);
   if (s->dependent && i->nops == 3 && i->op[2].file == SGX_F_K && hw_const(i->op[2].n) >= 0)
      i->op[2] = cref(hw_const(i->op[2].n));
   /* (the bias is an f32 source: a mediump one widens) */
   if (s->dependent && i->nops == 3 && (i->op[2].h16 || !slot_ok(SGX_OP_SMP, 3, &i->op[2]))) {
      struct sgx_ref m = materialize(c, i, i->op[2], 1);
      i->op[2] = m;
      i->op[2].step = 0;
   }

   if (tex->def.bit_size == 16) {
      /* to F16, two channels per instruction: the finalizer's own unpack */
      unsigned w = new_vreg16(c, 4);
      c->vals[tex->def.index].kind = VAL_V;
      c->vals[tex->def.index].n = w;
      c->vals[tex->def.index].comp = 0;
      for (unsigned p = 0; p < 2; p++) {
         struct sgx_ins *pk = ins_alloc(c, SGX_INS_ALU, SGX_OP_PCK_F16_U8);
         pk->nops = 3;
         pk->op[0] = href(w, 2 * p);
         pk->op[1] = raw;
         pk->op[1].lane = 2 * p;
         pk->op[2] = raw;
         pk->op[2].lane = 2 * p + 1;
         fld(pk, SGX_FLD_SCALE, 1);
         emit(c, pk);
      }
      return;
   }
   /* the texel: RGBA8, channel n in byte n (swizzle xyzw); to F32 */
   unsigned w = new_vreg(c, 4);
   c->vals[tex->def.index].kind = VAL_V;
   c->vals[tex->def.index].n = w;
   c->vals[tex->def.index].comp = 0;
   for (unsigned k = 0; k < 4; k++) {
      struct sgx_ins *p = ins_alloc(c, SGX_INS_ALU, SGX_OP_PCK_F32_U8);
      p->nops = 3;
      p->op[0] = vref(w, k);
      p->op[1] = raw;
      p->op[1].lane = k;
      p->op[2] = ref(SGX_F_IMM, 0);
      fld(p, SGX_FLD_SCALE, 1);
      emit(c, p);
   }
}

/* ------------------------------------------------------------------------
 * fragment epilogue: colour to packed BGRA8, discard feedback */

/* ------------------------------------------------------------------------
 * lowp in C10, as the DDK computes it: a lowp vec4 expression over lowp
 * uniforms that only the colour reads runs in the colour classes on 10-bit
 * fixed point (ISA.md "sop2"; corpus c1/c2 *_l). Its leaves are the uniforms
 * as C10 (xyz in one SA slot, w in channel 0 of another). A value goes into
 * an internal register (pck.c10.c10 i, xyz, w [scale]) for sopwm (add, sub,
 * min, max), sop3l (mul, mix) and sop3 (x - y * z); rcp, log2 and exp2 run
 * channel by channel and floor is x - fract(x) through F16. The root writes
 * the U8 colour itself. */

static bool
c10_leaf(struct sgx_ctx *c, nir_def *d)
{
   nir_instr *p = nir_def_instr(d);
   if (p->type != nir_instr_type_intrinsic)
      return false;
   nir_intrinsic_instr *in = nir_instr_as_intrinsic(p);
   if (in->intrinsic != nir_intrinsic_load_uniform || !nir_src_is_const(in->src[0]) ||
       nir_src_as_uint(in->src[0]) != 0)
      return false;
   unsigned k = nir_intrinsic_base(in);
   return k + 3 < ARRAY_SIZE(c->const_lowp) && c->const_lowp[k] && d->num_components == 4 && d->bit_size == 16;
}

/* source s of a C10 node: a whole vec4, maybe negated */
static nir_def *
c10_src(nir_alu_instr *alu, unsigned s, bool *neg)
{
   nir_legacy_alu_src ls = nir_legacy_chase_alu_src(&alu->src[s], true);
   if (!ls.src.is_ssa || ls.fabs)
      return NULL;
   for (unsigned j = 0; j < 4; j++)
      if (ls.swizzle[j] != j)
         return NULL;
   *neg = ls.fneg;
   return ls.src.ssa;
}

static nir_alu_instr *
c10_alu(nir_def *d)
{
   nir_instr *p = nir_def_instr(d);
   return p->type == nir_instr_type_alu ? nir_instr_as_alu(p) : NULL;
}

/* fadd(x, -fmul(y, z)) with the product used once: one sop3 */
static nir_alu_instr *
c10_msub(nir_alu_instr *alu, nir_def **x)
{
   bool n0, n1;
   nir_def *a = c10_src(alu, 0, &n0), *b = c10_src(alu, 1, &n1);
   if (alu->op != nir_op_fadd || !a || !b || n0 == n1)
      return NULL;
   nir_alu_instr *m = c10_alu(n1 ? b : a);
   if (!m || m->op != nir_op_fmul || !list_is_singular(&m->def.uses))
      return NULL;
   *x = n1 ? a : b;
   return m;
}

/* the tree under d: every node into `set` (with the fnegs folded into it) */
static bool
c10_tree(struct sgx_ctx *c, nir_def *d, struct set *set)
{
   if (d->bit_size != 16 || d->num_components != 4)
      return false;
   if (c10_leaf(c, d)) {
      _mesa_set_add(set, nir_def_instr(d));
      return true;
   }
   nir_alu_instr *alu = c10_alu(d);
   if (!alu)
      return false;
   switch (alu->op) {
   case nir_op_fadd: case nir_op_fmul: case nir_op_fmin: case nir_op_fmax: case nir_op_flrp:
   case nir_op_frcp: case nir_op_flog2: case nir_op_fexp2: case nir_op_ffloor: case nir_op_ffma:
   case nir_op_fmad:
      break;
   default:
      return false;
   }
   bool negs = false;
   for (unsigned s = 0; s < nir_op_infos[alu->op].num_inputs; s++) {
      bool neg;
      nir_def *sd = c10_src(alu, s, &neg);
      if (!sd || !c10_tree(c, sd, set))
         return false;
      if (neg) {
         /* fadd: one negated side; ffma: one negated factor */
         if ((alu->op != nir_op_fadd && !((alu->op == nir_op_ffma || alu->op == nir_op_fmad) && s < 2)) || negs)
            return false;
         negs = true;
         _mesa_set_add(set, nir_def_instr(alu->src[s].src.ssa));
      }
   }
   _mesa_set_add(set, &alu->instr);
   return true;
}

/* the colour's value as a C10 tree that nothing else reads */
static void
find_c10(struct sgx_ctx *c, nir_function_impl *impl)
{
   if (!c->p16)
      return;
   nir_intrinsic_instr *st = NULL;
   nir_foreach_block(b, impl) {
      nir_foreach_instr(instr, b) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
         if (in->intrinsic == nir_intrinsic_store_output) {
            if (st)
               return;
            st = in;
         }
      }
   }
   if (!st || nir_intrinsic_write_mask(st) != 0xf || nir_intrinsic_component(st) != 0)
      return;
   nir_alu_instr *cv = c10_alu(st->src[0].ssa);
   bool neg;
   if (!cv || cv->op != nir_op_f2f32 || !list_is_singular(&cv->def.uses))
      return;
   nir_def *root = c10_src(cv, 0, &neg);
   if (!root || neg || c10_leaf(c, root))
      return;
   struct set *set = _mesa_pointer_set_create(c);
   if (!c10_tree(c, root, set))
      return;
   /* nothing outside reads the tree */
   set_foreach(set, e) {
      nir_instr *instr = (nir_instr *)e->key;
      nir_def *d = instr->type == nir_instr_type_alu ? &nir_instr_as_alu(instr)->def
                                                     : &nir_instr_as_intrinsic(instr)->def;
      nir_foreach_use_including_if(use, d) {
         if (nir_src_is_if(use))
            return;
         nir_instr *u = nir_src_use_instr(use);
         if (u != &cv->instr && !_mesa_set_search(set, u))
            return;
      }
   }
   set_foreach(set, e)
      _mesa_set_add(c->skip, e->key);
   _mesa_set_add(c->skip, &cv->instr);
   _mesa_set_add(c->skip, &st->instr);
   c->c10_root = root;
   c->has_color = true;
}

struct c10_state {
   unsigned busy;                       /* internal registers in use */
   struct { nir_def *d; int i; unsigned left; } held[3];
   unsigned n_held;
};

/* a C10 value: an internal register, or a uniform's xyz/w slots */
struct c10v {
   int i;
   unsigned k;
   bool held;
};

static int
c10_ialloc(struct sgx_ctx *c, struct c10_state *st)
{
   for (int i = 0; i < 3; i++) {
      if (!(st->busy & 1u << i)) {
         st->busy |= 1u << i;
         return i;
      }
   }
   sgx_fail(c, "lowp expression needs more than three internal registers");
   return 0;
}

static struct sgx_ref
iref(int i, bool c10)
{
   struct sgx_ref r = ref(SGX_F_I, i);
   r.c10 = c10;
   return r;
}

static struct sgx_ref
c10_uref(struct sgx_ctx *c, unsigned k)
{
   c->const_used[k] = 1;
   struct sgx_ref r = ref(SGX_F_U, k);
   r.c10 = 1;
   return r;
}

/* the C10 tree's uses of d */
static unsigned
c10_uses(struct sgx_ctx *c, nir_def *d)
{
   unsigned n = 0;
   nir_foreach_use(use, d) {
      nir_instr *u = nir_src_use_instr(use);
      if (u->type == nir_instr_type_alu && nir_instr_as_alu(u)->op == nir_op_fneg &&
          nir_legacy_float_mod_folds(nir_instr_as_alu(u)))
         n += c10_uses(c, &nir_instr_as_alu(u)->def);
      else
         n++;
   }
   return n;
}

static void
c10_release(struct c10_state *st, struct c10v v)
{
   if (v.i < 0 || v.held)
      return;
   st->busy &= ~(1u << v.i);
}

static void emit_c10(struct sgx_ctx *c, struct c10_state *st, nir_def *d, struct sgx_ref dst);

/* d as a pair (a uniform not packed yet) or in an internal register */
static struct c10v
c10_get(struct sgx_ctx *c, struct c10_state *st, nir_def *d, bool want_i)
{
   for (unsigned h = 0; h < st->n_held; h++) {
      if (st->held[h].d == d) {
         struct c10v v = { st->held[h].i, 0, true };
         if (--st->held[h].left == 0) {
            v.held = false;     /* the last use frees it */
            st->held[h] = st->held[--st->n_held];
         }
         return v;
      }
   }
   struct c10v v = { -1, 0, false };
   if (c10_leaf(c, d)) {
      v.k = nir_intrinsic_base(nir_instr_as_intrinsic(nir_def_instr(d)));
      for (unsigned j = 0; j < 4; j++) {
         c->const_fmt[v.k + j] = j == 3 ? 4 : 3;
         c->const_used[v.k + j] = 1;   /* list C names every channel */
      }
      if (!want_i)
         return v;
      v.i = c10_ialloc(c, st);
      struct sgx_ins *p = ins_alloc(c, SGX_INS_ALU, SGX_OP_PCK_C10_C10);
      p->nops = 3;
      p->op[0] = iref(v.i, false);
      p->op[1] = c10_uref(c, v.k);
      p->op[2] = c10_uref(c, v.k + 3);
      p->op[1].c10 = p->op[2].c10 = 0;
      fld(p, SGX_FLD_SCALE, 1);
      add(c, p);
      return v;     /* packed again for each use */
   }
   emit_c10(c, st, d, ref(SGX_F_NONE, 0));
   v.i = st->held[--st->n_held].i;   /* emit_c10 leaves its result on top */
   unsigned uses = c10_uses(c, d);
   if (uses > 1) {
      assert(st->n_held < 3);
      st->held[st->n_held].d = d;
      st->held[st->n_held].i = v.i;
      st->held[st->n_held++].left = uses - 1;
      v.held = true;
   }
   return v;
}

static struct sgx_ref
c10_opd(struct sgx_ctx *c, struct c10v v, bool alpha)
{
   return v.i >= 0 ? iref(v.i, true) : c10_uref(c, v.k + (alpha ? 3 : 0));
}

static void
c10_sop(struct sgx_ctx *c, unsigned form, struct sgx_ref dst, const struct sgx_ref *src, unsigned n,
        const int *f, unsigned nf)
{
   struct sgx_ins *i = ins_alloc(c, SGX_INS_ALU, form);
   i->nops = 1 + n;
   i->op[0] = dst;
   for (unsigned k = 0; k < n; k++)
      i->op[1 + k] = src[k];
   for (unsigned k = 0; k < nf; k++)
      fld(i, f[2 * k], f[2 * k + 1]);
   add(c, i);
}

/* d into dst (the colour, U8), or, with dst NONE, into a new internal
 * register left on top of st->held */
static void
emit_c10(struct sgx_ctx *c, struct c10_state *st, nir_def *d, struct sgx_ref dst)
{
   nir_alu_instr *alu = c10_alu(d);
   bool to_i = dst.file == SGX_F_NONE;
   bool n0 = false, n1 = false;
   nir_def *a = c10_src(alu, 0, &n0);
   nir_def *b = nir_op_infos[alu->op].num_inputs > 1 ? c10_src(alu, 1, &n1) : NULL;
   struct c10v va, vb, vt;
   struct sgx_ref s[3];
   int di = -1;
#define DST() (to_i ? iref(di = c10_ialloc(c, st), true) : dst)
   switch (alu->op) {
   case nir_op_fadd:
   case nir_op_fmin:
   case nir_op_fmax: {
      nir_def *x;
      nir_alu_instr *m = c10_msub(alu, &x);
      if (m) {
         /* x - y * z: sop3 d, z, x, y [cop=sub c1=one c2=s0 a=one aop=sub] */
         bool ny, nz;
         nir_def *y = c10_src(m, 0, &ny), *z = c10_src(m, 1, &nz);
         struct c10v vz = c10_get(c, st, z, true);
         struct c10v vy = c10_get(c, st, y, true);
         struct c10v vx = c10_get(c, st, x, true);
         s[0] = c10_opd(c, vz, false);
         s[1] = c10_opd(c, vx, false);
         s[2] = c10_opd(c, vy, false);
         c10_release(st, vz);
         c10_release(st, vy);
         c10_release(st, vx);
         static const int f[] = { SGX_FLD_COP, 1, SGX_FLD_C1, 8, SGX_FLD_C2, 4, SGX_FLD_A, 4, SGX_FLD_AOP, 1 };
         c10_sop(c, SGX_OP_SOP3, DST(), s, 3, f, 5);
         break;
      }
      if (n0) {
         nir_def *t = a; a = b; b = t;
         n0 = false; n1 = true;
      }
      va = c10_get(c, st, a, true);
      vb = c10_get(c, st, b, true);
      s[0] = c10_opd(c, va, false);
      s[1] = c10_opd(c, vb, false);
      c10_release(st, va);
      c10_release(st, vb);
      int op = alu->op == nir_op_fmin ? 2 : alu->op == nir_op_fmax ? 3 : n1 ? 1 : 0;
      int f[] = { SGX_FLD_C1, 8, SGX_FLD_C2, 8, SGX_FLD_COP, op, SGX_FLD_AOP, op };
      c10_sop(c, SGX_OP_SOPWM, DST(), s, 2, f, 4);
      break;
   }
   case nir_op_ffma:
   case nir_op_fmad: {
      bool n2;
      nir_def *cc = c10_src(alu, 2, &n2);
      if (n0 || n1) {
         /* c - a * b: sop3 d, b, c, a [cop=sub c1=one c2=s0 a=one aop=sub],
          * libusc's x - y * z */
         struct c10v vz = c10_get(c, st, b, true);
         struct c10v vy = c10_get(c, st, a, true);
         struct c10v vx = c10_get(c, st, cc, true);
         s[0] = c10_opd(c, vz, false);
         s[1] = c10_opd(c, vx, false);
         s[2] = c10_opd(c, vy, false);
         c10_release(st, vz);
         c10_release(st, vy);
         c10_release(st, vx);
         static const int f[] = { SGX_FLD_COP, 1, SGX_FLD_C1, 8, SGX_FLD_C2, 4, SGX_FLD_A, 4, SGX_FLD_AOP, 1 };
         c10_sop(c, SGX_OP_SOP3, DST(), s, 3, f, 5);
         break;
      }
      /* a * b + c: the product into an internal register (sop3l), then a
       * sopwm add, as libusc does a lowp mad */
      nir_def *x = a, *y = b;
      if (!c10_leaf(c, a) && c10_leaf(c, b)) {
         x = b;
         y = a;
      }
      vb = c10_get(c, st, y, true);
      va = c10_get(c, st, x, false);
      s[0] = c10_opd(c, vb, false);
      s[1] = c10_opd(c, va, false);
      s[2] = c10_opd(c, va, true);
      c10_release(st, vb);
      c10_release(st, va);
      static const int fm[] = { SGX_FLD_COP, 0, SGX_FLD_C1, 4, SGX_FLD_C2, 0, SGX_FLD_A1, 0, SGX_FLD_A2, 1 };
      int pi = c10_ialloc(c, st);
      c10_sop(c, SGX_OP_SOP3L, iref(pi, true), s, 3, fm, 5);
      struct c10v vp = { pi, 0, false };
      vt = c10_get(c, st, cc, true);
      s[0] = c10_opd(c, vp, false);
      s[1] = c10_opd(c, vt, false);
      c10_release(st, vp);
      c10_release(st, vt);
      static const int fa[] = { SGX_FLD_C1, 8, SGX_FLD_C2, 8, SGX_FLD_COP, 0, SGX_FLD_AOP, 0 };
      c10_sop(c, SGX_OP_SOPWM, DST(), s, 2, fa, 4);
      break;
   }
   case nir_op_fmul: {
      /* sop3l d, y, x.xyz, x.w [cop=add c1=s0 c2=zero a1=zero a2=s2a]: x
       * straight from its slots when it is a uniform */
      nir_def *x = a, *y = b;
      if (!c10_leaf(c, a) && c10_leaf(c, b)) {
         x = b;
         y = a;
      }
      vb = c10_get(c, st, y, true);
      va = c10_get(c, st, x, false);
      s[0] = c10_opd(c, vb, false);
      s[1] = c10_opd(c, va, false);
      s[2] = c10_opd(c, va, true);
      c10_release(st, vb);
      c10_release(st, va);
      static const int f[] = { SGX_FLD_COP, 0, SGX_FLD_C1, 4, SGX_FLD_C2, 0, SGX_FLD_A1, 0, SGX_FLD_A2, 1 };
      c10_sop(c, SGX_OP_SOP3L, DST(), s, 3, f, 5);
      break;
   }
   case nir_op_flrp: {
      /* sop3l d, t, a, b [cop=add c1=1-s0 c2=s0 a1=s1a a2=s2a] */
      bool nt;
      nir_def *t = c10_src(alu, 2, &nt);
      vt = c10_get(c, st, t, true);
      va = c10_get(c, st, a, true);
      vb = c10_get(c, st, b, true);
      s[0] = c10_opd(c, vt, false);
      s[1] = c10_opd(c, va, false);
      s[2] = c10_opd(c, vb, false);
      c10_release(st, vt);
      c10_release(st, va);
      c10_release(st, vb);
      static const int f[] = { SGX_FLD_COP, 0, SGX_FLD_C1, 12, SGX_FLD_C2, 4, SGX_FLD_A1, 1, SGX_FLD_A2, 1 };
      c10_sop(c, SGX_OP_SOP3L, DST(), s, 3, f, 5);
      break;
   }
   case nir_op_frcp:
   case nir_op_flog2:
   case nir_op_fexp2: {
      /* per channel into C10 (f16d): xyz to temporaries, w to an internal
       * register, then two pck.c10.c10 put them together */
      unsigned form = alu->op == nir_op_frcp ? SGX_OP_FRCP : alu->op == nir_op_flog2 ? SGX_OP_FLOG2 : SGX_OP_FEXP2;
      va = c10_get(c, st, a, true);
      unsigned t[3];
      int wi = va.held ? c10_ialloc(c, st) : va.i;
      /* channel order as the DDK: z, y, x, then w over the source */
      for (unsigned n = 0; n < 4; n++) {
         unsigned ch = n < 3 ? 2 - n : 3;
         struct sgx_ins *i = ins_alloc(c, SGX_INS_ALU, form);
         i->nops = 2;
         if (ch < 3) {
            t[ch] = new_vreg(c, 1);
            i->op[0] = vref(t[ch], 0);
         } else {
            i->op[0] = iref(wi, false);
         }
         i->op[1] = iref(va.i, false);
         i->op[1].abs = alu->op == nir_op_flog2;
         fld(i, SGX_FLD_C10, 1);
         fld(i, SGX_FLD_CHAN, ch);
         fld(i, SGX_FLD_F16D, 1);
         add(c, i);
      }
      int r = c10_ialloc(c, st);
      struct sgx_ins *p = ins_alloc(c, SGX_INS_ALU, SGX_OP_PCK_C10_C10);
      p->nops = 3;
      p->op[0] = iref(r, false);
      p->op[1] = vref(t[1], 0);
      p->op[1].lane = 1;
      p->op[2] = vref(t[2], 0);
      p->op[2].lane = 2;
      fld(p, SGX_FLD_DMASK, 6);
      add(c, p);
      p = ins_alloc(c, SGX_INS_ALU, SGX_OP_PCK_C10_C10);
      p->nops = 3;
      p->op[0] = iref(r, false);
      p->op[1] = vref(t[0], 0);
      p->op[2] = iref(wi, false);
      p->op[2].lane = 3;
      fld(p, SGX_FLD_DMASK, 9);
      add(c, p);
      if (wi != va.i)
         st->busy &= ~(1u << wi);
      c10_release(st, va);
      if (to_i) {
         di = r;
      } else {
         s[0] = iref(r, true);
         s[1] = ref(SGX_F_IMM, 0);
         static const int f[] = { SGX_FLD_C1, 8, SGX_FLD_C2, 0, SGX_FLD_COP, 0, SGX_FLD_AOP, 0 };
         c10_sop(c, SGX_OP_SOPWM, dst, s, 2, f, 4);
         st->busy &= ~(1u << r);
      }
      break;
   }
   case nir_op_ffloor: {
      /* x - fract(x): fract per channel in f32 from F16 (fsubflr), back to
       * C10, then a sopwm subtract */
      va = c10_get(c, st, a, true);
      unsigned fr[4];
      static const uint8_t lanes[2][2] = { { 2, 1 }, { 0, 3 } };
      for (unsigned q = 0; q < 2; q++) {
         unsigned h = new_vreg16(c, 2);
         struct sgx_ins *p = ins_alloc(c, SGX_INS_ALU, SGX_OP_PCK_F16_C10);
         p->nops = 3;
         p->op[0] = href(h, 0);
         p->op[1] = iref(va.i, false);
         p->op[1].lane = lanes[q][0];
         p->op[2] = iref(va.i, false);
         p->op[2].lane = lanes[q][1];
         fld(p, SGX_FLD_SCALE, 1);
         add(c, p);
         for (unsigned hh = 0; hh < 2; hh++) {
            unsigned ch = lanes[q][hh];
            fr[ch] = new_vreg(c, 1);
            struct sgx_ins *i = ins_alloc(c, SGX_INS_ALU, SGX_OP_FSUBFLR);
            i->nops = 3;
            i->op[0] = vref(fr[ch], 0);
            i->op[1] = href(h, hh);
            i->op[2] = href(h, hh);
            add(c, i);
         }
      }
      int f = c10_ialloc(c, st);
      for (unsigned q = 0; q < 2; q++) {
         struct sgx_ins *p = ins_alloc(c, SGX_INS_ALU, SGX_OP_PCK_C10_F32);
         p->nops = 3;
         p->op[0] = iref(f, false);
         p->op[1] = vref(fr[q ? 0 : 1], 0);
         p->op[2] = vref(fr[q ? 3 : 2], 0);
         fld(p, SGX_FLD_DMASK, q ? 9 : 6);
         fld(p, SGX_FLD_SCALE, 1);
         add(c, p);
      }
      s[0] = c10_opd(c, va, false);
      s[1] = iref(f, true);
      c10_release(st, va);
      static const int fl[] = { SGX_FLD_C1, 8, SGX_FLD_C2, 8, SGX_FLD_COP, 1, SGX_FLD_AOP, 1 };
      if (to_i) {
         di = f;
         c10_sop(c, SGX_OP_SOPWM, iref(f, true), s, 2, fl, 4);
      } else {
         c10_sop(c, SGX_OP_SOPWM, dst, s, 2, fl, 4);
         st->busy &= ~(1u << f);
      }
      break;
   }
   default:
      sgx_fail(c, "C10 %s", nir_op_infos[alu->op].name);
      return;
   }
#undef DST
   if (to_i) {
      assert(st->n_held < 3 && di >= 0);
      st->held[st->n_held].d = d;
      st->held[st->n_held].i = di;
      st->held[st->n_held++].left = 0;
   }
}

static bool
reads_pa0(const struct sgx_ref *r)
{
   return r->file == SGX_F_PA && (r->h16 ? r->n / 2 : r->n) == 0;
}

/* The feedback point (USP labels 4/5), after the colour: p1 = keep for a
 * discard, gl_FragDepth clamped to [0, 1] in pa<depth_pa>. The driver puts
 * its atst8, or (p1) depthf pa<depth_pa> for a depth write, in the nop's
 * place (ISA.md "Phases, visibility feedback and discard"). */
static void
emit_feedback(struct sgx_ctx *c)
{
   if (c->discards && !c->keep_pred)
      emit_test(c, 1, vref(c->keep_vreg, 0), ref(SGX_F_K, 0), 0, 24);
   if (c->has_depth) {
      c->depth_pa = MAX2(c->n_pa_inputs, c->colour_pa + 1);
      struct sgx_ref d[4] = { ref(SGX_F_PA, c->depth_pa) };
      struct sgx_ref s[3][4] = { { c->depth }, { ref(SGX_F_K, 0) }, { ref(SGX_F_K, 0x3f800000) } };
      emit_runs(c, SGX_OP_FMINMAX, 1, d, 3, s, &NOF, 0);
   }
   if (c->discards || c->has_depth) {
      struct sgx_ins *f = ins_alloc(c, SGX_INS_FEEDBACK, SGX_OP_NOP);
      f->ndst = 0;
      emit(c, f);
   }
}

static void
emit_color(struct sgx_ctx *c)
{
   if (c->c10_root) {
      struct c10_state st = { 0 };
      struct list_head *before = c->ins.prev;
      c->colour_pa = 0;
      emit_c10(c, &st, c->c10_root, ref(SGX_F_PA, 0));
      /* internal registers don't survive a deschedule: nosched on all but
       * the last two instructions of the sequence, as libusc marks it */
      unsigned n = 0;
      for (struct list_head *l = before->next; l != &c->ins; l = l->next)
         n++;
      for (struct list_head *l = before->next; n > 2; l = l->next, n--)
         fld(list_entry(l, struct sgx_ins, link), SGX_FLD_NOSCHED, 1);
      emit_feedback(c);
      return;
   }
   struct sgx_ref col[4];
   for (unsigned k = 0; k < 4; k++)
      col[k] = c->has_color ? c->color[k] : ref(SGX_F_K, 0);
   bool h16 = c->has_color;
   for (unsigned k = 0; k < 4; k++)
      h16 &= col[k].h16 || (col[k].file == SGX_F_K && (col[k].n == 0 || col[k].n == 0x3f800000));
   if (h16) {
      /* pck.u8.f16 D, r, a [dmask=12] then D, b, g [dmask=3], as the DDK packs
       * an F16 colour */
      struct sgx_ref orig[4];
      memcpy(orig, col, sizeof(orig));
      for (unsigned k = 0; k < 4; k++) {
         if (col[k].file != SGX_F_K && (col[k].neg || col[k].abs)) {
            /* pck has no modifiers: x * 1 + 0 applies them (once per value) */
            unsigned j;
            for (j = 0; j < k && !ref_same(&orig[j], &orig[k]); j++)
               ;
            if (j < k) {
               col[k] = col[j];
               continue;
            }
            unsigned t = new_vreg16(c, 2);
            struct sgx_ref m[3][2] = { { col[k], col[k] } };
            m[1][0] = ref(SGX_F_K, 0x3c00); m[1][0].h16 = 1; m[1][1] = m[1][0];
            m[2][0] = ref(SGX_F_K, 0); m[2][0].h16 = 1; m[2][1] = m[2][0];
            fmad16(c, href(t, 0), m);
            col[k] = href(t, 0);
            continue;
         }
         if (col[k].file != SGX_F_K)
            continue;
         uint32_t n = col[k].n;
         if (n == 0 || (!col[k].h16 && n == 0x3f800000) || (col[k].h16 && (n & 0xffff) == 0x3c00)) {
            col[k] = cref(n ? 45 : 48);
            col[k].h16 = 1;
         } else {
            /* another F16 constant (a register alias of it): its halves */
            int sw;
            col[k] = pair_src(c, col[k], col[k], &sw);
            col[k].h16 = 1;
         }
      }
      /* (g, r) [dmask=6] and (b, a) [dmask=9] when r, g share a register and
       * b, a another (a vec4 as it is): each pck reads one register, so the
       * first one's may become the colour (libusc's mul_m) */
      bool rg = same_reg16(&col[0], &col[1]) && same_reg16(&col[2], &col[3]);
      struct sgx_ref pr[2][2] = { { col[0], col[3] }, { col[2], col[1] } };
      unsigned dm[2] = { 12, 3 };
      if (rg) {
         pr[0][0] = col[1]; pr[0][1] = col[0];
         pr[1][0] = col[2]; pr[1][1] = col[3];
         dm[0] = 6; dm[1] = 9;
      }
      bool first = reads_pa0(&pr[0][0]) || reads_pa0(&pr[0][1]);
      bool second = reads_pa0(&pr[1][0]) || reads_pa0(&pr[1][1]);
      unsigned d = second && first ? MAX2(c->n_pa_inputs, 1) : 0;
      bool swap = second && !first;
      c->colour_pa = d;
      for (unsigned hh = 0; hh < 2; hh++) {
         unsigned q = (hh == 0) != swap ? 0 : 1;
         struct sgx_ins *p = ins_alloc(c, SGX_INS_ALU, SGX_OP_PCK_U8_F16);
         p->nops = 3;
         p->op[0] = ref(SGX_F_PA, d);
         p->op[1] = pr[q][0];
         p->op[2] = pr[q][1];
         for (unsigned j = 1; j < 3; j++)
            if (p->op[j].file == SGX_F_C)
               p->op[j].lane = 0;
         fld(p, SGX_FLD_DMASK, dm[q]);
         fld(p, SGX_FLD_SCALE, 1);
         add(c, p);
      }
      emit_feedback(c);
      return;
   }
   /* pck.u8.f32 D, g, r [dmask=6] then D, b, a [dmask=9]: the second reads after
    * the first wrote D, so D must not be what it reads (ISA.md "Fragment shaders") */
   unsigned d = 0;
   for (unsigned k = 0; k < 4; k++) {
      if (col[k].h16 && col[k].file == SGX_F_K) {
         /* an F16 constant beside f32 channels */
         col[k] = const_f32(col[k]);
         continue;
      }
      if (col[k].h16 && col[k].file != SGX_F_K) {
         /* F16 -> f32 (format select) */
         unsigned v = new_vreg(c, 1);
         struct sgx_ref dd[4] = { vref(v, 0) }, ss[3][4] = { { col[k] }, { cref(52) }, { cref(48) } };
         ss[2][0].neg = 1;
         emit_runs(c, SGX_OP_FMAD, 1, dd, 3, ss, &NOF, 0);
         col[k] = vref(v, 0);
      }
   }
   bool first_reads0 = reads_pa0(&col[1]) || reads_pa0(&col[0]);
   bool second_reads0 = reads_pa0(&col[2]) || reads_pa0(&col[3]);
   bool swap = false;
   if (second_reads0 && first_reads0)
      d = MAX2(c->n_pa_inputs, 1);
   else if (second_reads0)
      swap = true;
   c->colour_pa = d;
   for (unsigned h = 0; h < 2; h++) {
      bool lohalf = (h == 0) != swap;
      struct sgx_ins *p = ins_alloc(c, SGX_INS_ALU, SGX_OP_PCK_U8_F32);
      p->nops = 3;
      p->op[0] = ref(SGX_F_PA, d);
      p->op[1] = lohalf ? col[1] : col[2];
      p->op[2] = lohalf ? col[0] : col[3];
      fld(p, SGX_FLD_DMASK, lohalf ? 6 : 9);
      fld(p, SGX_FLD_SCALE, 1);
      emit(c, p);
   }
   emit_feedback(c);
}

/* ------------------------------------------------------------------------
 * peephole: two adjacent instructions whose operands continue each other
 * become one repeat (vertex outputs written varying by varying) */

static bool
continues(const struct sgx_ref *a, unsigned na, const struct sgx_ref *b)
{
   if (a->file != b->file || a->neg != b->neg || a->abs != b->abs || a->h16 || b->h16 || a->lane || b->lane)
      return false;
   if (!a->step)
      return !b->step && ref_same(a, b);
   if (a->file == SGX_F_V)
      return a->n == b->n && a->comp + na == b->comp;
   if (a->file == SGX_F_K)
      return false;
   return a->n + na == b->n;
}

static bool
same_fields(const struct sgx_ins *a, const struct sgx_ins *b)
{
   if (a->nfld != b->nfld)
      return false;
   for (unsigned k = 0; k < a->nfld; k++)
      if (a->fld[k].id != b->fld[k].id || a->fld[k].v != b->fld[k].v)
         return false;
   return true;
}

static void
merge_runs(struct sgx_ctx *c)
{
   list_for_each_entry_safe(struct sgx_ins, b, &c->ins, link) {
      if (b->link.prev == &c->ins)
         continue;
      struct sgx_ins *a = list_entry(b->link.prev, struct sgx_ins, link);
      unsigned cls = form_class(a->form);
      if (a->kind != SGX_INS_ALU || b->kind != SGX_INS_ALU || a->form != b->form || a->pred || b->pred ||
          (cls != CLS_ALL && cls != CLS_INC) || a->rpt + b->rpt > max_rpt(cls) || a->nops != b->nops ||
          a->ndst != 1 || a->form == SGX_OP_FDP || !same_fields(a, b) || a->writes_pred || b->writes_pred ||
          /* (libusc never repeats a derivative) */
          a->form == SGX_OP_FDSX || a->form == SGX_OP_FDSY)
         continue;
      bool ok = true;
      for (unsigned k = 0; k < a->nops && ok; k++)
         ok = continues(&a->op[k], a->rpt, &b->op[k]) && (k == 0 || a->op[k].step || cls == CLS_INC);
      /* b must not read what a writes */
      for (unsigned k = 1; k < b->nops && ok; k++)
         ok = !(b->op[k].file == a->op[0].file && b->op[k].n == a->op[0].n &&
                (a->op[0].file != SGX_F_V || b->op[k].comp < a->op[0].comp + a->rpt));
      if (!ok)
         continue;
      a->rpt += b->rpt;
      list_del(&b->link);
   }
}

/* does r (over rpt iterations) touch registers [n, n + len) of file / vreg? */
static bool
ref_hits(const struct sgx_ref *r, unsigned rpt, unsigned file, unsigned v, unsigned n, unsigned len)
{
   if (r->file != file || (file == SGX_F_V && r->n != v))
      return false;
   unsigned lo = file == SGX_F_V ? r->comp : r->n;
   if (r->h16)
      lo /= 2;
   unsigned hi = lo + (r->step && rpt > 1 ? rpt - 1 : 0);
   return lo < n + len && hi >= n;
}

static bool
ins_hits(const struct sgx_ins *i, unsigned file, unsigned v, unsigned n, unsigned len)
{
   for (unsigned k = 0; k < i->nops; k++)
      if (ref_hits(&i->op[k], i->rpt, file, v, n, len))
         return true;
   return false;
}

/* mov d, t where t's only other reference is the instruction that wrote it:
 * that instruction writes d instead (vertex outputs, copies out of a
 * computation) */
static void
fold_movs(struct sgx_ctx *c)
{
   list_for_each_entry_safe(struct sgx_ins, m, &c->ins, link) {
      if (m->kind != SGX_INS_ALU || m->form != SGX_OP_MOV || m->pred || m->nfld || m->nops != 2)
         continue;
      struct sgx_ref d = m->op[0], s = m->op[1];
      if (s.file != SGX_F_V || s.neg || s.abs || s.h16 || s.lane || (m->rpt > 1 && !s.step) ||
          d.h16 || d.lane || !d.step || (d.file != SGX_F_V && d.file != SGX_F_O) ||
          sgx_vreg(c, s.n)->half || (int)s.n == c->keep_vreg || (int)s.n == c->face_vreg ||
          (int)s.n == c->face_f32 || (d.file == SGX_F_V && sgx_vreg(c, d.n)->half))
         continue;
      unsigned dn = d.file == SGX_F_V ? d.comp : d.n;
      /* the writer: back through this block, nothing between touching d */
      struct sgx_ins *p = NULL;
      bool ok = false;
      for (struct list_head *l = m->link.prev; l != &c->ins; l = l->prev) {
         struct sgx_ins *i = list_entry(l, struct sgx_ins, link);
         if (i->kind != SGX_INS_ALU)
            break;
         bool w = false;
         for (unsigned k = 0; k < i->ndst; k++)
            w |= ref_hits(&i->op[k], i->rpt, SGX_F_V, s.n, s.comp, m->rpt);
         if (w) {
            p = i;
            unsigned cls = form_class(i->form);
            ok = i->ndst == 1 && !i->pred && i->rpt == m->rpt && i->op[0].file == SGX_F_V &&
                 i->op[0].n == s.n && i->op[0].comp == s.comp && !i->op[0].h16 && i->op[0].step &&
                 (cls == CLS_INC || cls == CLS_ALL) && i->form != SGX_OP_FDP &&
                 slot_ok(i->form, 0, &d);
            for (unsigned k = i->ndst; k < i->nops && ok; k++)
               ok = !ref_hits(&i->op[k], i->rpt, d.file, d.n, dn, m->rpt);
            break;
         }
         if (ins_hits(i, d.file, d.n, dn, m->rpt))
            break;
      }
      if (!p || !ok)
         continue;
      /* t's other references: only before p in this run of instructions,
       * the first of them an unconditional write of all of it */
      struct list_head *start = &p->link;
      while (start->prev != &c->ins && list_entry(start->prev, struct sgx_ins, link)->kind == SGX_INS_ALU)
         start = start->prev;
      bool seen = false, in_run = false;
      list_for_each_entry(struct sgx_ins, i, &c->ins, link) {
         if (&i->link == start)
            in_run = true;
         if (i == p)
            in_run = false;
         if (i == m || i == p || !ins_hits(i, SGX_F_V, s.n, s.comp, m->rpt))
            continue;
         if (!in_run) {
            ok = false;
            break;
         }
         if (!seen) {
            bool rd = false;
            for (unsigned k = i->ndst; k < i->nops; k++)
               rd |= ref_hits(&i->op[k], i->rpt, SGX_F_V, s.n, s.comp, m->rpt);
            if (rd || i->pred || i->ndst != 1 || i->op[0].file != SGX_F_V || i->op[0].n != s.n ||
                i->op[0].comp > s.comp || i->op[0].comp + (i->op[0].step ? i->rpt : 1) < s.comp + m->rpt) {
               ok = false;
               break;
            }
            seen = true;
         }
      }
      if (!ok)
         continue;
      p->op[0] = d;
      list_del(&m->link);
   }
}

/* the register file and number a plain register operand ends up in, after
 * allocation; false for anything else */
static bool
reg_of(const struct sgx_ctx *c, const struct sgx_ref *r, unsigned *file, unsigned *n)
{
   if (r->neg || r->abs || r->h16 || r->lane || r->c10)
      return false;
   switch (r->file) {
   case SGX_F_V: {
      const struct sgx_vreg *v = sgx_vreg((struct sgx_ctx *)c, r->n);
      *file = v->pa ? SGX_F_PA : v->o ? SGX_F_O : SGX_F_R;
      *n = v->phys + r->comp;
      return true;
   }
   case SGX_F_R: case SGX_F_PA: case SGX_F_O:
      *file = r->file;
      *n = r->n;
      return true;
   default:
      return false;
   }
}

/* a mov whose source and destination landed in the same registers (a value
 * placed where its output goes) */
static void
drop_self_moves(struct sgx_ctx *c)
{
   list_for_each_entry_safe(struct sgx_ins, i, &c->ins, link) {
      unsigned fd, nd, fs, ns;
      if (i->kind == SGX_INS_ALU && i->form == SGX_OP_MOV && !i->pred && i->nops == 2 && !i->nfld &&
          (i->rpt == 1 || (i->op[0].step && i->op[1].step)) &&
          reg_of(c, &i->op[0], &fd, &nd) && reg_of(c, &i->op[1], &fs, &ns) && fd == fs && nd == ns)
         list_del(&i->link);
   }
}

/* ------------------------------------------------------------------------
 * helper instances
 *
 * A derivative reads its 2x2 neighbours, and a dependent sample takes its LOD
 * from them, so whatever feeds one runs with skipinv clear: helper instances
 * then hold real values (the DDK's dsy, fwidth and tx_dep words). Flow-
 * insensitive: every writer of a needed register is needed, and with any of
 * them, the branch predicates too. PA isn't tracked: the iterations fill it
 * on every instance, and the program writes it only for its outputs. */

enum { NEED_R = 0, NEED_I = 128, NEED_P = 132, NEED_IDX = 136, NEED_V = 140 };

/* mark (or test) the registers r covers */
static bool
need_ref(BITSET_WORD *nd, const struct sgx_ref *r, unsigned rpt, bool mark)
{
   unsigned base, lim;
   switch (r->file) {
   case SGX_F_V: base = NEED_V; lim = ~0u; break;
   case SGX_F_R: base = NEED_R; lim = 128; break;
   case SGX_F_I: base = NEED_I; lim = 4; break;
   case SGX_F_P: base = NEED_P; lim = 4; break;
   case SGX_F_IDX: base = NEED_IDX; lim = 4; break;
   case SGX_F_IXSA: base = NEED_IDX + 1; lim = 1; break;
   default: return false;
   }
   unsigned lo = r->file == SGX_F_IXSA ? 0 : r->n, hi = lo;
   if (r->file != SGX_F_V && r->file != SGX_F_IXSA) {
      hi += r->step && rpt > 1 ? rpt - 1 : 0;
      if (r->h16) {
         lo /= 2;
         hi /= 2;
      }
   }
   bool any = false;
   for (unsigned k = lo; k <= hi && k < lim; k++) {
      any |= BITSET_TEST(nd, base + k);
      if (mark)
         BITSET_SET(nd, base + k);
   }
   return any;
}

static void
mark_helpers(struct sgx_ctx *c)
{
   unsigned nv = util_dynarray_num_elements(&c->vregs, struct sgx_vreg);
   BITSET_WORD *nd = rzalloc_array(c, BITSET_WORD, BITSET_WORDS(NEED_V + nv));
   bool any = false;
   list_for_each_entry(struct sgx_ins, i, &c->ins, link) {
      if (i->kind == SGX_INS_ALU && (i->form == SGX_OP_FDSX || i->form == SGX_OP_FDSY)) {
         for (unsigned k = i->ndst; k < i->nops; k++)
            need_ref(nd, &i->op[k], i->rpt, true);
         any = true;
      } else if (i->kind == SGX_INS_SAMPLE && i->smp->dependent) {
         need_ref(nd, &i->op[1], 1, true);
         any = true;
      }
   }
   if (!any)
      return;
   list_for_each_entry(struct sgx_ins, i, &c->ins, link)
      if (i->kind == SGX_INS_BR && i->pred)
         BITSET_SET(nd, NEED_P + (i->pred & 3) - 1);
   for (bool progress = true; progress;) {
      progress = false;
      list_for_each_entry(struct sgx_ins, i, &c->ins, link) {
         if (i->kind != SGX_INS_ALU || i->noskip)
            continue;
         bool hit = i->writes_pred && BITSET_TEST(nd, NEED_P + i->writes_pred - 1);
         for (unsigned k = 0; k < i->ndst && !hit; k++)
            hit = need_ref(nd, &i->op[k], i->rpt, false);
         if (!hit)
            continue;
         i->noskip = true;
         progress = true;
         for (unsigned k = i->ndst; k < i->nops; k++)
            need_ref(nd, &i->op[k], i->rpt, true);
         if (i->pred)
            BITSET_SET(nd, NEED_P + (i->pred & 3) - 1);
      }
   }
}

/* ------------------------------------------------------------------------
 * liveness and register allocation */

static void
touch(struct sgx_ctx *c, const struct sgx_ref *r, int at, unsigned span)
{
   if (r->file != SGX_F_V)
      return;
   struct sgx_vreg *v = sgx_vreg(c, r->n);
   (void)span;
   if (v->first < 0 || at < v->first)
      v->first = at;
   if (at > v->last)
      v->last = at;
}

/* Temporaries in PA registers the program no longer needs: an input after
 * its last read, any PA register below the count the program has anyway
 * (fragment: iterations and colour; vertex: the attributes it reads), as
 * libusc does it. Fewer temporaries per instance, more instances resident. */
/* A 32-bit vreg nothing reads or writes as a run (no repeat steps through
 * it) becomes one vreg per component: each is allocated, or placed in PA,
 * for its own live range (a vec4 built a component at a time). */
static void
split_vregs(struct sgx_ctx *c)
{
   unsigned nv = util_dynarray_num_elements(&c->vregs, struct sgx_vreg);
   bool *run = calloc(nv, sizeof(bool));
   list_for_each_entry(struct sgx_ins, i, &c->ins, link)
      for (unsigned k = 0; k < i->nops; k++)
         if (i->op[k].file == SGX_F_V && i->op[k].n < nv &&
             (i->op[k].h16 || (i->op[k].step && i->rpt > 1)))
            run[i->op[k].n] = true;
   int *split = malloc(nv * 16 * sizeof(int));
   for (unsigned v = 0; v < nv * 16; v++)
      split[v] = -1;
   list_for_each_entry(struct sgx_ins, i, &c->ins, link) {
      for (unsigned k = 0; k < i->nops; k++) {
         struct sgx_ref *r = &i->op[k];
         if (r->file != SGX_F_V || r->n >= nv || run[r->n])
            continue;
         struct sgx_vreg *x = sgx_vreg(c, r->n);
         if (x->half || x->size < 2 || x->size > 16)
            continue;
         int *slot = &split[r->n * 16 + r->comp];
         if (*slot < 0)
            *slot = new_vreg(c, 1);
         r->n = *slot;
         r->comp = 0;
      }
   }
   free(split);
   free(run);
}

/* A repeat whose source vreg dies here, read at the component its
 * destination writes and stepping with it, writes in place: iteration k
 * reads component k before writing it, and no later iteration reads what an
 * earlier one wrote (libusc's fmad pa0, pa0, pa4, pa8 rpt=4). The
 * destination vreg becomes the source's, so a mat4 * v chain needs one
 * register run, not two. */
static void
coalesce_repeats(struct sgx_ctx *c)
{
   unsigned nv = util_dynarray_num_elements(&c->vregs, struct sgx_vreg);
   list_for_each_entry(struct sgx_ins, i, &c->ins, link) {
      if (i->kind != SGX_INS_ALU || i->rpt < 2 || i->ndst != 1 || i->pred ||
          i->form == SGX_OP_FDSX || i->form == SGX_OP_FDSY || i->form == SGX_OP_FDP)
         continue;
      struct sgx_ref d = i->op[0];
      if (d.file != SGX_F_V || d.h16 || !d.step || d.n >= nv)
         continue;
      struct sgx_vreg *t1 = sgx_vreg(c, d.n);
      if (t1->half || t1->first != (int)(2 * i->ip) || (int)d.n == c->keep_vreg || (int)d.n == c->face_vreg)
         continue;
      int src = -1;
      for (unsigned k = 1; k < i->nops; k++) {
         const struct sgx_ref *r = &i->op[k];
         if (r->file != SGX_F_V || r->n == d.n || r->h16 || !r->step || r->comp != d.comp)
            continue;
         struct sgx_vreg *t0 = sgx_vreg(c, r->n);
         if (!t0->half && t0->last == (int)(2 * i->ip) && (int)r->n != c->keep_vreg &&
             (int)r->n != c->face_vreg && (int)r->n != c->face_f32)
            src = r->n;
         break;
      }
      if (src < 0)
         continue;
      /* every other operand naming the source reads it the same way */
      bool ok = true;
      for (unsigned k = 1; k < i->nops && ok; k++) {
         const struct sgx_ref *r = &i->op[k];
         if (r->file == SGX_F_V && r->n == (unsigned)src)
            ok = r->comp == d.comp && r->step && !r->h16;
      }
      if (!ok)
         continue;
      struct sgx_vreg *t0 = sgx_vreg(c, src);
      unsigned from = d.n;
      list_for_each_entry(struct sgx_ins, j, &c->ins, link)
         for (unsigned k = 0; k < j->nops; k++)
            if (j->op[k].file == SGX_F_V && j->op[k].n == from)
               j->op[k].n = src;
      t0->last = t1->last;
      t0->size = MAX2(t0->size, t1->size);
      t1->first = t1->last = -1;
   }
}

struct loop_span {
   int head, tail;
};

/* Vertex shader: temporaries in output registers before the program's
 * first write of them (a mat4 * v accumulator in the o0-o3 it ends in, in
 * place at the last step, as coalesce_repeats). Outputs are read back like
 * any source; only their final value leaves the shader. */
static void
place_in_o(struct sgx_ctx *c, const BITSET_WORD *six, unsigned nins,
           const struct loop_span *loops, unsigned nloops, const unsigned *minb)
{
   unsigned nv = util_dynarray_num_elements(&c->vregs, struct sgx_vreg);
   unsigned end = 2 * nins + 2, no = 0;
   int from[128];
   for (unsigned p = 0; p < 128; p++)
      from[p] = -1;
   list_for_each_entry(struct sgx_ins, i, &c->ins, link) {
      for (unsigned k = 0; k < i->nops; k++) {
         const struct sgx_ref *r = &i->op[k];
         if (r->file != SGX_F_O)
            continue;
         unsigned n = r->step && i->rpt > 1 ? i->rpt : 1;
         int at = 2 * i->ip + (k < i->ndst && (i->rpt == 1 || i->form == SGX_OP_FDP));
         for (unsigned j = r->n; j < r->n + n && j < 128; j++) {
            if (from[j] < 0 || at < from[j])
               from[j] = at;
            no = MAX2(no, j + 1);
         }
      }
   }
   for (unsigned p = 0; p < no; p++)
      for (unsigned l = 0; l < nloops; l++)
         if (from[p] > loops[l].head && from[p] <= loops[l].tail)
            from[p] = loops[l].head;
   /* the in-place step: an explicit output write reading, aligned, a vreg
    * that dies there */
   int *inplace = malloc(nv * sizeof(int));
   for (unsigned v = 0; v < nv; v++)
      inplace[v] = -1;
   list_for_each_entry(struct sgx_ins, i, &c->ins, link) {
      const struct sgx_ref *d = &i->op[0];
      if (i->kind != SGX_INS_ALU || i->ndst != 1 || i->pred || d->file != SGX_F_O || !d->step ||
          i->form == SGX_OP_FDSX || i->form == SGX_OP_FDSY || i->form == SGX_OP_FDP)
         continue;
      for (unsigned k = 1; k < i->nops; k++) {
         const struct sgx_ref *r = &i->op[k];
         if (r->file != SGX_F_V || r->n >= nv || r->h16 || (i->rpt > 1 && !r->step) || r->comp > d->n ||
             sgx_vreg(c, r->n)->last != (int)(2 * i->ip))
            continue;
         bool good = true;
         for (unsigned j = 1; j < i->nops && good; j++)
            if (j != k && i->op[j].file == SGX_F_V && i->op[j].n == r->n)
               good = i->op[j].comp == r->comp && i->op[j].step == r->step && !i->op[j].h16;
         if (good)
            inplace[r->n] = d->n - r->comp;
         break;
      }
   }
   bool *ok = calloc(nv, sizeof(bool));
   for (unsigned v = 0; v < nv; v++) {
      struct sgx_vreg *r = sgx_vreg(c, v);
      ok[v] = r->first >= 0 && !r->pa && !r->half && (int)v != c->keep_vreg && (int)v != c->face_vreg &&
              (int)v != c->face_f32;
   }
   list_for_each_entry(struct sgx_ins, i, &c->ins, link) {
      for (unsigned k = 0; k < i->nops; k++) {
         const struct sgx_ref *r = &i->op[k];
         if (r->file != SGX_F_V || r->n >= nv)
            continue;
         struct sgx_ref x = *r;
         x.file = SGX_F_O;
         if (i->kind != SGX_INS_ALU || r->h16 || !slot_ok(i->form, k, &x))
            ok[r->n] = false;
      }
   }
   /* what the placed vregs hold, per register */
   BITSET_WORD *busy = rzalloc_array(c, BITSET_WORD, MAX2(no, 1) * BITSET_WORDS(end));
#define OBUSY(p) (busy + (p) * BITSET_WORDS(end))
   for (unsigned p = 0; p < no; p++)
      if (from[p] >= 0)
         BITSET_SET_RANGE(OBUSY(p), from[p], end - 1);
      else
         BITSET_SET_RANGE(OBUSY(p), 0, end - 1);   /* not an output the program writes */
   for (unsigned v = 0; v < nv; v++) {
      struct sgx_vreg *r = sgx_vreg(c, v);
      if (!ok[v])
         continue;
      unsigned lo = r->first, hi = r->last, lim = BITSET_TEST(six, v) ? 64 : 128;
      for (unsigned b = minb[v]; b + r->size <= MIN2(no, lim); b++) {
         bool free = true;
         for (unsigned p = b; p < b + r->size && free; p++)
            for (unsigned x = lo; x <= hi && x < end && free; x++)
               free = !BITSET_TEST(OBUSY(p), x) || ((int)b == inplace[v] && x == hi);
         if (!free)
            continue;
         for (unsigned p = b; p < b + r->size; p++)
            BITSET_SET_RANGE(OBUSY(p), lo, MIN2(hi, end - 1));
         r->o = true;
         r->phys = b;
         break;
      }
   }
#undef OBUSY
   free(ok);
   free(inplace);
}

static void
place_in_pa(struct sgx_ctx *c, const BITSET_WORD *six, unsigned nins,
            const struct loop_span *loops, unsigned nloops, const unsigned *minb)
{
   unsigned nv = util_dynarray_num_elements(&c->vregs, struct sgx_vreg);
   unsigned end = 2 * nins + 2, npa = 0;
   bool input[128] = { 0 };
   if (c->stage == MESA_SHADER_FRAGMENT) {
      bool real = false;
      for (unsigned i = 0; i < c->n_iters; i++) {
         const struct sgx_iter *it = &c->iters[i];
         npa = MAX2(npa, it->pa + (it->sample ? 1 : it->f16 ? (it->comps + 1) / 2 : it->comps));
         real |= !it->pointcoord;
      }
      if (!real)
         npa++;
      for (unsigned p = 0; p < npa && p < 128; p++)
         input[p] = true;
      npa = MAX2(npa, c->colour_pa + 1);
      if (c->has_depth)
         npa = MAX2(npa, c->depth_pa + 1);
   }
   /* what the program does with PA itself: inputs busy until their last
    * read, a written register (colour, depth) from its write to the end */
   int last[128], first_w[128];
   for (unsigned p = 0; p < 128; p++)
      last[p] = first_w[p] = -1;
   list_for_each_entry(struct sgx_ins, i, &c->ins, link) {
      /* a finalizer that samples a non-dependent texture with USSE code
       * reads the coordinate's plain iteration there (ISA.md, "Code
       * generation") */
      if (i->kind == SGX_INS_SAMPLE && !i->smp->dependent && i->smp->coord_pa >= 0) {
         for (unsigned n = 0; n < c->n_iters; n++) {
            const struct sgx_iter *it = &c->iters[n];
            if (it->sample || (int)it->pa != i->smp->coord_pa)
               continue;
            for (unsigned j = it->pa; j < it->pa + (it->f16 ? (it->comps + 1) / 2 : it->comps) && j < 128; j++)
               last[j] = MAX2(last[j], (int)(2 * i->ip));
         }
      }
      for (unsigned k = 0; k < i->nops; k++) {
         const struct sgx_ref *r = &i->op[k];
         if (r->file != SGX_F_PA)
            continue;
         unsigned lo = r->h16 ? r->n / 2 : r->n;
         unsigned n = r->step && i->rpt > 1 ? i->rpt : 1;
         for (unsigned j = lo; j < lo + n && j < 128; j++) {
            if (k < i->ndst) {
               /* (a single instruction reads before it writes: the colour's
                * own sources may sit in the colour register) */
               if (first_w[j] < 0)
                  first_w[j] = 2 * i->ip + (i->rpt == 1 || i->form == SGX_OP_FDP);
            } else {
               last[j] = MAX2(last[j], (int)(2 * i->ip));
               if (c->stage == MESA_SHADER_VERTEX)
                  input[j] = true;
            }
         }
      }
   }
   if (c->stage == MESA_SHADER_VERTEX)
      for (unsigned p = 0; p < 128; p++)
         if (input[p])
            npa = p + 1;
   /* in a loop, the next iteration reads (and writes) it again: busy over
    * the whole loop */
   for (bool changed = true; changed;) {
      changed = false;
      for (unsigned p = 0; p < 128; p++) {
         for (unsigned l = 0; l < nloops; l++) {
            if (last[p] >= loops[l].head && last[p] < loops[l].tail) {
               last[p] = loops[l].tail;
               changed = true;
            }
            if (first_w[p] > loops[l].head && first_w[p] <= loops[l].tail) {
               first_w[p] = loops[l].head;
               changed = true;
            }
         }
      }
   }
   if (!npa)
      return;
   BITSET_WORD *busy = rzalloc_array(c, BITSET_WORD, npa * BITSET_WORDS(end));
#define BUSY(p) (busy + (p) * BITSET_WORDS(end))
   for (unsigned p = 0; p < npa; p++) {
      if (!input[p] && c->stage == MESA_SHADER_VERTEX) {
         BITSET_SET_RANGE(BUSY(p), 0, end - 1);
         continue;
      }
      if (input[p] && last[p] >= 0)
         BITSET_SET_RANGE(BUSY(p), 0, last[p]);
      if (first_w[p] >= 0)
         BITSET_SET_RANGE(BUSY(p), first_w[p], end - 1);
   }
   /* a vreg every reference of which can name PA */
   bool *ok = rzalloc_array(c, bool, nv);
   for (unsigned v = 0; v < nv; v++)
      ok[v] = sgx_vreg(c, v)->first >= 0 && (int)v != c->keep_vreg && (int)v != c->face_vreg &&
              (int)v != c->face_f32;
   list_for_each_entry(struct sgx_ins, i, &c->ins, link) {
      for (unsigned k = 0; k < i->nops; k++) {
         const struct sgx_ref *r = &i->op[k];
         if (r->file != SGX_F_V || r->n >= nv)
            continue;
         struct sgx_ref x = *r;
         x.file = SGX_F_PA;
         if (i->kind != SGX_INS_ALU || !slot_ok(i->form, k, &x))
            ok[r->n] = false;
      }
   }
   /* in place: a repeat that starts a vreg and reads, at the component it
    * writes and stepping with it, an input for the last time may write over
    * that input (coalesce_repeats' rule) */
   int *inplace = malloc(nv * sizeof(int));
   for (unsigned v = 0; v < nv; v++)
      inplace[v] = -1;
   list_for_each_entry(struct sgx_ins, i, &c->ins, link) {
      const struct sgx_ref *d = &i->op[0];
      if (i->kind != SGX_INS_ALU || i->rpt < 2 || i->ndst != 1 || i->pred || d->file != SGX_F_V ||
          d->h16 || !d->step || d->n >= nv || sgx_vreg(c, d->n)->first != (int)(2 * i->ip) ||
          i->form == SGX_OP_FDSX || i->form == SGX_OP_FDSY || i->form == SGX_OP_FDP)
         continue;
      int base = -1;
      for (unsigned k = 1; k < i->nops && base < 0; k++) {
         const struct sgx_ref *r = &i->op[k];
         if (r->file == SGX_F_PA && r->step && !r->h16 && r->n >= d->comp && r->n + i->rpt <= npa)
            base = r->n - d->comp;
      }
      bool good = base >= 0;
      for (unsigned j = 0; good && j < i->rpt; j++)
         good = last[base + d->comp + j] == (int)(2 * i->ip);
      /* (every operand naming those registers reads them aligned) */
      for (unsigned k = 1; good && k < i->nops; k++) {
         const struct sgx_ref *r = &i->op[k];
         if (r->file == SGX_F_PA && r->n + (r->step ? i->rpt : 1) > (unsigned)base + d->comp &&
             r->n < (unsigned)base + d->comp + i->rpt)
            good = r->step && !r->h16 && r->n == (unsigned)base + d->comp;
      }
      if (good)
         inplace[d->n] = base;
   }
   /* each component's own last use (a 32-bit vreg's register k is free
    * once component k is: the colour's first pck reads .x, its second .z) */
   int *lastc = malloc(nv * 16 * sizeof(int)), *firstc = malloc(nv * 16 * sizeof(int));
   bool *odd = calloc(nv, sizeof(bool));
   for (unsigned v = 0; v < nv * 16; v++) {
      lastc[v] = -1;
      firstc[v] = INT_MAX;
   }
   list_for_each_entry(struct sgx_ins, i, &c->ins, link) {
      for (unsigned k = 0; k < i->nops; k++) {
         const struct sgx_ref *r = &i->op[k];
         if (r->file != SGX_F_V || r->n >= nv)
            continue;
         bool half = sgx_vreg(c, r->n)->half;
         if (r->h16 != half) {
            odd[r->n] = true;
            continue;
         }
         /* (an F16 vreg by register: a repeat steps a register at a time) */
         unsigned n = r->step && i->rpt > 1 ? i->rpt : 1;
         unsigned first = half ? r->comp / 2 : r->comp;
         bool early = (i->rpt > 1 && i->form != SGX_OP_FDP) || i->kind == SGX_INS_SAMPLE ||
                      (i->kind == SGX_INS_ALU && (i->form == SGX_OP_FDSX || i->form == SGX_OP_FDSY));
         int at = 2 * i->ip + (k < i->ndst && !early);
         for (unsigned j = first; j < first + n && j < 16; j++) {
            lastc[r->n * 16 + j] = MAX2(lastc[r->n * 16 + j], at);
            firstc[r->n * 16 + j] = MIN2(firstc[r->n * 16 + j], at);
         }
      }
   }
   for (unsigned v = 0; v < nv; v++) {
      struct sgx_vreg *r = sgx_vreg(c, v);
      int m = -1, f = INT_MAX;
      for (unsigned j = 0; j < 16; j++) {
         m = MAX2(m, lastc[v * 16 + j]);
         f = MIN2(f, firstc[v * 16 + j]);
      }
      /* (a loop keeps it all: the allocator's range) */
      if ((r->half ? (r->size + 1) / 2 : r->size) > 16 || m < r->last || f > r->first || odd[v]) {
         for (unsigned j = 0; j < 16; j++) {
            lastc[v * 16 + j] = r->last;
            firstc[v * 16 + j] = r->first;
         }
      }
   }
   for (unsigned v = 0; v < nv; v++) {
      struct sgx_vreg *r = sgx_vreg(c, v);
      if (!ok[v])
         continue;
      unsigned regs = r->half ? (r->size + 1) / 2 : r->size;
      unsigned lim = r->half ? 32 : BITSET_TEST(six, v) ? 64 : 128;
      unsigned lo = r->first, hi = r->last;
      for (unsigned b = minb[v]; b + regs <= MIN2(npa, lim); b++) {
         bool free = true;
         for (unsigned p = b; p < b + regs && free; p++) {
            unsigned l = MIN2(MAX2(firstc[v * 16 + p - b], (int)lo), (int)hi);
            unsigned h = (unsigned)MAX2(lastc[v * 16 + p - b], (int)l);
            for (unsigned x = l; x <= h && x < end && free; x++)
               free = !BITSET_TEST(BUSY(p), x) || ((int)b == inplace[v] && x == lo);
         }
         if (!free)
            continue;
         for (unsigned p = b; p < b + regs; p++) {
            unsigned l = MIN2(MAX2(firstc[v * 16 + p - b], (int)lo), (int)hi);
            unsigned h = (unsigned)MAX2(lastc[v * 16 + p - b], (int)l);
            BITSET_SET_RANGE(BUSY(p), l, MIN2(h, end - 1));
         }
         r->pa = true;
         r->phys = b;
         break;
      }
   }
   free(inplace);
   free(lastc);
   free(firstc);
   free(odd);
#undef BUSY
}

static bool
allocate(struct sgx_ctx *c)
{
   split_vregs(c);
   unsigned nv = util_dynarray_num_elements(&c->vregs, struct sgx_vreg);
   int ip = 0;
   struct loop_span loops[64];
   unsigned nloops = 0;
   int label_ip[256];
   for (unsigned i = 0; i < 256; i++)
      label_ip[i] = -1;

   /* uses at 2*ip, writes at 2*ip+1, except that a repeated instruction's
    * destination is live while it reads (iterations interleave; not fdp's,
    * which writes once, after its last read), and so is a derivative's (a
    * neighbour instance reads the source after this one has written) */
   bool *first_is_read = calloc(nv, sizeof(bool));
   list_for_each_entry(struct sgx_ins, i, &c->ins, link) {
      i->ip = ip;
      if (i->kind == SGX_INS_LABEL && i->label < 256)
         label_ip[i->label] = ip;
      if (i->kind == SGX_INS_BR && i->label < 256 && label_ip[i->label] >= 0 && nloops < 64) {
         loops[nloops].head = 2 * label_ip[i->label];
         loops[nloops++].tail = 2 * ip + 1;
      }
      for (unsigned k = i->ndst; k < i->nops; k++) {
         const struct sgx_ref *r = &i->op[k];
         if (r->file == SGX_F_V && sgx_vreg(c, r->n)->first < 0)
            first_is_read[r->n] = true;
         touch(c, r, 2 * ip, 0);
      }
      bool early = (i->rpt > 1 && i->form != SGX_OP_FDP) || i->kind == SGX_INS_SAMPLE ||
                   (i->kind == SGX_INS_ALU && (i->form == SGX_OP_FDSX || i->form == SGX_OP_FDSY));
      for (unsigned k = 0; k < i->ndst; k++)
         touch(c, &i->op[k], early ? 2 * ip : 2 * ip + 1, 0);
      /* a predicated write keeps the old value */
      if (i->pred && i->ndst && i->op[0].file == SGX_F_V)
         touch(c, &i->op[0], 2 * ip, 0);
      ip++;
   }
   /* loops: anything live across the back edge lives through the whole loop */
   bool changed = true;
   while (changed) {
      changed = false;
      for (unsigned v = 0; v < nv; v++) {
         struct sgx_vreg *r = sgx_vreg(c, v);
         if (r->first < 0)
            continue;
         for (unsigned l = 0; l < nloops; l++) {
            int h = loops[l].head, t = loops[l].tail;
            bool overlaps = r->first <= t && r->last >= h;
            bool inside = r->first >= h && r->last <= t;
            if (overlaps && (!inside || first_is_read[v])) {
               if (r->first > h) { r->first = h; changed = true; }
               if (r->last < t) { r->last = t; changed = true; }
            }
         }
      }
   }
   free(first_is_read);
   coalesce_repeats(c);

   /* F16 values in r0-r31: under format select an F16 operand's number
    * counts halves, so only those registers can be named (ISA.md "Float
    * classes") */
   struct ra_regs *regs = ra_alloc_reg_set(c, NUM_TEMPS, true);
   struct ra_class *cls[17] = { 0 }, *cls_h[17] = { 0 }, *cls_6[17] = { 0 };
   struct ra_class *cls_b[4] = { 0 }, *cls_b6[4] = { 0 };   /* one register from b up */
   for (unsigned b = 1; b < 4; b++) {
      cls_b[b] = ra_alloc_contig_reg_class(regs, 1);
      cls_b6[b] = ra_alloc_contig_reg_class(regs, 1);
      for (unsigned r = b; r < NUM_TEMPS; r++)
         ra_class_add_reg(cls_b[b], r);
      for (unsigned r = b; r < 64; r++)
         ra_class_add_reg(cls_b6[b], r);
   }
   for (unsigned s = 1; s <= 16; s++) {
      cls[s] = ra_alloc_contig_reg_class(regs, s);
      for (unsigned b = 0; b + s <= NUM_TEMPS; b++)
         ra_class_add_reg(cls[s], b);
      cls_h[s] = ra_alloc_contig_reg_class(regs, s);
      for (unsigned b = 0; b + s <= 32; b++)
         ra_class_add_reg(cls_h[s], b);
      cls_6[s] = ra_alloc_contig_reg_class(regs, s);
      for (unsigned b = 0; b + s <= 64; b++)
         ra_class_add_reg(cls_6[s], b);
   }
   ra_set_finalize(regs, NULL);
   /* (an f32 operand beside an F16 one has a 6-bit number too: r0-r63) */
   BITSET_WORD *six = rzalloc_array(c, BITSET_WORD, BITSET_WORDS(nv));
   list_for_each_entry(struct sgx_ins, i, &c->ins, link) {
      bool fmt = false;
      for (unsigned k = 0; k < i->nops; k++)
         fmt |= i->op[k].h16 && i->op[k].file != SGX_F_K && i->form != SGX_OP_FMAD16;
      for (unsigned k = 0; fmt && k < i->nops; k++)
         if (i->op[k].file == SGX_F_V && i->op[k].n < nv)
            BITSET_SET(six, i->op[k].n);
   }
   /* a 32-bit vreg needs only the components something names (a bool's
    * mask vreg of 4 written in .x) */
   unsigned *used = calloc(nv, sizeof(unsigned));
   bool *halfref = calloc(nv, sizeof(bool));
   list_for_each_entry(struct sgx_ins, i, &c->ins, link) {
      for (unsigned k = 0; k < i->nops; k++) {
         const struct sgx_ref *r = &i->op[k];
         if (r->file != SGX_F_V || r->n >= nv)
            continue;
         halfref[r->n] |= r->h16;
         used[r->n] = MAX2(used[r->n], r->comp + (r->step && i->rpt > 1 ? i->rpt : 1));
      }
   }
   for (unsigned v = 0; v < nv; v++) {
      struct sgx_vreg *r = sgx_vreg(c, v);
      if (!r->half && !halfref[v] && used[v] && used[v] < r->size)
         r->size = used[v];
   }
   free(used);
   free(halfref);
   /* an fdp's result register sits rpt - 1 above its operand number */
   unsigned *minb = calloc(nv, sizeof(unsigned));
   list_for_each_entry(struct sgx_ins, i, &c->ins, link)
      if (i->kind == SGX_INS_ALU && i->form == SGX_OP_FDP && i->op[0].file == SGX_F_V && i->op[0].n < nv)
         minb[i->op[0].n] = MAX2(minb[i->op[0].n], i->rpt - 1u);
   place_in_pa(c, six, ip, loops, nloops, minb);
   if (c->stage == MESA_SHADER_VERTEX)
      place_in_o(c, six, ip, loops, nloops, minb);
   struct ra_graph *g = ra_alloc_interference_graph(regs, nv);
   for (unsigned v = 0; v < nv; v++) {
      struct sgx_vreg *r = sgx_vreg(c, v);
      unsigned regs = r->half ? (r->size + 1) / 2 : r->size;
      if (minb[v] && !r->half && regs <= 1)
         ra_set_node_class(g, v, (BITSET_TEST(six, v) ? cls_b6 : cls_b)[MIN2(minb[v], 3)]);
      else
         ra_set_node_class(g, v, (r->half ? cls_h : BITSET_TEST(six, v) ? cls_6 : cls)[MIN2(MAX2(regs, 1), 16)]);
   }
   for (unsigned a = 0; a < nv; a++) {
      struct sgx_vreg *x = sgx_vreg(c, a);
      if (x->first < 0 || x->pa || x->o)
         continue;
      for (unsigned b = a + 1; b < nv; b++) {
         struct sgx_vreg *y = sgx_vreg(c, b);
         if (y->first >= 0 && !y->pa && !y->o && x->first <= y->last && y->first <= x->last)
            ra_add_node_interference(g, a, b);
      }
   }
   free(minb);
   if (!ra_allocate(g)) {
      sgx_fail(c, "out of temporaries (no spilling yet)");
      return false;
   }
   c->n_temps = 0;
   for (unsigned v = 0; v < nv; v++) {
      struct sgx_vreg *r = sgx_vreg(c, v);
      if (r->first < 0 || r->pa || r->o)
         continue;
      r->phys = ra_get_node_reg(g, v);
      c->n_temps = MAX2(c->n_temps, r->phys + (r->half ? (r->size + 1) / 2 : r->size));
   }
   return true;
}

/* ------------------------------------------------------------------------
 * encoding */

/* constants -> SA slots by format: f32 one each; F16 two per slot, by pairs
 * of constant numbers (2m, 2m+1); C10 three per slot, constant k of a
 * 4-aligned group at bits 20 - 10 * (k % 4) */
static void
uniform_layout(struct sgx_ctx *c)
{
   c->n_sa = 0;
   int pair_slot = -1, pair = -1, grp_slot = -1, grp = -1;
   for (unsigned k = 0; k < ARRAY_SIZE(c->const_used); k++) {
      c->const_slot[k] = -1;
      c->const_shift[k] = 0;
      if (!c->const_used[k])
         continue;
      unsigned fmt = c->const_fmt[k] ? c->const_fmt[k] : 1;
      if (fmt == 2 || fmt == 5) {
         if (pair != (int)(k / 2)) {
            pair = k / 2;
            pair_slot = c->n_sa++;
         }
         c->const_slot[k] = pair_slot;
         c->const_shift[k] = (k & 1) * 16;
      } else if (fmt == 4) {
         /* C10 on its own (a lowp vector's w): channel 0 of a slot */
         c->const_slot[k] = c->n_sa++;
      } else if (fmt == 3) {
         if (grp != (int)(k / 4)) {
            grp = k / 4;
            grp_slot = c->n_sa++;
         }
         c->const_slot[k] = grp_slot;
         c->const_shift[k] = 20 - 10 * (k % 4);
      } else {
         c->const_slot[k] = c->n_sa++;
      }
   }
   for (unsigned i = 0; i < c->n_lits; i++)
      c->lit_slot[i] = c->n_sa++;
}

/* the slots an operand reads: first, and how many (a stepping repeat) */
static bool
operand_slots(const struct sgx_ctx *c, const struct sgx_ins *i, const struct sgx_ref *r,
              unsigned *first, unsigned *n)
{
   if (r->file == SGX_F_U)
      *first = c->const_slot[r->n];
   else if (r->file == SGX_F_LIT)
      *first = c->lit_slot[r->n];
   else
      return false;
   unsigned rpt = r->step && i->rpt > 1 ? i->rpt : 1;
   unsigned half = r->h16 && r->file == SGX_F_U ? c->const_shift[r->n] / 16 : 0;
   *n = r->h16 ? (half + rpt + 1) / 2 : rpt;
   return true;
}

/* An instruction names sa0-sa127. The constants go from sa_base, a
 * fragment program's dependent texture state (4 each) after them. When the
 * slots don't fit, every slot also gets block word slot + 1 (ld.a.d's
 * offset #i reads base + 4 * (i + 1): iso sanity_st), indexed arrays that
 * fit whole and then the most-used constants stay in SA, and mem_index /
 * mem_uniforms load the rest. */
static void
place_constants(struct sgx_ctx *c)
{
   unsigned deps = 0;
   for (unsigned s = 0; s < c->n_samples; s++)
      deps += c->samples[s]->dependent;
   unsigned budget = 128 - c->sa_base - MIN2(4 * deps, 128 - c->sa_base);
   c->mem_dwords = c->n_mem_bases = 0;
   for (unsigned s = 0; s < c->n_sa; s++)
      c->slot_sa[s] = s;
   c->n_sa_phys = c->n_sa;
   if (c->n_sa <= budget)
      return;

   unsigned nb = DIV_ROUND_UP(c->n_sa, 128);
   unsigned room = budget > nb ? budget - nb : 0, kept = 0;
   unsigned *uses = calloc(c->n_sa, sizeof(*uses));
   bool *keep = calloc(c->n_sa, sizeof(*keep));
   list_for_each_entry(struct sgx_ins, i, &c->ins, link) {
      unsigned s, n;
      for (unsigned k = i->ndst; k < i->nops; k++)
         if (operand_slots(c, i, &i->op[k], &s, &n))
            for (unsigned j = 0; j < n && s + j < c->n_sa; j++)
               uses[s + j]++;
   }
   /* indexed arrays whole (sa[idx1 + n]), smallest first; the others are
    * read from memory (mem_index) */
   bool *done = calloc(c->n_ix_arr, sizeof(*done));
   for (;;) {
      int best = -1;
      for (unsigned a = 0; a < c->n_ix_arr; a++)
         if (!done[a] && (best < 0 || c->ix_arr[a].range < c->ix_arr[best].range))
            best = a;
      if (best < 0)
         break;
      done[best] = true;
      unsigned k = c->ix_arr[best].k, need = 0;
      for (unsigned j = 0; j < c->ix_arr[best].range; j++)
         need += !keep[c->const_slot[k + j]];
      if (kept + need > room)
         continue;
      for (unsigned j = 0; j < c->ix_arr[best].range; j++)
         keep[c->const_slot[k + j]] = true;
      kept += need;
   }
   free(done);
   for (; kept < room; kept++) {
      int best = -1;
      for (unsigned s = 0; s < c->n_sa; s++)
         if (!keep[s] && (best < 0 || uses[s] > uses[best]))
            best = s;
      keep[best] = true;
   }
   unsigned p = 0;
   for (unsigned s = 0; s < c->n_sa; s++)
      c->slot_sa[s] = keep[s] ? (int)p++ : -1;
   c->n_mem_bases = nb;
   for (unsigned j = 0; j < nb; j++)
      c->mem_base_sa[j] = p++;
   c->n_sa_phys = p;
   c->mem_dwords = c->n_sa + 1;
   free(uses);
   free(keep);
}

/* An indexed array in memory: its sa[idx1 + j] sequence (pck.s16.f32 t, x;
 * ima16 idx1, t, #1, #sa; mov d, sa[idx1 + j]) becomes
 *    mov x', x; limm l, #max; fminmax x', x', c48, l   (stay in the block)
 *    pck.s16.f32 t, x'
 *    imae.sgn a, t, #4, sa<base> [t2=u32]              (per base used)
 *    ld.a.d d, a, #((s + j) % 128) ...; wdf */
static bool
array_in_sa(const struct sgx_ctx *c, unsigned k)
{
   for (unsigned a = 0; a < c->n_ix_arr; a++) {
      if (c->ix_arr[a].k != k)
         continue;
      for (unsigned j = 0; j < c->ix_arr[a].range; j++)
         if (c->slot_sa[c->const_slot[k + j]] < 0)
            return false;
   }
   return c->slot_sa[c->const_slot[k]] >= 0;
}

static void
mem_index(struct sgx_ctx *c)
{
   if (!c->mem_dwords)
      return;
   list_for_each_entry_safe(struct sgx_ins, ix, &c->ins, link) {
      if (ix->kind != SGX_INS_ALU || ix->form != SGX_OP_IMA16 || ix->op[0].file != SGX_F_IDX ||
          ix->op[3].file != SGX_F_USLOT || array_in_sa(c, ix->op[3].n))
         continue;
      struct sgx_ins *pk = list_entry(ix->link.prev, struct sgx_ins, link);
      if (&pk->link == &c->ins || pk->form != SGX_OP_PCK_S16_F32 || pk->op[0].file != SGX_F_V ||
          ix->op[1].file != SGX_F_V || pk->op[0].n != ix->op[1].n) {
         sgx_fail(c, "indexed uniform: no pck before ima16");
         return;
      }
      unsigned s = c->const_slot[ix->op[3].n], comps = 0;
      for (struct sgx_ins *m = list_entry(ix->link.next, struct sgx_ins, link);
           &m->link != &c->ins && m->form == SGX_OP_MOV && m->op[1].file == SGX_F_IXSA;
           m = list_entry(m->link.next, struct sgx_ins, link))
         comps = MAX2(comps, m->op[1].n + (m->op[1].step ? m->rpt : 1));

      union { float f; uint32_t u; } max = { .f = (float)MAX2((int)(c->n_sa - s - comps), 0) };
      unsigned x0 = new_vreg(c, 1), lim = new_vreg(c, 1);
      struct sgx_ins *mv = ins_alloc(c, SGX_INS_ALU, SGX_OP_MOV);
      mv->nops = 2;
      mv->op[0] = vref(x0, 0);
      mv->op[1] = pk->op[1];
      insert_before(pk, mv);
      struct sgx_ins *li = ins_alloc(c, SGX_INS_ALU, SGX_OP_LIMM);
      li->nops = 2;
      li->op[0] = vref(lim, 0);
      li->op[1] = ref(SGX_F_IMM, max.u);
      insert_before(pk, li);
      struct sgx_ins *cl = ins_alloc(c, SGX_INS_ALU, SGX_OP_FMINMAX);
      cl->nops = 4;
      cl->op[0] = vref(x0, 0);
      cl->op[1] = vref(x0, 0);
      cl->op[2] = cref(48);
      cl->op[3] = vref(lim, 0);
      insert_before(pk, cl);
      pk->op[1] = vref(x0, 0);

      int addr[8];
      for (unsigned b = 0; b < ARRAY_SIZE(addr); b++)
         addr[b] = -1;
      struct sgx_ins *m = list_entry(ix->link.next, struct sgx_ins, link);
      while (&m->link != &c->ins && m->form == SGX_OP_MOV && m->op[1].file == SGX_F_IXSA) {
         struct sgx_ins *next = list_entry(m->link.next, struct sgx_ins, link);
         struct sgx_ref d = m->op[0];
         unsigned n = m->op[1].n, rpt = m->rpt, pred = m->pred;
         /* (a repeated mov: one load per iteration) */
         for (unsigned r = 0; r < rpt; r++) {
            unsigned w = s + n + (m->op[1].step ? r : 0), b = w / 128;
            if (addr[b] < 0) {
               addr[b] = new_vreg(c, 1);
               struct sgx_ins *ia = ins_alloc(c, SGX_INS_ALU, SGX_OP_IMAE);
               ia->nops = 4;
               ia->op[0] = vref(addr[b], 0);
               ia->op[1] = ix->op[1];
               ia->op[2] = ref(SGX_F_IMM, 4);
               ia->op[3] = ref(SGX_F_SA, c->sa_base + c->mem_base_sa[b]);
               fld(ia, SGX_FLD_SGN, 1);
               fld(ia, SGX_FLD_T2, 2);
               insert_before(ix, ia);
            }
            struct sgx_ins *ld = r ? ins_alloc(c, SGX_INS_ALU, SGX_OP_LD) : m;
            ld->form = SGX_OP_LD;
            ld->nops = 3;
            ld->nfld = 0;
            ld->rpt = 1;
            ld->pred = pred;
            ld->op[0] = d;
            if (d.step && r) {
               if (d.file == SGX_F_V)
                  ld->op[0].comp += r;
               else
                  ld->op[0].n += r;
            }
            ld->op[1] = vref(addr[b], 0);
            ld->op[2] = ref(SGX_F_IMM, w % 128);
            if (r)
               insert_before(next, ld);
         }
         m = next;
      }
      struct sgx_ins *wd = ins_alloc(c, SGX_INS_ALU, SGX_OP_WDF);
      wd->ndst = 0;
      if (&m->link == &c->ins)
         list_addtail(&wd->link, &c->ins);
      else
         insert_before(m, wd);
      list_del(&ix->link);
   }
}

/* Operands on slots not kept in SA: ld.a.d their words into a temporary
 * (drc0), wdf, and read the temporary with the same half / lane / format. */
static void
mem_uniforms(struct sgx_ctx *c)
{
   if (!c->mem_dwords)
      return;
   list_for_each_entry(struct sgx_ins, i, &c->ins, link) {
      bool loaded = false;
      for (unsigned k = i->ndst; k < i->nops; k++) {
         struct sgx_ref *r = &i->op[k];
         unsigned s, n;
         if (!operand_slots(c, i, r, &s, &n))
            continue;
         bool in_sa = true;
         for (unsigned j = 0; j < n; j++)
            in_sa &= s + j < c->n_sa && c->slot_sa[s + j] >= 0;
         if (in_sa)
            continue;
         unsigned half = r->h16 && r->file == SGX_F_U ? c->const_shift[r->n] / 16 : 0;
         unsigned t = new_vreg(c, n);
         for (unsigned j = 0; j < n; j++) {
            struct sgx_ins *ld = ins_alloc(c, SGX_INS_ALU, SGX_OP_LD);
            ld->nops = 3;
            ld->op[0] = vref(t, j);
            ld->op[1] = ref(SGX_F_SA, c->sa_base + c->mem_base_sa[(s + j) / 128]);
            ld->op[2] = ref(SGX_F_IMM, (s + j) % 128);
            insert_before(i, ld);
         }
         struct sgx_ref v = r->h16 ? href(t, half) : vref(t, 0);
         v.neg = r->neg;
         v.abs = r->abs;
         v.step = r->step;
         v.c10 = r->c10;
         v.lane = r->lane;
         *r = v;
         loaded = true;
      }
      if (loaded) {
         struct sgx_ins *w = ins_alloc(c, SGX_INS_ALU, SGX_OP_WDF);
         w->ndst = 0;
         insert_before(i, w);
      }
   }
}

/* A format-select F16 operand counts halves in 6 bits: it reaches sa0-sa31
 * only. With many uniforms an F16 constant's SA register can be past that
 * (dEQP uniform_api mat4[3]): copy its halves to a temporary first with
 * pck.f16.f16, whose sources are whole registers with a lane. Needs the
 * layout, which only depends on the constants the program uses. */
static void
far_f16_uniforms(struct sgx_ctx *c)
{
   uniform_layout(c);
   place_constants(c);
   list_for_each_entry(struct sgx_ins, i, &c->ins, link) {
      if (i->kind != SGX_INS_ALU || i->form == SGX_OP_FMAD16 ||
          sgx_form_is_scalar(i->form) || i->form == SGX_OP_PCK_U8_F16 ||
          i->form == SGX_OP_PCK_F16_F16 || i->form == SGX_OP_PCK_F32_F16)
         continue;
      bool fmt = false;
      for (unsigned k = 0; k < i->nops; k++)
         fmt |= i->op[k].h16 && i->op[k].file != SGX_F_K;
      for (unsigned k = i->ndst; k < i->nops && fmt; k++) {
         /* an f32 literal beside an F16 operand has a 6-bit number too */
         struct sgx_ref *r = &i->op[k];
         if (r->file != SGX_F_LIT || r->h16 || c->slot_sa[c->lit_slot[r->n]] < 0 || sgx_lit_sa(c, r->n) < 64)
            continue;
         unsigned n = r->step && i->rpt > 1 ? i->rpt : 1;
         unsigned t = new_vreg(c, n);
         struct sgx_ins *m = ins_alloc(c, SGX_INS_ALU, SGX_OP_MOV);
         m->nops = 2;
         m->rpt = n;
         m->op[0] = vref(t, 0);
         m->op[1] = *r;
         m->op[1].neg = m->op[1].abs = 0;
         insert_before(i, m);
         struct sgx_ref v = vref(t, 0);
         v.neg = r->neg;
         v.abs = r->abs;
         v.step = r->step;
         *r = v;
      }
      for (unsigned k = i->ndst; k < i->nops; k++) {
         struct sgx_ref *r = &i->op[k];
         if (r->file != SGX_F_U || !r->h16 || c->const_slot[r->n] < 0 ||
             c->slot_sa[c->const_slot[r->n]] < 0 || sgx_const_sa(c, r->n) < 32)
            continue;
         unsigned n = r->step && i->rpt > 1 ? i->rpt : 1;
         unsigned t = new_vreg16(c, ALIGN_POT(n, 2));
         for (unsigned j = 0; j < n; j += 2) {
            struct sgx_ins *pk = ins_alloc(c, SGX_INS_ALU, SGX_OP_PCK_F16_F16);
            pk->nops = 3;
            pk->op[0] = href(t, j);
            pk->op[1] = ref(SGX_F_U, r->n + j);
            pk->op[1].h16 = 1;
            pk->op[2] = ref(SGX_F_U, r->n + (j + 1 < n ? j + 1 : j));
            pk->op[2].h16 = 1;
            insert_before(i, pk);
         }
         struct sgx_ref v = href(t, 0);
         v.neg = r->neg;
         v.abs = r->abs;
         v.step = r->step;
         *r = v;
      }
   }
}

static struct sgx_isa_src
phys_opd(struct sgx_ctx *c, const struct sgx_ref *r)
{
   struct sgx_isa_src o = { 0 };
   o.neg = r->neg;
   o.abs = r->abs;
   o.lane = r->lane;
   if (r->h16 && r->file != SGX_F_K && r->file != SGX_F_C) {
      /* an F16 half: register + half */
      switch (r->file) {
      case SGX_F_V:
         o.num = sgx_vreg(c, r->n)->phys + r->comp / 2;
         o.half = r->comp & 1;
         o.file = sgx_vreg(c, r->n)->pa ? SGX_ISA_FILE_PA : SGX_ISA_FILE_R;
         break;
      case SGX_F_U:
         o.num = sgx_const_sa(c, r->n);
         o.half = c->const_shift[r->n] / 16;
         o.file = SGX_ISA_FILE_SA;
         break;
      case SGX_F_LIT:
         o.num = sgx_lit_sa(c, r->n);
         o.file = SGX_ISA_FILE_SA;
         break;
      default:
         o.num = r->n / 2;
         o.half = r->n & 1;
         o.file = r->file == SGX_F_PA ? SGX_ISA_FILE_PA : r->file == SGX_F_SA ? SGX_ISA_FILE_SA :
                  r->file == SGX_F_O ? SGX_ISA_FILE_O : SGX_ISA_FILE_R;
         break;
      }
      return o;
   }
   if (r->c10)
      o.fmt = SGX_ISA_FMT_C10;
   switch (r->file) {
   case SGX_F_V:
      o.file = sgx_vreg(c, r->n)->pa ? SGX_ISA_FILE_PA : sgx_vreg(c, r->n)->o ? SGX_ISA_FILE_O : SGX_ISA_FILE_R;
      o.num = sgx_vreg(c, r->n)->phys + r->comp;
      break;
   case SGX_F_R: o.file = SGX_ISA_FILE_R; o.num = r->n; break;
   case SGX_F_PA: o.file = SGX_ISA_FILE_PA; o.num = r->n; break;
   case SGX_F_SA: o.file = SGX_ISA_FILE_SA; o.num = r->n; break;
   case SGX_F_G: o.file = SGX_ISA_FILE_G; o.num = r->n; break;
   case SGX_F_U: o.file = SGX_ISA_FILE_SA; o.num = sgx_const_sa(c, r->n); break;
   case SGX_F_O: o.file = SGX_ISA_FILE_O; o.num = r->n; break;
   case SGX_F_C: o.file = SGX_ISA_FILE_C; o.num = r->n; break;
   case SGX_F_I: o.file = SGX_ISA_FILE_I; o.num = r->n; break;
   case SGX_F_IMM: o.file = SGX_ISA_FILE_IMM; o.num = r->n; break;
   case SGX_F_P: o.num = r->n; break;
   case SGX_F_USLOT: o.file = SGX_ISA_FILE_IMM; o.num = sgx_const_sa(c, r->n); break;
   case SGX_F_LIT: o.file = SGX_ISA_FILE_SA; o.num = sgx_lit_sa(c, r->n); break;
   case SGX_F_IXSA: o.file = SGX_ISA_FILE_IX; o.ixbank = 3; o.ixreg = 1; o.num = r->n; break;
   case SGX_F_IDX: o.file = SGX_ISA_FILE_IDX; o.num = r->n; break;
   default: break;
   }
   return o;
}

int
sgx_encode_ins(struct sgx_ctx *c, const struct sgx_ins *i, uint64_t *w)
{
   unsigned form = i->form;
   struct sgx_isa_instr x;
   bool scalar = sgx_form_is_scalar(form);
   bool pck16 = form == SGX_OP_PCK_U8_F16 || form == SGX_OP_PCK_F16_F16 || form == SGX_OP_PCK_F32_F16 ||
                form == SGX_OP_PCK_F16_U8 || form == SGX_OP_PCK_F16_C10;
   int f16_chan = -1;
   bool fmt = false;

   sgx_form_init(&x, form);
   for (unsigned k = 0; k < i->nops; k++) {
      struct sgx_isa_src o = phys_opd(c, &i->op[k]);
      if (o.file == SGX_ISA_FILE_I && !(sgx_form_slot(form, k)->files & SGX_FILE_BIT(SGX_ISA_FILE_I))) {
         /* sop3's src0 has no internal bank: i<n> is r124+n, six bits
          * under format control (ISA.md "Operand slots") */
         o.file = SGX_ISA_FILE_R;
         o.num += 60;
      }
      if (i->op[k].h16 && i->op[k].file != SGX_F_K && i->op[k].file != SGX_F_C) {
         if (form == SGX_OP_FMAD16) {
            o.half = 0;             /* lanes come from the swizzle fields */
         } else if (pck16) {
            if (k >= i->ndst)
               o.lane = o.half * 2; /* 16-bit sources: .l0 low, .l2 high half */
            o.half = 0;
         } else if (scalar && k >= i->ndst) {
            f16_chan = o.half;
            o.half = 0;
         } else if (k >= i->ndst) {
            o.fmt = SGX_ISA_FMT_F16; /* format select: the number counts halves */
            fmt = true;
         }
      }
      if (form == SGX_OP_FDP && k == 0 && i->rpt > 1) {
         assert(o.num >= i->rpt - 1u);
         o.num -= i->rpt - 1;
      }
      sgx_form_opd(&x, form, k, &o);
   }
   for (unsigned k = 0; k < i->nfld; k++)
      sgx_form_set(&x, form, i->fld[k].id, i->fld[k].v);
   unsigned cls = form_class(form);
   if (i->kind == SGX_INS_ALU && !i->noskip)
      x.skipinv = true;
   if (i->pred)
      x.pred = i->pred;
   if (cls == CLS_INC) {
      x.rpt = i->rpt;
      bool all = true;
      for (unsigned k = i->ndst; k < i->nops; k++)
         all &= i->op[k].step || i->rpt == 1;
      if (all) {
         x.moe = true;
      } else {
         for (unsigned k = i->ndst; k < i->nops; k++) {
            if (i->op[k].step)
               x.inc[sgx_form_slot(form, k)->role - SGX_ROLE_SRC0] = true;
         }
      }
   } else if (cls == CLS_ALL) {
      if (i->rpt > 1) {
         x.rcnt = true;
         x.rpt = i->rpt;
      } else {
         x.mask = 1;
      }
   } else if (cls == CLS_TESTMASK) {
      x.mask = (1 << i->rpt) - 1;
   } else if (form == SGX_OP_FMAD16) {
      x.rpt = 1;
   }
   if (f16_chan >= 0) {
      sgx_form_set(&x, form, SGX_FLD_F16, 1);
      x.chan = f16_chan;
   }
   if (fmt)
      x.fmt = true;
   *w = sgx545_isa_encode(&x, NULL);
   return 0;
}

/* ------------------------------------------------------------------------ */

int
sgx_compile(nir_shader *nir, const struct sgx_compile_options *opts, const struct sgx_varyings *vary,
            struct sgx_shader *out)
{
   struct sgx_ctx *c = rzalloc(NULL, struct sgx_ctx);
   int ret = -1;
   memset(out, 0, sizeof(*out));
   c->nir = nir;
   c->opts = opts;
   c->vary = vary;
   c->stage = nir->info.stage;
   c->keep_vreg = -1;
   c->face_vreg = -1;
   c->face_f32 = -1;
   c->p16 = opts->f16;
   c->n_labels = 6;     /* 3 start, 4 feedback, 5 phase 1: as libusc numbers them */
   c->sa_base = c->stage == MESA_SHADER_FRAGMENT ? 9 : 10;
   util_dynarray_init(&c->vregs, c);
   util_dynarray_init(&c->words, c);
   list_inithead(&c->ins);

   sgx_lower(c);
   if (opts->verbose)
      nir_print_shader(nir, stderr);

   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   c->vals = rzalloc_array(c, struct sgx_val, impl->ssa_alloc + 1);
   c->skip = _mesa_pointer_set_create(c);
   c->proj = _mesa_pointer_hash_table_create(c);
   if (c->stage == MESA_SHADER_FRAGMENT) {
      find_projected(c, impl);
      find_discard(c, impl);
      find_c10(c, impl);
   }
   c->shadow_regs = _mesa_pointer_set_create(c);
   find_shadow_regs(c, impl);
   emit_cf_list(c, &impl->body, NULL);
   if (!c->failed && c->stage == MESA_SHADER_FRAGMENT)
      emit_color(c);
   if (!c->failed)
      merge_runs(c);
   if (!c->failed)
      fold_movs(c);
   if (!c->failed)
      far_f16_uniforms(c);
   if (!c->failed)
      mem_index(c);
   if (!c->failed)
      mem_uniforms(c);
   if (!c->failed && c->stage == MESA_SHADER_FRAGMENT)
      mark_helpers(c);
   if (!c->failed)
      allocate(c);
   if (!c->failed)
      drop_self_moves(c);
   if (!c->failed) {
      uniform_layout(c);
      ret = sgx_finish_program(c, out);
   }
   out->stage = c->stage;
   out->log = c->log;
   c->log = NULL;
   if (c->failed)
      ret = -1;
   ralloc_free(c);
   return ret;
}

void
sgx_shader_free(struct sgx_shader *s)
{
   free(s->blocks);
   free(s->code);
   free(s->log);
   memset(s, 0, sizeof(*s));
}
