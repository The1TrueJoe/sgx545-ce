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
ins_alloc(struct sgx_ctx *c, unsigned kind, enum usse_form_id form)
{
   struct sgx_ins *i = rzalloc(c, struct sgx_ins);
   i->kind = kind;
   i->form = form;
   i->rpt = 1;
   i->ndst = 1;
   return i;
}

static void
fld(struct sgx_ins *i, enum usse_field_id id, int32_t v)
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
   case USSE_FMAD: case USSE_FADM: case USSE_FMSA: case USSE_FSUBFLR:
   case USSE_FMINMAX: case USSE_FMAXMIN: case USSE_FDP: case USSE_FDSX: case USSE_FDSY:
      return CLS_INC;
   case USSE_FRCP: case USSE_FRSQ: case USSE_FLOG2: case USSE_FEXP2: case USSE_FSQRT:
   case USSE_FSIN: case USSE_FCOS: case USSE_MOV:
   case USSE_MOVC_Z_F32: case USSE_MOVC_Z_I32:
      return CLS_ALL;
   case USSE_TESTMASK_FADD_S0Z0O_BYTE_1:
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

/* can this slot of the form hold an operand of this file (kind)? */
static int
file_kind(unsigned file)
{
   switch (file) {
   case SGX_F_V: case SGX_F_R: return USSE_K_R;
   case SGX_F_PA: return USSE_K_PA;
   case SGX_F_SA: case SGX_F_U: return USSE_K_SA;
   case SGX_F_O: return USSE_K_O;
   case SGX_F_C: return USSE_K_C;
   case SGX_F_I: return USSE_K_I;
   case SGX_F_IMM: return USSE_K_IMM;
   case SGX_F_P: return USSE_K_P;
   default: return -1;
   }
}

static const struct usse_slot *
slot_of(unsigned form, unsigned k)
{
   return usse_form_slot(form, k);
}

static bool
slot_ok(unsigned form, unsigned k, const struct sgx_ref *r)
{
   const struct usse_slot *s = slot_of(form, k);
   int kind = file_kind(r->file);
   if (!s || !s->role || kind < 0 || s->enc[kind] < 0)
      return false;
   int be = s->enc[kind];
   if ((be >> 1) && !s->bank.n)
      return false;
   if ((be & 1) && !s->ext.n)
      return false;
   if (r->neg && !s->neg.n)
      return false;
   if (r->abs && !s->abs.n)
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
      m = ins_alloc(c, SGX_INS_ALU, USSE_LIMM);
      m->nops = 2;
      m->op[0] = vref(v, 0);
      m->op[1] = ref(SGX_F_IMM, src.neg ? src.n ^ 0x80000000u : src.n);
      if (src.abs)
         m->op[1].n &= 0x7fffffff;
      insert_before(at, m);
   } else if (src.h16) {
      /* F16 -> f32: x.f16 - floor(0) under format select (src1 takes any bank
       * and the modifiers) */
      m = ins_alloc(c, SGX_INS_ALU, USSE_FSUBFLR);
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
      m = ins_alloc(c, SGX_INS_ALU, USSE_FSUBFLR);
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
      m = ins_alloc(c, SGX_INS_ALU, USSE_MOV);
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
         const struct usse_slot *s = slot_of(i->form, k);
         int h = hw_const(r->n);
         bool negok = s && s->neg.n;
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
            struct sgx_ref k2 = ref(SGX_F_K, want);
            k2.step = 0;
            *r = materialize(c, i, k2, 1);
            r->step = 0;
         }
         if (!r->step && cls != CLS_INC && i->rpt > 1)
            sgx_fail(c, "constant can't step in %s", usse_form_name(i->form));
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
         const struct usse_slot *s = slot_of(i->form, k);
         bool scalar = i->form >= USSE_FRCP && i->form <= USSE_FCOS;
         bool pck16 = i->form == USSE_PCK_U8_F16 || i->form == USSE_PCK_F16_F16 || i->form == USSE_PCK_F32_F16;
         unsigned reg = r->file == SGX_F_V ? r->comp / 2 : r->n / 2;
         bool small = r->file == SGX_F_V || r->file == SGX_F_U || reg < 32;
         struct sgx_ref plain = *r;
         plain.h16 = 0;
         bool kind_ok = slot_ok(i->form, k, &plain);
         if (((s && s->fmt && small && i->form != USSE_TESTMASK_FADD_S0Z0O_BYTE_1) || scalar || pck16 ||
              i->form == USSE_FMAD16) && kind_ok)
            continue;
      }
      if (slot_ok(i->form, k, r))
         continue;
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
      r.h16 = c->const_fmt[v->n + comp] == 2;
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
   r.abs = ls.fabs;
   r.neg = ls.fneg;
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

static void
emit_runs(struct sgx_ctx *c, unsigned form, unsigned wmask, struct sgx_ref dst[4],
          unsigned nsrc, struct sgx_ref src[][4], const struct fieldset *fs, uint8_t pred)
{
   unsigned cls = form_class(form);
   unsigned mr = max_rpt(cls);
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
         if (!ref_next(&dst[j - 1], &dst[j]) || dst[j].file == SGX_F_K)
            break;
         bool ok = true;
         int nm[3];
         for (unsigned s = 0; s < nsrc; s++) {
            nm[s] = mode[s];
            bool st = ref_next(&src[s][j - 1], &src[s][j]);
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
   struct sgx_ins *i = ins_alloc(c, SGX_INS_ALU, USSE_FMAD16);
   i->nops = 4;
   i->op[0] = href(t, 0);
   if (x.file == SGX_F_K) {
      int h = hw_const(x.n);
      if (h < 0) {
         unsigned l = new_vreg(c, 1);
         struct sgx_ins *m = ins_alloc(c, SGX_INS_ALU, USSE_LIMM);
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
      emit_runs(c, x.neg || x.abs ? USSE_FSUBFLR : USSE_MOV, 1, dd, x.neg || x.abs ? 2 : 1, ss, &NOF, 0);
      x = vref(v, 0);
   }
   i->op[1] = x;
   i->op[2] = cref(45);
   i->op[3] = cref(48);
   fld(i, USSE_F_SWZ0, SWZ_F32);
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

/* a register operand for a fmad16 source reading lanes (a, b), and its swizzle */
static struct sgx_ref
pair_src(struct sgx_ctx *c, struct sgx_ref a, struct sgx_ref b, int *swz)
{
   if (a.file == SGX_F_K && a.h16 && b.file == SGX_F_K && b.h16) {
      uint32_t w = (a.n & 0xffff) | (b.n & 0xffff) << 16;
      *swz = SWZ_LOHI;
      if (w == 0)
         return cref(48);
      if (w == 0x3c003c00)
         return cref(45);
      unsigned l = new_vreg16(c, 2);
      struct sgx_ins *m = ins_alloc(c, SGX_INS_ALU, USSE_LIMM);
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
      else
         r.n &= ~1u;
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
   if (a.file == SGX_F_K || b.file == SGX_F_K) {
      int sw;
      return pair_src(c, a, b, &sw);
   }
   unsigned t = new_vreg16(c, 2);
   struct sgx_ins *i = ins_alloc(c, SGX_INS_ALU, USSE_PCK_F16_F16);
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
   bool neg0 = sp[0][0].neg;
   sp[0][0].neg = sp[0][1].neg = 0;
   for (unsigned j = 0; j < 3; j++)
      r[j] = pair_src(c, sp[j][0], sp[j][1], &swz[j]);
   if (!is_rpa(&r[0])) {
      r[0] = pair_reg(c, sp[0][0], sp[0][1]);
      swz[0] = SWZ_LOHI;
   }
   if (neg0)
      r[1].neg ^= 1;
   struct sgx_ins *i = ins_alloc(c, SGX_INS_ALU, USSE_FMAD16);
   i->nops = 4;
   i->op[0] = dpair;
   for (unsigned j = 0; j < 3; j++)
      i->op[1 + j] = r[j];
   fld(i, USSE_F_SWZ0, swz[0]);
   fld(i, USSE_F_SWZ1, swz[1]);
   fld(i, USSE_F_SWZ2, swz[2]);
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

/* a 16-bit op done in f32: the instruction selected as if the def were
 * 32-bit (its F16 sources are format-select operands), then each component
 * rounded to F16 */
static void
alu_as_f32(struct sgx_ctx *c, nir_alu_instr *alu)
{
   unsigned nc = alu->def.num_components;
   alu->def.bit_size = 32;
   struct sgx_val save = c->vals[alu->def.index];
   emit_alu(c, alu);
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

/* the value of a 16-bit def lives in a new 16-bit vreg; pairs of it */
static void
emit_alu16(struct sgx_ctx *c, nir_alu_instr *alu)
{
   unsigned nc = alu->def.num_components, nin = nir_op_infos[alu->op].num_inputs;
   struct sgx_ref s[3][4];
   for (unsigned j = 0; j < nin && j < 3; j++)
      for (unsigned k = 0; k < nc; k++)
         s[j][k] = alu_src(c, alu, j, k);
   if (alu->op == nir_op_fmin || alu->op == nir_op_fmax || alu->op == nir_op_fdot2 ||
       alu->op == nir_op_fdot3 || alu->op == nir_op_fdot4 || alu->op == nir_op_fsat || alu->op == nir_op_fcsel ||
       alu->op == nir_op_fsqrt || alu->op == nir_op_fsin_agx) {
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
      for (unsigned k = 0; k < nc; k++) {
         r[k] = alu->op == nir_op_mov || alu->op == nir_op_f2f16 || alu->op == nir_op_b2f16 ? s[0][k]
                                                                                            : alu_src(c, alu, k, 0);
         if (r[k].file == SGX_F_K && !r[k].h16) {
            /* a constant: its F16 bits (round to nearest) */
            r[k].n = _mesa_float_to_half(uif(r[k].n));
            r[k].h16 = 1;
         } else if (!r[k].h16) {
            r[k] = f32_to_h(c, r[k]);
         }
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
            struct sgx_ins *i = ins_alloc(c, SGX_INS_ALU, USSE_FSUBFLR);
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
            fmad16(c, href(t, 0), mm);
            lane[q] = href(t, 0);
            if (l1 == l0)
               break;
         }
         struct sgx_ins *i = ins_alloc(c, SGX_INS_ALU, USSE_PCK_F16_F16);
         i->nops = 3;
         i->op[0] = dp;
         i->op[1] = lane[0];
         i->op[2] = l1 == l0 ? lane[0] : lane[1];
         add(c, i);
         break;
      }
      case nir_op_frcp: case nir_op_frsq: case nir_op_fsqrt: case nir_op_fexp2: case nir_op_flog2: {
         /* F16 source and destination: the result lands in the source's half
          * of a temporary (the other half zeroed), then packed */
         unsigned form = alu->op == nir_op_frcp ? USSE_FRCP : alu->op == nir_op_frsq ? USSE_FRSQ :
                         alu->op == nir_op_fsqrt ? USSE_FSQRT : alu->op == nir_op_fexp2 ? USSE_FEXP2 : USSE_FLOG2;
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
            fld(i, USSE_F_F16D, 1);
            emit(c, i);
            lane[q] = href(t, hi);
         }
         struct sgx_ins *i = ins_alloc(c, SGX_INS_ALU, USSE_PCK_F16_F16);
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
   if (alu->op == nir_op_fsat && nir_legacy_fsat_folds(alu))
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
      set_refs(c, &alu->def, r, alu->def.num_components);
      return;
   }
   nir_legacy_alu_dest ld = nir_legacy_chase_alu_dest(&alu->def);
   unsigned vreg, base;
   unsigned nc = alu->def.num_components;
   unsigned wmask = ld.dest.is_ssa ? nir_component_mask(nc) : ld.write_mask;
   unsigned nin = nir_op_infos[alu->op].num_inputs;

   /* dot products: one fdp over all components, result in the last iteration's
    * destination: give it a vreg as wide as the inputs */
   if (alu->op == nir_op_fdot2 || alu->op == nir_op_fdot3 || alu->op == nir_op_fdot4) {
      unsigned n = alu->op == nir_op_fdot2 ? 2 : alu->op == nir_op_fdot3 ? 3 : 4;
      unsigned w = new_vreg(c, n);
      struct sgx_ref s[2][4], d[4];
      for (unsigned k = 0; k < n; k++) {
         d[k] = vref(w, k);
         s[0][k] = alu_src(c, alu, 0, k);
         s[1][k] = alu_src(c, alu, 1, k);
      }
      /* both sources must be one run: copy any that isn't */
      for (unsigned j = 0; j < 2; j++) {
         bool st = true, hd = true;
         for (unsigned k = 1; k < n; k++) {
            st &= ref_next(&s[j][k - 1], &s[j][k]);
            hd &= ref_same(&s[j][k - 1], &s[j][k]);
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
            emit_runs(c, USSE_MOV, nir_component_mask(n), td, 1, ts, &NOF, 0);
            for (unsigned k = 0; k < n; k++) {
               s[j][k] = vref(t, k);
               s[j][k].neg = ng;
               s[j][k].abs = ab;
            }
         }
      }
      emit_runs(c, USSE_FDP, nir_component_mask(n), d, 2, s, &NOF, 0);
      /* the result: component n-1 of w (clamped to [0, 1] when NIR folded an
       * fsat into it), moved where NIR wants it */
      struct sgx_ref res = vref(w, n - 1);
      if (ld.fsat) {
         unsigned t = new_vreg(c, 1);
         struct sgx_ref td[4] = { vref(t, 0) }, ts[3][4] = { { res }, { ref(SGX_F_K, 0) }, { ref(SGX_F_K, 0x3f800000) } };
         emit_runs(c, USSE_FMINMAX, 1, td, 3, ts, &NOF, 0);
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
               emit_runs(c, USSE_MOV, 1u << k, dd, 1, ss, &NOF, 0);
      }
      return;
   }

   def_dest(c, &alu->def, &ld.dest, &vreg, &base);
   struct sgx_ref d[4], s[3][4];
   for (unsigned k = 0; k < 4; k++) {
      d[k] = vref(vreg, base + k);
      for (unsigned j = 0; j < nin && j < 3; j++)
         s[j][k] = (wmask & (1u << k)) ? alu_src(c, alu, j, k) : ref(SGX_F_C, 48);
   }

   struct sgx_ref ops[3][4];
   struct fieldset fs = { 0 };
   unsigned form, n = 0;

#define SRC(j, x) do { for (unsigned k = 0; k < 4; k++) ops[j][k] = (x); } while (0)
   switch (alu->op) {
   case nir_op_mov:
      form = USSE_MOV;
      n = 1;
      SRC(0, s[0][k]);
      for (unsigned k = 0; k < 4; k++)
         if ((wmask & (1u << k)) && (s[0][k].neg || s[0][k].abs))
            form = USSE_FSUBFLR;
      if (form == USSE_FSUBFLR) {
         n = 2;
         SRC(1, ref(SGX_F_C, 48));
      }
      break;
   case nir_op_fneg:
   case nir_op_fabs:
      form = USSE_FSUBFLR;
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
      form = USSE_FMAD;
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
         form = USSE_FMINMAX;
         SRC(0, (*a)[k]);
         SRC(1, (*a)[k]);
         SRC(2, (*b)[k]);
      } else {
         form = USSE_FMAXMIN;
         SRC(0, (*a)[k]);
         SRC(1, (*b)[k]);
         SRC(2, (*a)[k]);
      }
      n = 3;
      break;
   }
   case nir_op_fsat:
      form = USSE_FMINMAX;
      SRC(0, s[0][k]);
      SRC(1, ref(SGX_F_K, 0));
      SRC(2, ref(SGX_F_K, 0x3f800000));
      n = 3;
      break;
   case nir_op_frcp: form = USSE_FRCP; goto scalar;
   case nir_op_frsq: form = USSE_FRSQ; goto scalar;
   case nir_op_fsqrt: form = USSE_FSQRT; goto scalar;
   case nir_op_fexp2: form = USSE_FEXP2; goto scalar;
   case nir_op_flog2: form = USSE_FLOG2; goto scalar;
   case nir_op_fsin_agx: form = USSE_FSIN; goto scalar;
   scalar:
      SRC(0, s[0][k]);
      n = 1;
      break;
   case nir_op_ffract:
      form = USSE_FSUBFLR;
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
      emit_runs(c, USSE_FSUBFLR, wmask, td, 2, ts, &NOF, 0);
      form = USSE_FMAD;
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
      emit_runs(c, USSE_FSUBFLR, wmask, fd, 2, ts, &NOF, 0);
      for (unsigned k = 0; k < 4; k++) {
         ts[0][k] = fd[k];
         ts[1][k] = ref(SGX_F_K, 0xbf800000);
         ts[2][k] = s[0][k];
         ts[2][k].abs = 1;
         ts[2][k].neg = 0;
      }
      emit_runs(c, USSE_FMAD, wmask, ad, 3, ts, &NOF, 0);
      for (unsigned k = 0; k < 4; k++) {
         ts[0][k] = ad[k];
         ts[1][k] = ref(SGX_F_K, 0xbf800000);
         ts[2][k] = ref(SGX_F_K, 0x80000000);
      }
      emit_runs(c, USSE_FMAD, wmask, nd, 3, ts, &NOF, 0);
      form = USSE_MOVC_Z_F32;
      fs.n = 1;
      fs.f[0].id = USSE_F_COND;
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
      struct fieldset tf = { 3, { { USSE_F_ALU, 14 }, { USSE_F_COND, cd }, { USSE_F_TYPE, 2 } } };
      emit_runs(c, USSE_TESTMASK_FADD_S0Z0O_BYTE_1, wmask, td, 2, ts, &tf, 0);
      form = USSE_MOVC_Z_I32;
      fs.n = 1;
      fs.f[0].id = USSE_F_COND;
      fs.f[0].v = 1;
      SRC(0, td[k]);
      SRC(1, ref(SGX_F_K, 0x3f800000));
      SRC(2, ref(SGX_F_K, 0));
      n = 3;
      break;
   }
   case nir_op_fcsel:
      form = USSE_MOVC_Z_F32;
      fs.n = 1;
      fs.f[0].id = USSE_F_COND;
      fs.f[0].v = 1;
      SRC(0, s[0][k]);
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
      emit_runs(c, USSE_FMINMAX, wmask, d, 3, ss, &NOF, 0);
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
   emit_runs(c, USSE_MOV, wmask, d, 1, s, &NOF, 0);
}

static struct sgx_ins *
emit_test(struct sgx_ctx *c, unsigned p, struct sgx_ref a, struct sgx_ref b, int alu, int cond)
{
   struct sgx_ins *t = ins_alloc(c, SGX_INS_ALU, USSE_TEST_FADD_S0Z0O);
   t->nops = 3;
   t->ndst = 0;
   t->op[0] = ref(SGX_F_P, p);
   t->op[1] = a;
   t->op[2] = b;
   t->writes_pred = p + 1;
   fld(t, USSE_F_ALU, alu);
   fld(t, USSE_F_COND, cond);
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
      struct sgx_ins *m = ins_alloc(c, SGX_INS_ALU, USSE_MOV);
      m->nops = 2;
      m->op[0] = vref(c->keep_vreg, 0);
      m->op[1] = cref(52);
      list_add(&m->link, &c->ins);   /* at the start of the program */
   }
   c->discards = true;
   struct sgx_ref d[4], s[3][4];
   struct fieldset fs = { 1, { { USSE_F_COND, 1 } } };
   d[0] = vref(c->keep_vreg, 0);
   if (cond) {
      nir_legacy_src ls = nir_legacy_chase_src(&in->src[0]);
      s[0][0] = src_comp(c, &ls, 0);
      s[1][0] = ref(SGX_F_K, 0);
      s[2][0] = vref(c->keep_vreg, 0);
      emit_runs(c, USSE_MOVC_Z_F32, 1, d, 3, s, &fs, 0);
   } else {
      s[0][0] = cref(48);
      emit_runs(c, USSE_MOV, 1, d, 1, s, &NOF, 0);
   }
}

static void
emit_intrinsic(struct sgx_ctx *c, nir_intrinsic_instr *in)
{
   struct sgx_val *v = nir_intrinsic_infos[in->intrinsic].has_dest ? &c->vals[in->def.index] : NULL;
   switch (in->intrinsic) {
   case nir_intrinsic_decl_reg: {
      unsigned n = nir_intrinsic_num_components(in) * MAX2(nir_intrinsic_num_array_elems(in), 1);
      v->kind = VAL_V;
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
         for (unsigned pr = 0; pr < 2; pr++) {
            unsigned m = wm >> (2 * pr) & 3;
            if (!m)
               continue;
            struct sgx_ins *pk = ins_alloc(c, SGX_INS_ALU, USSE_PCK_F16_F16);
            pk->nops = 3;
            pk->op[0] = href(rv->n, base + 2 * pr);
            struct sgx_ref lo = src_comp(c, &ls, 2 * pr), hi = src_comp(c, &ls, 2 * pr + 1);
            if (m & 1)
               lo = store_h(c, lo);
            if (m & 2)
               hi = store_h(c, hi);
            pk->op[1] = m == 2 ? hi : lo;
            pk->op[2] = m == 3 ? hi : ref(SGX_F_IMM, 0);
            pk->op[1].neg = pk->op[1].abs = pk->op[2].neg = pk->op[2].abs = 0;
            if (m != 3)
               fld(pk, USSE_F_DMASK, m == 1 ? 3 : 0xc);
            add(c, pk);
         }
         return;
      }
      if (p->type == nir_instr_type_alu || p->type == nir_instr_type_tex) {
         nir_legacy_dest ld = p->type == nir_instr_type_alu ? nir_legacy_chase_alu_dest(val).dest
                                                            : nir_legacy_chase_dest(val);
         if (!ld.is_ssa)
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
         nir_legacy_src lo = nir_legacy_chase_src(&in->src[0]);
         unsigned t = new_vreg(c, 1);
         struct sgx_ins *pk = ins_alloc(c, SGX_INS_ALU, USSE_PCK_S16_F32);
         pk->nops = 3;
         pk->op[0] = vref(t, 0);
         pk->op[1] = src_comp(c, &lo, 0);
         pk->op[2] = ref(SGX_F_IMM, 0);
         fld(pk, USSE_F_DMASK, 3);
         emit(c, pk);
         struct sgx_ins *ix = ins_alloc(c, SGX_INS_ALU, USSE_IMA16);
         ix->nops = 4;
         ix->op[0] = ref(SGX_F_IDX, 1);
         ix->op[1] = vref(t, 0);
         ix->op[2] = ref(SGX_F_IMM, 1);
         ix->op[3] = ref(SGX_F_USLOT, k);
         fld(ix, USSE_F_SGN, 1);
         fld(ix, USSE_F_F1, 1);
         fld(ix, USSE_F_F2, 1);
         add(c, ix);
         unsigned w = new_vreg(c, in->def.num_components);
         for (unsigned j = 0; j < in->def.num_components; j++) {
            struct sgx_ins *m = ins_alloc(c, SGX_INS_ALU, USSE_MOV);
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
            struct sgx_ins *i = ins_alloc(c, SGX_INS_ALU, USSE_PCK_F16_C10);
            i->nops = 3;
            i->op[0] = href(t, 2 * p);
            i->op[1] = ref(SGX_F_U, k + 2 * p);
            i->op[1].lane = 2 - 2 * p;
            if (2 * p + 1 < nc) {
               i->op[2] = ref(SGX_F_U, k + 2 * p + 1);
               i->op[2].lane = 1 - 2 * p;
            } else {
               i->op[2] = ref(SGX_F_IMM, 0);
               fld(i, USSE_F_DMASK, 3);
            }
            fld(i, USSE_F_SCALE, 1);
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
         /* gl_FrontFacing: p0 at program entry (DDK capture: libusc reads p0
          * before anything writes it); 1.0 front, 0.0 back */
         if (c->face_vreg < 0) {
            c->face_vreg = new_vreg(c, 1);
            struct sgx_ins *m1 = ins_alloc(c, SGX_INS_ALU, USSE_MOV);
            m1->nops = 2;
            m1->op[0] = vref(c->face_vreg, 0);
            m1->op[1] = cref(48);
            struct sgx_ins *m2 = ins_alloc(c, SGX_INS_ALU, USSE_MOV);
            m2->nops = 2;
            m2->op[0] = vref(c->face_vreg, 0);
            m2->op[1] = cref(52);
            m2->pred = 1;
            list_add(&m2->link, &c->ins);
            list_add(&m1->link, &c->ins);
         }
         v->kind = VAL_V;
         v->n = c->face_vreg;
         v->comp = 0;
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
      for (unsigned k = 0; k < 4; k++) {
         d[k] = ss[0][k] = ref(SGX_F_C, 48);
         int r = slot * 4 + k < ARRAY_SIZE(c->out_reg) ? c->out_reg[slot * 4 + k] : -1;
         if (!(wmask & (1u << k)) || r < 0)
            continue;
         d[k] = ref(SGX_F_O, r);
         ss[0][k] = src_comp(c, &ls, k - nir_intrinsic_component(in));
         m |= 1u << k;
      }
      emit_runs(c, USSE_MOV, m, d, 1, ss, &NOF, 0);
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
   struct sgx_ins *i = ins_alloc(c, SGX_INS_BR, USSE_BR);
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
   if (in->intrinsic != nir_intrinsic_load_interpolated_input || in->def.num_components != 4 ||
       nir_intrinsic_component(in) != 0)
      return NULL;
   return in;
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
      for (unsigned i = 0; i < c->n_iters; i++) {
         if (!c->iters[i].sample && c->iters[i].pa == pa && c->iters[i].comps == 4) {
            s->dependent = false;
            s->proj = true;
            s->set = c->iters[i].set;
            s->coord_pa = pa;
         }
      }
      if (s->dependent) {
         sgx_fail(c, "projected read of a varying that isn't iterated");
         return;
      }
   } else if (ls.is_ssa && bi < 0 && pi < 0 && c->vals[ls.ssa->index].kind == VAL_PA) {
      /* (a whole varying, or its first components) */
      unsigned pa = c->vals[ls.ssa->index].n;
      for (unsigned i = 0; i < c->n_iters; i++) {
         if (!c->iters[i].sample && c->iters[i].pa == pa && c->iters[i].comps >= ncoord) {
            s->dependent = false;
            s->set = c->iters[i].set;
            s->coord_pa = pa;
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
         struct sgx_ref qd[4] = { vref(q, 0) }, qs[1][4] = { { src_comp(c, &lp, 0) } };
         emit_runs(c, USSE_FRCP, 1, qd, 1, qs, &NOF, 0);
         struct sgx_ref td[4], ts[3][4];
         for (unsigned k = 0; k < 4; k++) {
            td[k] = vref(t, k);
            ts[0][k] = k < ncoord ? cr[k] : cref(48);
            ts[1][k] = vref(q, 0);
            ts[2][k] = ref(SGX_F_K, 0x80000000);
         }
         emit_runs(c, USSE_FMAD, nir_component_mask(ncoord), td, 3, ts, &NOF, 0);
         for (unsigned k = 0; k < ncoord; k++)
            cr[k] = vref(t, k);
      }
      bool run = !cr[0].neg && !cr[0].abs && cr[0].file != SGX_F_K && cr[0].file != SGX_F_C;
      for (unsigned k = 1; k < ncoord; k++)
         run &= ref_next(&cr[k - 1], &cr[k]);
      if (!run) {
         unsigned t = new_vreg(c, ncoord);
         struct sgx_ref td[4], ts[1][4];
         for (unsigned k = 0; k < 4; k++) {
            td[k] = vref(t, k);
            ts[0][k] = k < ncoord ? cr[k] : ref(SGX_F_C, 48);
         }
         emit_runs(c, USSE_MOV, nir_component_mask(ncoord), td, 1, ts, &NOF, 0);
         cr[0] = vref(t, 0);
      }
      unsigned rv = new_vreg(c, 1);
      raw = vref(rv, 0);
      i->ndst = 1;
      i->nops = 2;
      i->op[0] = raw;
      i->op[1] = cr[0];
      i->rpt = ncoord;          /* the coordinate's registers are read */
      if (bi >= 0) {
         nir_legacy_src lb = nir_legacy_chase_src(&tex->src[bi].src);
         s->lod = 1;
         i->op[2] = src_comp(c, &lb, 0);
         i->op[2].step = 0;
         i->nops = 3;
      }
      s->ins = i;
   }
   emit(c, i);
   if (s->dependent && i->nops == 3 && i->op[2].file == SGX_F_K && hw_const(i->op[2].n) >= 0)
      i->op[2] = cref(hw_const(i->op[2].n));
   if (s->dependent && i->nops == 3 && !slot_ok(USSE_SMP_1D_BIAS, 3, &i->op[2])) {
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
         struct sgx_ins *pk = ins_alloc(c, SGX_INS_ALU, USSE_PCK_F16_U8);
         pk->nops = 3;
         pk->op[0] = href(w, 2 * p);
         pk->op[1] = raw;
         pk->op[1].lane = 2 * p;
         pk->op[2] = raw;
         pk->op[2].lane = 2 * p + 1;
         fld(pk, USSE_F_SCALE, 1);
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
      struct sgx_ins *p = ins_alloc(c, SGX_INS_ALU, USSE_PCK_F32_U8);
      p->nops = 3;
      p->op[0] = vref(w, k);
      p->op[1] = raw;
      p->op[1].lane = k;
      p->op[2] = ref(SGX_F_IMM, 0);
      fld(p, USSE_F_SCALE, 1);
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
   case nir_op_frcp: case nir_op_flog2: case nir_op_fexp2: case nir_op_ffloor:
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
         if (alu->op != nir_op_fadd || negs)
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
      struct sgx_ins *p = ins_alloc(c, SGX_INS_ALU, USSE_PCK_C10_C10);
      p->nops = 3;
      p->op[0] = iref(v.i, false);
      p->op[1] = c10_uref(c, v.k);
      p->op[2] = c10_uref(c, v.k + 3);
      p->op[1].c10 = p->op[2].c10 = 0;
      fld(p, USSE_F_SCALE, 1);
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
         static const int f[] = { USSE_F_COP, 1, USSE_F_C1, 8, USSE_F_C2, 4, USSE_F_A, 4, USSE_F_AOP, 1 };
         c10_sop(c, USSE_SOP3, DST(), s, 3, f, 5);
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
      int f[] = { USSE_F_C1, 8, USSE_F_C2, 8, USSE_F_COP, op, USSE_F_AOP, op };
      c10_sop(c, USSE_SOPWM, DST(), s, 2, f, 4);
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
      static const int f[] = { USSE_F_COP, 0, USSE_F_C1, 4, USSE_F_C2, 0, USSE_F_A1, 0, USSE_F_A2, 1 };
      c10_sop(c, USSE_SOP3L, DST(), s, 3, f, 5);
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
      static const int f[] = { USSE_F_COP, 0, USSE_F_C1, 12, USSE_F_C2, 4, USSE_F_A1, 1, USSE_F_A2, 1 };
      c10_sop(c, USSE_SOP3L, DST(), s, 3, f, 5);
      break;
   }
   case nir_op_frcp:
   case nir_op_flog2:
   case nir_op_fexp2: {
      /* per channel into C10 (f16d): xyz to temporaries, w to an internal
       * register, then two pck.c10.c10 put them together */
      unsigned form = alu->op == nir_op_frcp ? USSE_FRCP : alu->op == nir_op_flog2 ? USSE_FLOG2 : USSE_FEXP2;
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
         fld(i, USSE_F_C10, 1);
         fld(i, USSE_F_CHAN, ch);
         fld(i, USSE_F_F16D, 1);
         add(c, i);
      }
      int r = c10_ialloc(c, st);
      struct sgx_ins *p = ins_alloc(c, SGX_INS_ALU, USSE_PCK_C10_C10);
      p->nops = 3;
      p->op[0] = iref(r, false);
      p->op[1] = vref(t[1], 0);
      p->op[1].lane = 1;
      p->op[2] = vref(t[2], 0);
      p->op[2].lane = 2;
      fld(p, USSE_F_DMASK, 6);
      add(c, p);
      p = ins_alloc(c, SGX_INS_ALU, USSE_PCK_C10_C10);
      p->nops = 3;
      p->op[0] = iref(r, false);
      p->op[1] = vref(t[0], 0);
      p->op[2] = iref(wi, false);
      p->op[2].lane = 3;
      fld(p, USSE_F_DMASK, 9);
      add(c, p);
      if (wi != va.i)
         st->busy &= ~(1u << wi);
      c10_release(st, va);
      if (to_i) {
         di = r;
      } else {
         s[0] = iref(r, true);
         s[1] = ref(SGX_F_IMM, 0);
         static const int f[] = { USSE_F_C1, 8, USSE_F_C2, 0, USSE_F_COP, 0, USSE_F_AOP, 0 };
         c10_sop(c, USSE_SOPWM, dst, s, 2, f, 4);
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
         struct sgx_ins *p = ins_alloc(c, SGX_INS_ALU, USSE_PCK_F16_C10);
         p->nops = 3;
         p->op[0] = href(h, 0);
         p->op[1] = iref(va.i, false);
         p->op[1].lane = lanes[q][0];
         p->op[2] = iref(va.i, false);
         p->op[2].lane = lanes[q][1];
         fld(p, USSE_F_SCALE, 1);
         add(c, p);
         for (unsigned hh = 0; hh < 2; hh++) {
            unsigned ch = lanes[q][hh];
            fr[ch] = new_vreg(c, 1);
            struct sgx_ins *i = ins_alloc(c, SGX_INS_ALU, USSE_FSUBFLR);
            i->nops = 3;
            i->op[0] = vref(fr[ch], 0);
            i->op[1] = href(h, hh);
            i->op[2] = href(h, hh);
            add(c, i);
         }
      }
      int f = c10_ialloc(c, st);
      for (unsigned q = 0; q < 2; q++) {
         struct sgx_ins *p = ins_alloc(c, SGX_INS_ALU, USSE_PCK_C10_F32);
         p->nops = 3;
         p->op[0] = iref(f, false);
         p->op[1] = vref(fr[q ? 0 : 1], 0);
         p->op[2] = vref(fr[q ? 3 : 2], 0);
         fld(p, USSE_F_DMASK, q ? 9 : 6);
         fld(p, USSE_F_SCALE, 1);
         add(c, p);
      }
      s[0] = c10_opd(c, va, false);
      s[1] = iref(f, true);
      c10_release(st, va);
      static const int fl[] = { USSE_F_C1, 8, USSE_F_C2, 8, USSE_F_COP, 1, USSE_F_AOP, 1 };
      if (to_i) {
         di = f;
         c10_sop(c, USSE_SOPWM, iref(f, true), s, 2, fl, 4);
      } else {
         c10_sop(c, USSE_SOPWM, dst, s, 2, fl, 4);
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
         fld(list_entry(l, struct sgx_ins, link), USSE_F_NOSCHED, 1);
      if (c->discards) {
         if (!c->keep_pred)
            emit_test(c, 1, vref(c->keep_vreg, 0), ref(SGX_F_K, 0), 0, 24);
         struct sgx_ins *f = ins_alloc(c, SGX_INS_FEEDBACK, USSE_NOP);
         f->ndst = 0;
         emit(c, f);
      }
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
      for (unsigned k = 0; k < 4; k++) {
         if (col[k].file == SGX_F_K) {
            col[k] = cref(col[k].n ? 45 : 48);
            col[k].h16 = 1;
         }
      }
      bool first = reads_pa0(&col[0]) || reads_pa0(&col[3]);
      bool second = reads_pa0(&col[2]) || reads_pa0(&col[1]);
      unsigned d = second && first ? MAX2(c->n_pa_inputs, 1) : 0;
      bool swap = second && !first;
      c->colour_pa = d;
      for (unsigned hh = 0; hh < 2; hh++) {
         bool ra = (hh == 0) != swap;
         struct sgx_ins *p = ins_alloc(c, SGX_INS_ALU, USSE_PCK_U8_F16);
         p->nops = 3;
         p->op[0] = ref(SGX_F_PA, d);
         p->op[1] = ra ? col[0] : col[2];
         p->op[2] = ra ? col[3] : col[1];
         for (unsigned j = 1; j < 3; j++)
            if (p->op[j].file == SGX_F_C)
               p->op[j].lane = 0;
         fld(p, USSE_F_DMASK, ra ? 12 : 3);
         fld(p, USSE_F_SCALE, 1);
         add(c, p);
      }
      if (c->discards && !c->keep_pred) {
         emit_test(c, 1, vref(c->keep_vreg, 0), ref(SGX_F_K, 0), 0, 24);
      }
      if (c->discards) {
         struct sgx_ins *f = ins_alloc(c, SGX_INS_FEEDBACK, USSE_NOP);
         f->ndst = 0;
         emit(c, f);
      }
      return;
   }
   /* pck.u8.f32 D, g, r [dmask=6] then D, b, a [dmask=9]: the second reads after
    * the first wrote D, so D must not be what it reads (ISA.md "Fragment shaders") */
   unsigned d = 0;
   for (unsigned k = 0; k < 4; k++) {
      if (col[k].h16 && col[k].file != SGX_F_K) {
         /* F16 -> f32 (format select) */
         unsigned v = new_vreg(c, 1);
         struct sgx_ref dd[4] = { vref(v, 0) }, ss[3][4] = { { col[k] }, { cref(52) }, { cref(48) } };
         ss[2][0].neg = 1;
         emit_runs(c, USSE_FMAD, 1, dd, 3, ss, &NOF, 0);
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
      struct sgx_ins *p = ins_alloc(c, SGX_INS_ALU, USSE_PCK_U8_F32);
      p->nops = 3;
      p->op[0] = ref(SGX_F_PA, d);
      p->op[1] = lohalf ? col[1] : col[2];
      p->op[2] = lohalf ? col[0] : col[3];
      fld(p, USSE_F_DMASK, lohalf ? 6 : 9);
      fld(p, USSE_F_SCALE, 1);
      emit(c, p);
   }
   if (c->discards) {
      if (!c->keep_pred)
         emit_test(c, 1, vref(c->keep_vreg, 0), ref(SGX_F_K, 0), 0, 24);
      struct sgx_ins *f = ins_alloc(c, SGX_INS_FEEDBACK, USSE_NOP);
      f->ndst = 0;
      emit(c, f);
   }
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
          a->ndst != 1 || a->form == USSE_FDP || !same_fields(a, b) || a->writes_pred || b->writes_pred)
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

static bool
allocate(struct sgx_ctx *c)
{
   unsigned nv = util_dynarray_num_elements(&c->vregs, struct sgx_vreg);
   int ip = 0;
   struct { int head, tail; } loops[64];
   unsigned nloops = 0;
   int label_ip[256];
   for (unsigned i = 0; i < 256; i++)
      label_ip[i] = -1;

   /* uses at 2*ip, writes at 2*ip+1, except that a repeated instruction's
    * destination is live while it reads (iterations interleave) */
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
      for (unsigned k = 0; k < i->ndst; k++)
         touch(c, &i->op[k], i->rpt > 1 || i->kind == SGX_INS_SAMPLE ? 2 * ip : 2 * ip + 1, 0);
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

   struct ra_regs *regs = ra_alloc_reg_set(c, NUM_TEMPS, true);
   struct ra_class *cls[17] = { 0 };
   for (unsigned s = 1; s <= 16; s++) {
      cls[s] = ra_alloc_contig_reg_class(regs, s);
      for (unsigned b = 0; b + s <= NUM_TEMPS; b++)
         ra_class_add_reg(cls[s], b);
   }
   ra_set_finalize(regs, NULL);
   struct ra_graph *g = ra_alloc_interference_graph(regs, nv);
   for (unsigned v = 0; v < nv; v++) {
      struct sgx_vreg *r = sgx_vreg(c, v);
      unsigned regs = r->half ? (r->size + 1) / 2 : r->size;
      ra_set_node_class(g, v, cls[MIN2(MAX2(regs, 1), 16)]);
   }
   for (unsigned a = 0; a < nv; a++) {
      struct sgx_vreg *x = sgx_vreg(c, a);
      if (x->first < 0)
         continue;
      for (unsigned b = a + 1; b < nv; b++) {
         struct sgx_vreg *y = sgx_vreg(c, b);
         if (y->first >= 0 && x->first <= y->last && y->first <= x->last)
            ra_add_node_interference(g, a, b);
      }
   }
   if (!ra_allocate(g)) {
      sgx_fail(c, "out of temporaries (no spilling yet)");
      return false;
   }
   c->n_temps = 0;
   for (unsigned v = 0; v < nv; v++) {
      struct sgx_vreg *r = sgx_vreg(c, v);
      if (r->first < 0)
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
      if (fmt == 2) {
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
}

static struct usse_opd
phys_opd(struct sgx_ctx *c, const struct sgx_ref *r)
{
   struct usse_opd o = { 0 };
   o.neg = r->neg;
   o.abs = r->abs;
   o.lane = r->lane;
   if (r->h16 && r->file != SGX_F_K && r->file != SGX_F_C) {
      /* an F16 half: register + half */
      unsigned reg, half;
      switch (r->file) {
      case SGX_F_V:
         reg = sgx_vreg(c, r->n)->phys + r->comp / 2;
         half = r->comp & 1;
         o.kind = USSE_K_R;
         break;
      case SGX_F_U:
         reg = c->sa_base + c->const_slot[r->n];
         half = c->const_shift[r->n] / 16;
         o.kind = USSE_K_SA;
         break;
      default:
         reg = r->n / 2;
         half = r->n & 1;
         o.kind = r->file == SGX_F_PA ? USSE_K_PA : r->file == SGX_F_SA ? USSE_K_SA :
                  r->file == SGX_F_O ? USSE_K_O : USSE_K_R;
         break;
      }
      o.n = reg;
      o.half = half;
      return o;
   }
   o.fcsel = r->c10;
   switch (r->file) {
   case SGX_F_V:
      o.kind = USSE_K_R;
      o.n = sgx_vreg(c, r->n)->phys + r->comp;
      break;
   case SGX_F_R: o.kind = USSE_K_R; o.n = r->n; break;
   case SGX_F_PA: o.kind = USSE_K_PA; o.n = r->n; break;
   case SGX_F_SA: o.kind = USSE_K_SA; o.n = r->n; break;
   case SGX_F_U: o.kind = USSE_K_SA; o.n = c->sa_base + c->const_slot[r->n]; break;
   case SGX_F_O: o.kind = USSE_K_O; o.n = r->n; break;
   case SGX_F_C: o.kind = USSE_K_C; o.n = r->n; break;
   case SGX_F_I: o.kind = USSE_K_I; o.n = r->n; break;
   case SGX_F_IMM: o.kind = USSE_K_IMM; o.n = (int32_t)r->n; break;
   case SGX_F_P: o.kind = USSE_K_P; o.n = r->n; break;
   case SGX_F_USLOT: o.kind = USSE_K_IMM; o.n = c->sa_base + c->const_slot[r->n]; break;
   case SGX_F_IXSA: o.kind = USSE_K_IX; o.ixbank = 3; o.idx = 1; o.n = r->n; break;
   case SGX_F_IDX: o.kind = USSE_K_IDX; o.n = r->n; break;
   default: o.kind = USSE_K_NONE; break;
   }
   return o;
}

int
sgx_encode_ins(struct sgx_ctx *c, const struct sgx_ins *i, uint64_t *w)
{
   struct usse_inst m;
   usse_init(&m, i->form);
   for (unsigned k = 0; k < i->nfld; k++)
      usse_set(&m, i->fld[k].id, i->fld[k].v);
   unsigned cls = form_class(i->form);
   if (i->kind == SGX_INS_ALU && i->form != USSE_LIMM)
      usse_set(&m, USSE_F_SKIPINV, 1);
   if (i->form == USSE_LIMM)
      usse_set(&m, USSE_F_SKIPINV, 1);
   if (i->pred)
      usse_set(&m, USSE_F_PRED, i->pred);
   if (cls == CLS_INC) {
      usse_set(&m, USSE_F_RPT, i->rpt);
      bool all = true;
      for (unsigned k = i->ndst; k < i->nops; k++)
         all &= i->op[k].step || i->rpt == 1;
      if (all) {
         usse_set(&m, USSE_F_MOE, 1);
      } else {
         for (unsigned k = i->ndst; k < i->nops; k++) {
            if (!i->op[k].step)
               continue;
            const char *role = slot_of(i->form, k)->role;
            usse_set(&m, !strcmp(role, "src0") ? USSE_F_INC0 : !strcmp(role, "src1") ? USSE_F_INC1 : USSE_F_INC2, 1);
         }
      }
   } else if (cls == CLS_ALL) {
      if (i->rpt > 1) {
         usse_set(&m, USSE_F_RCNT, 1);
         usse_set(&m, USSE_F_RPT, i->rpt);
      } else {
         usse_set(&m, USSE_F_MASK, 1);
      }
   } else if (cls == CLS_TESTMASK) {
      usse_set(&m, USSE_F_MASK, (1 << i->rpt) - 1);
   } else if (i->form == USSE_FMAD16) {
      usse_set(&m, USSE_F_RPT, 1);
   }
   m.nops = i->nops;
   bool scalar = i->form >= USSE_FRCP && i->form <= USSE_FCOS;
   bool pck16 = i->form == USSE_PCK_U8_F16 || i->form == USSE_PCK_F16_F16 || i->form == USSE_PCK_F32_F16 ||
                i->form == USSE_PCK_F16_U8 || i->form == USSE_PCK_F16_C10;
   for (unsigned k = 0; k < i->nops; k++) {
      m.op[k] = phys_opd(c, &i->op[k]);
      if (m.op[k].kind == USSE_K_I && slot_of(i->form, k)->enc[USSE_K_I] < 0) {
         /* sop3's src0 has no internal bank: i<n> is r124+n, six bits
          * under format control (ISA.md "Operand slots") */
         m.op[k].kind = USSE_K_R;
         m.op[k].n += 60;
      }
      if (!i->op[k].h16 || i->op[k].file == SGX_F_K || i->op[k].file == SGX_F_C)
         continue;
      if (i->form == USSE_FMAD16) {
         m.op[k].half = 0;           /* lanes come from the swizzle fields */
      } else if (pck16) {
         if (k >= i->ndst)
            m.op[k].lane = m.op[k].half * 2;   /* 16-bit sources: .l0 low, .l2 high half */
         m.op[k].half = 0;
      } else if (scalar && k >= i->ndst) {
         usse_set(&m, USSE_F_F16, 1);
         usse_set(&m, USSE_F_CHAN, m.op[k].half);
         m.op[k].half = 0;
      } else if (k >= i->ndst) {
         m.op[k].f16 = 1;            /* format select: the number counts halves */
         usse_set(&m, USSE_F_FMT, 1);
      }
   }
   const char *err;
   if (usse_encode(&m, USSE_FC_DEFAULT, w, &err)) {
      sgx_fail(c, "encode %s (form %u, %u operands, op1 file %u neg %u): %s", usse_form_name(i->form), i->form,
               i->nops, i->op[1].file, i->op[1].neg, err);
      return -1;
   }
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
   emit_cf_list(c, &impl->body, NULL);
   if (!c->failed && c->stage == MESA_SHADER_FRAGMENT)
      emit_color(c);
   if (!c->failed)
      merge_runs(c);
   if (!c->failed)
      allocate(c);
   if (!c->failed) {
      uniform_layout(c);
      ret = sgx_write_usp(c, out);
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
   free(s->usp);
   free(s->code);
   free(s->log);
   memset(s, 0, sizeof(*s));
}
