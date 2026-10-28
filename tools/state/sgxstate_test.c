/*
 * sgxstate_test: sgxstate.h against words of DDK captures (docs/STATE.md, "Captures").
 *   cc -Wall -O1 -o sgxstate_test sgxstate_test.c && ./sgxstate_test
 */
#include <stdio.h>
#include <string.h>

#include "sgxstate.h"

static int fails;

static void
check(const char *what, const uint32_t *got, const uint32_t *want, unsigned n)
{
   for (unsigned i = 0; i < n; i++)
      if (got[i] != want[i]) {
         printf("FAIL %s word %u: %08x, the DDK %08x\n", what, i, got[i], want[i]);
         fails++;
         return;
      }
   printf("ok   %s\n", what);
}

static void
checkf(const char *what, const float *got, const float *want, unsigned n)
{
   if (memcmp(got, want, n * sizeof(float))) {
      printf("FAIL %s\n", what);
      fails++;
   } else {
      printf("ok   %s\n", what);
   }
}

int
main(void)
{
   struct sgxs_vpgen_obj o[2];
   struct sgxs_rect r;

   /* sc1: glScissor(100, 50, 200, 100) on 720x480, first in the render */
   r = sgxs_rect_from_gl(100, 50, 200, 100, 720, 480);
   {
      static const uint32_t reset[] = { 0xd485, 0x07e00100, 0, 0x80000016, 0x0000001d, 0x00040000, 0x40, 0, 0 };
      static const uint32_t set[] = { 0xd485, 0x07e00100, 0x02000000, 0x80030009, 0x0014001a, 0x00040000, 0x40, 0, 0 };
      static const float rv[] = { 0, 0, 1440, 0, 0, 960 };
      static const float sv[] = { 100, 330, 300, 330, 100, 430, 300, 430 };

      check("sc1 count", (uint32_t[]){ sgxs_scissor_objects(o, NULL, &r, 720, 480) }, (uint32_t[]){ 2 }, 1);
      check("sc1 reset block", o[0].blk, reset, 9);
      check("sc1 set block", o[1].blk, set, 9);
      checkf("sc1 reset vertices", o[0].verts, rv, 6);
      checkf("sc1 set vertices", o[1].verts, sv, 8);
      check("sc1 prims", (uint32_t[]){ o[0].prim, o[0].count, o[1].prim, o[1].count },
            (uint32_t[]){ 0, 3, 3, 4 }, 4);
   }
   /* sc2: glScissor(37, 23, 101, 57): tiles 1..4, 25..28 */
   r = sgxs_rect_from_gl(37, 23, 101, 57, 720, 480);
   {
      static const float sv[] = { 37, 400, 138, 400, 37, 457, 138, 457 };
      uint32_t rc[2];

      sgxs_region_clip(rc, &r);
      check("sc2 region clip", rc, (uint32_t[]){ 0x80010004, 0x0019001c }, 2);
      sgxs_scissor_objects(o, NULL, &r, 720, 480);
      checkf("sc2 set vertices", o[1].verts, sv, 8);
   }
   /* sc5: then glScissor(400, 200, 150, 120): reset over the old tiles, set 12..17, 10..17 */
   {
      struct sgxs_rect a = sgxs_rect_from_gl(100, 50, 200, 100, 720, 480);
      struct sgxs_rect b = sgxs_rect_from_gl(400, 200, 150, 120, 720, 480);
      static const float sv[] = { 400, 160, 550, 160, 400, 280, 550, 280 };

      sgxs_scissor_objects(o, &a, &b, 720, 480);
      check("sc5 reset clip", o[0].blk + 3, (uint32_t[]){ 0x80030009, 0x0014001a }, 2);
      check("sc5 set clip", o[1].blk + 3, (uint32_t[]){ 0x800c0011, 0x000a0011 }, 2);
      checkf("sc5 set vertices", o[1].verts, sv, 8);
   }
   /* sc6: glScissor(0, 0, 0, 0): the reset only */
   r = sgxs_rect_from_gl(0, 0, 0, 0, 720, 480);
   check("sc6 count", (uint32_t[]){ sgxs_scissor_objects(o, NULL, &r, 720, 480) }, (uint32_t[]){ 1 }, 1);

   printf(fails ? "%d FAILED\n" : "all ok\n", fails);
   return fails != 0;
}
