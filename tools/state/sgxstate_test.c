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

   /* lines and points (ln1, ln4, ln9, tri2, fan, pt8, pt16, spr16 captures) */
   {
      uint32_t vb, ot;

      sgxs_prim(SGXS_GL_LINES, false, &vb, &ot);
      check("ln1 VDM word / ISPA", (uint32_t[]){ 0x80c00000 | vb | 4, sgxs_ispa_objtype(0x01d00200, ot) },
            (uint32_t[]){ 0x84c00004, 0x01d08200 }, 2);
      sgxs_prim(SGXS_GL_TRIANGLE_FAN, false, &vb, &ot);
      check("fan VDM word", (uint32_t[]){ 0x80c00000 | vb | 4 }, (uint32_t[]){ 0x90c00004 }, 1);
      sgxs_prim(SGXS_GL_TRIANGLE_STRIP, false, &vb, &ot);
      check("tri2 VDM word", (uint32_t[]){ 0x80c00000 | vb | 4 }, (uint32_t[]){ 0x8cc00004 }, 1);
      sgxs_prim(SGXS_GL_POINTS, false, &vb, &ot);
      check("pt8 VDM word / ISPA", (uint32_t[]){ 0x80c00000 | vb | 3, sgxs_ispa_objtype(0x01d00200, ot) },
            (uint32_t[]){ 0x88c00003, 0x01d18200 }, 2);
      sgxs_prim(SGXS_GL_POINTS, true, &vb, &ot);
      check("spr16 ISPA", (uint32_t[]){ sgxs_ispa_objtype(0x01d00200, ot) }, (uint32_t[]){ 0x01d20200 }, 1);
      check("line strip refused", (uint32_t[]){ sgxs_prim(SGXS_GL_LINE_STRIP, false, &vb, &ot) },
            (uint32_t[]){ 0 }, 1);
      check("ISPB widths 1 4 9.5",
            (uint32_t[]){ sgxs_ispb_width(0xf0, 1), sgxs_ispb_width(0xf0, 4), sgxs_ispb_width(0xf0, 9.5f) },
            (uint32_t[]){ 0x10f0, 0x40f0, 0x98f0 }, 3);
      check("point size words 8 16 32",
            (uint32_t[]){ sgxs_point_size_word(8), sgxs_point_size_word(16), sgxs_point_size_word(32) },
            (uint32_t[]){ 0x5ffc4800, 0x5ffc4c00, 0x5ffc5000 }, 3);
      check("pt8 output select", (uint32_t[]){ sgxs_outsel(7, true) }, (uint32_t[]){ 0x00073100 }, 1);
      {
         uint16_t idx[8];
         unsigned n = sgxs_line_list_indices(idx, 4, true, 0);

         check("line loop of 4", (uint32_t[]){ n, idx[0], idx[1], idx[2], idx[5], idx[6], idx[7] },
               (uint32_t[]){ 8, 0, 1, 1, 3, 3, 0 }, 7);
      }
   }

   printf(fails ? "%d FAILED\n" : "all ok\n", fails);
   return fails != 0;
}
