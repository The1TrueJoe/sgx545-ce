/* Read "form fc nfld (id val)* nops (kind n ixbank idx neg abs f16 half fcsel)*" lines,
 * encode each with usse_encode(), print "hi lo" or "ERR msg". Driven by check_enc.py. */
#include <stdio.h>
#include <stdlib.h>
#include "usse.h"

int
main(void)
{
   unsigned form, fc, nf, no;
   while (scanf("%u %u %u", &form, &fc, &nf) == 3) {
      struct usse_inst in;
      usse_init(&in, form);
      for (unsigned i = 0; i < nf; i++) {
         int id, v;
         if (scanf("%d %d", &id, &v) != 2)
            return 1;
         usse_set(&in, id, v);
      }
      if (scanf("%u", &no) != 1)
         return 1;
      in.nops = no;
      for (unsigned i = 0; i < no; i++) {
         unsigned k, b, x, ng, ab, f16, h, fs, sw, nw, ln;
         int n;
         if (scanf("%u %d %u %u %u %u %u %u %u %u %u %u", &k, &n, &b, &x, &ng, &ab, &f16, &h, &fs, &sw, &nw, &ln) != 12)
            return 1;
         struct usse_opd *o = &in.op[i];
         o->kind = k; o->n = n; o->ixbank = b; o->idx = x; o->neg = ng; o->abs = ab;
         o->f16 = f16; o->half = h; o->fcsel = fs; o->swz = sw; o->nw = nw; o->lane = ln;
      }
      uint64_t w;
      const char *err;
      if (usse_encode(&in, fc, &w, &err))
         printf("ERR %s\n", err);
      else
         printf("%08x %08x\n", (unsigned)(w >> 32), (unsigned)w);
   }
   return 0;
}
