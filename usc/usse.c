/*
 * USSE encoder: hw.encode() (re/isa/hw.py) over the generated tables.
 */
#include <string.h>

#include "usse.h"
#define USSE_FORMS_TABLES
#include "usse_forms.h"

void
usse_init(struct usse_inst *inst, enum usse_form_id form)
{
   memset(inst, 0, sizeof(*inst));
   inst->form = form;
}

void
usse_set(struct usse_inst *inst, enum usse_field_id f, int32_t v)
{
   inst->set[f] = 1;
   inst->f[f] = v;
}

int32_t
usse_get(const struct usse_inst *inst, enum usse_field_id f)
{
   return inst->set[f] ? inst->f[f] : 0;
}

const char *
usse_form_name(enum usse_form_id form)
{
   return usse_forms[form].name;
}

const char *
usse_field_name(enum usse_field_id f)
{
   return usse_field_names[f];
}

static bool
put(uint64_t *v, unsigned n, const uint8_t *b, uint64_t x)
{
   if (n < 64 && (x >> n))
      return false;
   for (unsigned i = 0; i < n; i++)
      *v = (*v & ~(1ull << b[i])) | ((x >> i & 1) << b[i]);
   return true;
}

static bool
putb(uint64_t *v, const struct usse_bits *b, uint64_t x)
{
   return put(v, b->n, b->b, x);
}

/* banks with F16 numbers that count halves (hw.HALF_KINDS) */
static bool
half_kind(unsigned k)
{
   return k == USSE_K_R || k == USSE_K_O || k == USSE_K_PA || k == USSE_K_SA;
}

/* hw.FC_KINDS */
static bool
fc_kind(unsigned k)
{
   return half_kind(k) || k == USSE_K_I;
}

static int
enc_opd(uint64_t *v, const struct usse_slot *s, const struct usse_opd *o, bool fmt_on, unsigned fc,
        const char **err)
{
   unsigned width = s->num.n;
   bool fmt = s->fmt && fmt_on;
   int8_t be;
   unsigned n;

   if (!s->role) {
      *err = "operand slot not encodable from C";
      return -1;
   }
   if (o->kind > USSE_K_P || s->enc[o->kind] < 0) {
      *err = "operand kind not encodable in this slot";
      return -1;
   }
   switch (s->type) {
   case USSE_SLOT_IMM: {
      /* limm's 32-bit immediate is passed as its bit pattern */
      int64_t x = s->num.n == 32 ? (int64_t)(uint32_t)o->n : o->n;
      int64_t lo = s->isigned ? -(1ll << (s->num.n - 1)) : 0;
      int64_t hi = s->isigned ? (1ll << (s->num.n - 1)) - 1 : (1ll << s->num.n) - 1;
      if (o->neg || o->abs || x < lo || x > hi)
         goto range;
      putb(v, &s->num, (uint64_t)x & ((1ull << s->num.n) - 1));
      return 0;
   }
   case USSE_SLOT_MOEVAL:
      if (o->swz) {
         putb(v, &s->extra, 1);
         return putb(v, &s->num, o->n) ? 0 : (*err = "bad swizzle", -1);
      }
      putb(v, &s->extra, 0);
      if (o->n < -128 || o->n > 127)
         goto range;
      putb(v, &s->num, (uint32_t)o->n & 0xff);
      return 0;
   case USSE_SLOT_PRED:
      return putb(v, &s->num, o->n) ? 0 : (*err = "needs p0-p3", -1);
   case USSE_SLOT_PREDDST: {
      static const struct usse_bits h6 = {1, {38}};
      putb(v, &h6, o->nw ? 0 : 1);
      return putb(v, &s->num, o->n) ? 0 : (*err = "needs p0-p3", -1);
   }
   case USSE_SLOT_SAMEBANK:
      return putb(v, &s->num, o->n) ? 0 : (*err = "src0 number too big", -1);
   }
   if (s->extra.n && o->lane && !putb(v, &s->extra, o->lane))
      goto range;
   if (fmt)
      width = 6;
   be = s->enc[o->kind];
   if (s->bank.n) {
      if (!putb(v, &s->bank, be >> 1))
         goto range;
   } else if (be >> 1) {
      *err = "kind needs bank bits the slot lacks";
      return -1;
   }
   if (s->ext.n) {
      putb(v, &s->ext, be & 1);
   } else if (be & 1) {
      *err = "kind needs an extension bit the slot lacks";
      return -1;
   }
   if (o->kind == USSE_K_IX) {
      unsigned ob = width - 3;
      if (o->n >> ob)
         goto range;
      n = o->ixbank << (ob + 1) | (o->idx - 1) << ob | o->n;
   } else if (o->kind == USSE_K_G) {
      if (width != 7) {
         *err = "special registers need a 7-bit number";
         return -1;
      }
      n = o->n | 0x40;
   } else {
      n = o->n;
   }
   /* Opd.halved(); test's C10 operands (COpd fmtname) are whole registers */
   bool f16 = o->f16 || (s->fmtname && o->fcsel);
   bool halved;
   if (!half_kind(o->kind) || (s->fmtname && o->fcsel))
      halved = false;
   else if (s->fmt && fmt_on && o->f16)
      halved = s->half16;
   else
      halved = s->fc == 0 && (fc & 1) && o->f16;
   if (halved)
      n = 2 * n + o->half;
   if (fmt) {
      if (n >> 6)
         goto range;
      n |= (unsigned)f16 << 6;
      width = 7;
   }
   if (s->fc >= 0 && (fc >> s->fc & 1) && fc_kind(o->kind) && width == 7) {
      if (n >> 6)
         goto range;
      if (o->fcsel || (s->fc == 0 && o->f16))
         n |= 0x40;
   } else if (o->fcsel && !(s->fmtname && fmt)) {
      *err = "format-control operand with format control off";
      return -1;
   }
   if (!putb(v, &s->num, n))
      goto range;
   if (o->neg && !putb(v, &s->neg, 1))
      goto nomod;
   if (o->abs && !putb(v, &s->abs, 1))
      goto nomod;
   if ((o->neg && !s->neg.n) || (o->abs && !s->abs.n))
      goto nomod;
   return 0;
range:
   *err = "operand number out of range";
   return -1;
nomod:
   *err = "no such modifier in this slot";
   return -1;
}

int
usse_encode(const struct usse_inst *inst, unsigned fc, uint64_t *word, const char **err)
{
   const struct usse_form *f = &usse_forms[inst->form];
   uint64_t v = f->fval;
   bool fmt_on = usse_get(inst, USSE_F_FMT) != 0;

   for (unsigned i = 0; i < f->nflds; i++) {
      const struct usse_fld *fl = &f->flds[i];
      if (fl->dep >= 0 && ((usse_get(inst, fl->dep) == fl->depk) == fl->depop)) {
         if (inst->set[fl->id] && inst->f[fl->id] != fl->def + fl->bias) {
            *err = "field given in a mode that doesn't have it";
            return -1;
         }
         continue;
      }
      int64_t x = inst->set[fl->id] ? (int64_t)inst->f[fl->id] - fl->bias : fl->def;
      if (x < 0 || !put(&v, fl->n, fl->b, (uint64_t)x)) {
         *err = "field value does not fit";
         return -1;
      }
   }
   unsigned k = 0;
   for (unsigned i = 0; i < f->nops; i++) {
      const struct usse_slot *s = &f->ops[i];
      if (s->role && s->dep >= 0 && ((usse_get(inst, s->dep) == s->depk) == s->depop))
         continue;
      if (k >= inst->nops) {
         *err = "too few operands";
         return -1;
      }
      if (enc_opd(&v, s, &inst->op[k++], fmt_on, fc, err))
         return -1;
   }
   if (k != inst->nops) {
      *err = "too many operands";
      return -1;
   }
   *word = v;
   return 0;
}

const struct usse_form *
usse_form_desc(unsigned form)
{
   return &usse_forms[form];
}

/* the k-th operand slot of a form, NULL past the end */
const struct usse_slot *
usse_form_slot(unsigned form, unsigned k)
{
   return k < usse_forms[form].nops ? &usse_forms[form].ops[k] : NULL;
}
