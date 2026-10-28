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

#include "sgx_priv.h"

#define SGX_F_K 20   /* constant bits, resolved to c<n> or limm when legalised */

enum { VAL_NONE, VAL_V, VAL_PA, VAL_U, VAL_CONST };

struct sgx_val {
   uint8_t kind;
   uint8_t comp;
   uint32_t n;
   nir_load_const_instr *lc;
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
         continue;
      }
      if (slot_ok(i->form, k, r))
         continue;
      struct sgx_ref m = materialize(c, i, *r, i->rpt);
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
      return vref(v->n, comp + ls->reg.base_offset * 4);
   }
   struct sgx_val *v = &c->vals[ls->ssa->index];
   switch (v->kind) {
   case VAL_V:
      return vref(v->n, v->comp + comp);
   case VAL_PA:
      return ref(SGX_F_PA, v->n + comp);
   case VAL_U:
      c->const_used[v->n + comp] = 1;
      return ref(SGX_F_U, v->n + comp);
   case VAL_CONST:
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

static void
emit_alu(struct sgx_ctx *c, nir_alu_instr *alu)
{
   if ((alu->op == nir_op_fneg || alu->op == nir_op_fabs) && nir_legacy_float_mod_folds(alu))
      return;
   if (alu->op == nir_op_fsat && nir_legacy_fsat_folds(alu))
      return;

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
      /* the result: component n-1 of w, moved where NIR wants it */
      struct sgx_ref res = vref(w, n - 1);
      if (ld.dest.is_ssa) {
         struct sgx_val *v = &c->vals[alu->def.index];
         v->kind = VAL_V;
         v->n = w;
         v->comp = n - 1;
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
      if (ld.fsat)
         sgx_fail(c, "saturated dot product");
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
   case nir_intrinsic_decl_reg:
      v->kind = VAL_V;
      v->n = new_vreg(c, nir_intrinsic_num_components(in) * MAX2(nir_intrinsic_num_array_elems(in), 1));
      return;
   case nir_intrinsic_load_reg:
   case nir_intrinsic_store_reg: {
      if (in->intrinsic == nir_intrinsic_load_reg)
         return;     /* chased by the users */
      /* a store whose value no instruction wrote into the register: copy */
      nir_def *val = in->src[0].ssa;
      nir_instr *p = nir_def_instr(val);
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
         sgx_fail(c, "indirect uniform load");
         return;
      }
      v->kind = VAL_U;
      /* (nir_lower_int_to_float has turned the offset into a float) */
      uint32_t off = nir_src_as_uint(in->src[0]);
      if (off >= 0x10000)
         off = (uint32_t)uif(off);
      v->n = nir_intrinsic_base(in) + off;
      if (v->n + in->def.num_components > ARRAY_SIZE(c->const_used))
         sgx_fail(c, "uniform constant %u out of range", v->n);
      return;
   }
   case nir_intrinsic_load_input:
   case nir_intrinsic_load_interpolated_input: {
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
static void emit_cf_list(struct sgx_ctx *c, struct exec_list *list, struct loop_labels *loop);

static void
emit_block(struct sgx_ctx *c, nir_block *b, struct loop_labels *loop)
{
   nir_foreach_instr(instr, b) {
      if (c->failed)
         return;
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
   if (ls.is_ssa && bi < 0 && pi < 0 && c->vals[ls.ssa->index].kind == VAL_PA) {
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

static void
emit_color(struct sgx_ctx *c)
{
   struct sgx_ref col[4];
   for (unsigned k = 0; k < 4; k++)
      col[k] = c->has_color ? c->color[k] : ref(SGX_F_K, 0);
   /* pck.u8.f32 D, g, r [dmask=6] then D, b, a [dmask=9]: the second reads after
    * the first wrote D, so D must not be what it reads (ISA.md "Fragment shaders") */
   unsigned d = 0;
   bool first_reads0 = (col[1].file == SGX_F_PA && col[1].n == 0) || (col[0].file == SGX_F_PA && col[0].n == 0);
   bool second_reads0 = (col[2].file == SGX_F_PA && col[2].n == 0) || (col[3].file == SGX_F_PA && col[3].n == 0);
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
      emit_test(c, 1, vref(c->keep_vreg, 0), ref(SGX_F_K, 0), 0, 24);
      struct sgx_ins *f = ins_alloc(c, SGX_INS_FEEDBACK, USSE_NOP);
      f->ndst = 0;
      emit(c, f);
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
      ra_set_node_class(g, v, cls[MIN2(MAX2(r->size, 1), 16)]);
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
      c->n_temps = MAX2(c->n_temps, r->phys + r->size);
   }
   return true;
}

/* ------------------------------------------------------------------------
 * encoding */

static void
uniform_layout(struct sgx_ctx *c)
{
   c->n_sa = 0;
   for (unsigned k = 0; k < ARRAY_SIZE(c->const_used); k++)
      c->const_slot[k] = c->const_used[k] ? (int)c->n_sa++ : -1;
}

static struct usse_opd
phys_opd(struct sgx_ctx *c, const struct sgx_ref *r)
{
   struct usse_opd o = { 0 };
   o.neg = r->neg;
   o.abs = r->abs;
   o.lane = r->lane;
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
   }
   m.nops = i->nops;
   for (unsigned k = 0; k < i->nops; k++)
      m.op[k] = phys_opd(c, &i->op[k]);
   const char *err;
   if (usse_encode(&m, USSE_FC_DEFAULT, w, &err)) {
      sgx_fail(c, "encode %s: %s", usse_form_name(i->form), err);
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
   emit_cf_list(c, &impl->body, NULL);
   if (!c->failed && c->stage == MESA_SHADER_FRAGMENT)
      emit_color(c);
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
