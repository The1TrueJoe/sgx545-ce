/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * The state-word packers against words the hardware documentation quotes:
 * each case packs its fields, compares the word, then unpacks the word and
 * packs it again.
 */

#include <stdio.h>

#include "sgx_csb.h"

static unsigned failures, cases;

#define CHECK(T, expect, name, ...)                                            \
   do {                                                                        \
      uint32_t _w[8] = {0}, _w2[8] = {0};                                      \
      struct SGX_##T _u;                                                    \
      sgx_pack(_w, T, name)                                                 \
      {                                                                        \
         __VA_ARGS__;                                                          \
      }                                                                        \
      SGX_##T##_unpack(_w, &_u);                                            \
      SGX_##T##_pack(_w2, &_u);                                             \
      cases++;                                                                 \
      if (_w[0] != (expect) || _w2[0] != _w[0]) {                              \
         fprintf(stderr,                                                       \
                 "%s:%d: " #T " %08x, expected %08x (repacked %08x)\n",        \
                 __FILE__, __LINE__, _w[0], (uint32_t)(expect), _w2[0]);       \
         failures++;                                                           \
      }                                                                        \
   } while (0)

static void
test_vdm(void)
{
   /* a state program and its info word, a TA block of 12 forwarded dwords */
   CHECK(VDMCTRL_PDS_STATE0, 0x43040e00, s,
         s.pds_addr = SGX_DEV_ADDR(0x3040e000));
   CHECK(VDMCTRL_PDS_STATE1, 0x0e022403, s, s.data_rows = 3;
         s.kind = SGX_VDMCTRL_STATE_KIND_TA_BLOCK; s.attr_size = 3);
   /* the empty secondary update, the TA terminate */
   CHECK(VDMCTRL_PDS_STATE0, 0x43040006, s,
         s.pds_addr = SGX_DEV_ADDR(0x30400060));
   CHECK(VDMCTRL_PDS_STATE1, 0x0400e100, s, s.data_rows = 1;
         s.kind = SGX_VDMCTRL_STATE_KIND_DMA);
   CHECK(VDMCTRL_PDS_STATE0, 0x63040000, s,
         s.block_type = SGX_VDMCTRL_BLOCK_TYPE_PDS_STATE_TERMINATE;
         s.pds_addr = SGX_DEV_ADDR(0x30400000));
   CHECK(VDMCTRL_PDS_STATE1, 0x0e022401, s, s.data_rows = 3;
         s.kind = SGX_VDMCTRL_STATE_KIND_TA_BLOCK; s.attr_size = 1);

   /* a draw: three triangle indices, a fan of four */
   CHECK(VDMCTRL_INDEX_LIST0, 0x80c00003, d,
         d.prim = SGX_VDMCTRL_PRIM_TRIANGLES;
         d.index_count = 3);
   CHECK(VDMCTRL_INDEX_LIST0, 0x90c00004, d, d.fan = true; d.index_count = 4);
   CHECK(VDMCTRL_INDEX_LIST1, 0x3e517004, d,
         d.index_addr = SGX_DEV_ADDR(0x3e517004));
   CHECK(VDMCTRL_INDEX_LIST2, 0xf0000000, d, d.batch = 15);
   CHECK(VDMCTRL_INDEX_LIST3, 0x003fffff, d, (void)d);
   CHECK(VDMCTRL_INDEX_LIST4, 0xf3040d82, d, d.vertices_per_task = 15;
         d.pds_addr = SGX_DEV_ADDR(0x3040d820));
   CHECK(VDMCTRL_INDEX_LIST5, 0x01c00082, d, d.data_rows = 2; d.in_size = 1;
         d.n_min3 = 3; d.out_size = 1);
   CHECK(VDMCTRL_STREAM_TERMINATE0, 0xc0000000, t, (void)t);
}

static void
test_ta(void)
{
   /* headers: the clear, a triangle, two-sided stencil, a scissor object */
   CHECK(TA_STATE_HEADER, 0xd4c3, h,
         h.ispa = h.ispb = h.pds = h.region_clip = h.outselect = h.mte =
            h.tsp0 = h.tsp1 = true);
   CHECK(TA_STATE_HEADER, 0xdf43, h,
         h.ispa = h.ispb = h.pds = h.viewport = h.unknown_word = h.outselect =
            h.wclamp = true;
         h.mte = h.tsp0 = h.tsp1 = true);
   CHECK(
      TA_STATE_HEADER, 0xdf7f, h,
      h.ispa = h.ispb = h.ispc = h.ispa2 = h.ispb2 = h.ispc2 = h.pds =
         h.viewport = true;
      h.unknown_word = h.outselect = h.wclamp = h.mte = h.tsp0 = h.tsp1 = true);
   CHECK(TA_STATE_HEADER, 0xd485, h,
         h.ispa = h.ispc = h.region_clip = h.outselect = h.mte = h.tsp0 =
            h.tsp1 = true);

   /* ISPA: a scissor object; a stencil reference with depth off */
   CHECK(TA_STATE_ISPA, 0x07e00100, a,
         a.pass_type = SGX_TA_PASSTYPE_VIEWPORT_GENERATE;
         a.no_color_write = true; a.depth_func = SGX_TA_CMP_ALWAYS;
         a.ispc_present = true);
   CHECK(TA_STATE_ISPA, 0x01d00355, a, a.stencil_ref = 0x55;
         a.ispc_present = true; a.ispb_present = true;
         a.depth_write_disable = true; a.depth_func = SGX_TA_CMP_ALWAYS);

   /* ISPB: line width 9.5 */
   CHECK(TA_STATE_ISPB, 0x000098f0, b, b.line_width = 0x98; b.color_mask = 0xf);

   /* ISPC: EQUAL with masks; ZERO/REPLACE/INCR; the stencil clear */
   CHECK(TA_STATE_ISPC, 0x0400f0ff, s, s.func = SGX_TA_CMP_EQUAL;
         s.compare_mask = 0xf0; s.write_mask = 0xff);
   CHECK(TA_STATE_ISPC, 0x0453f0ff, s, s.func = SGX_TA_CMP_EQUAL;
         s.compare_mask = 0xf0; s.write_mask = 0xff;
         s.sfail = SGX_TA_STENCILOP_ZERO;
         s.zfail = SGX_TA_STENCILOP_REPLACE;
         s.zpass = SGX_TA_STENCILOP_INCR);
   CHECK(TA_STATE_ISPC, 0x0e0200ff, s, s.func = SGX_TA_CMP_ALWAYS;
         s.zpass = SGX_TA_STENCILOP_REPLACE; s.write_mask = 0xff);

   /* PDS word 1: a uniform color, then 1-5 mediump varyings */
   CHECK(TA_STATE_PDS_SIZEINFO, 0x160f0001, p, p.primary_units = 1;
         p.secondary_size = 3; p.unknown_25_28 = 0xb);
   CHECK(TA_STATE_PDS_SIZEINFO, 0x18030001, p, p.primary_units = 1;
         p.unknown_25_28 = 12);
   CHECK(TA_STATE_PDS_SIZEINFO, 0x0c030002, p, p.primary_units = 2;
         p.unknown_25_28 = 6);
   CHECK(TA_STATE_PDS_SIZEINFO, 0x08030003, p, p.primary_units = 3;
         p.unknown_25_28 = 4);
   CHECK(TA_STATE_PDS_PROGRAM, 0x04001800, p, p.data_rows = 1;
         p.offset = 0x1800);

   /* region clip: glScissor(37, 23, 101, 57) on 720x480 */
   CHECK(TA_STATE_REGION_CLIP0, 0x80010004, r, r.x_min = 1; r.x_max = 4);
   CHECK(TA_STATE_REGION_CLIP1, 0x0019001c, r, r.y_min = 0x19; r.y_max = 0x1c);

   /* output select: the clear, a point, a point sprite with a varying */
   CHECK(TA_STATE_OUTSELECT, 0x00081800, o, o.out_dwords = 8;
         o.unknown_11_13 = SGX_TA_STATE_OUTSELECT_UNKNOWN_11_13_CLEAR);
   CHECK(TA_STATE_OUTSELECT, 0x00073100, o, o.out_dwords = 7;
         o.point_size = true;
         o.unknown_11_13 = SGX_TA_STATE_OUTSELECT_UNKNOWN_11_13_DRAW);
   CHECK(TA_STATE_OUTSELECT, 0x00093100, o, o.out_dwords = 9;
         o.point_size = true;
         o.unknown_11_13 = SGX_TA_STATE_OUTSELECT_UNKNOWN_11_13_DRAW);

   CHECK(TA_STATE_WCLAMP, 0x3727c5ac, w, w.value = 1e-5f);
   CHECK(TA_STATE_MTE, 0x01555621, m, m.cull = SGX_TA_CULL_CW;
         m.unknown_2_31 = SGX_TA_STATE_MTE_UNKNOWN_2_31_DRAW);
   CHECK(TA_STATE_MTE, 0x00000040, m,
         m.unknown_2_31 = SGX_TA_STATE_MTE_UNKNOWN_2_31_CLEAR);

   /* TSP: 4-component sets; a sprite's coordinate after a color */
   CHECK(TA_STATE_TSP0, 0x5b6d, t,
         t.set0 = t.set1 = t.set2 = t.set3 = t.set4 = 5);
   CHECK(TA_STATE_TSP0, 0x15, t, t.set0 = 5; t.set1 = 2);
   CHECK(TA_STATE_TSP1, 0x1f, t, t.f16 = 0x1f);
}

static void
test_pbe(void)
{
   /* a 720x480 window: pitch 736 */
   CHECK(PBE_STATE_WORD0, 0x00000000, w, (void)w);
   CHECK(PBE_STATE_WORD1, 0x01df02cf, w, w.x_max = 719; w.y_max = 479);
   CHECK(PBE_STATE_WORD2, 0x016f8000, w, w.pitch = 367);
   CHECK(PBE_STATE_WORD3, 0x3e265000, w,
         w.address = SGX_DEV_ADDR(0x3e265000));
   CHECK(PBE_SIDEBAND, 0x00000600, s, s.format = SGX_PBE_FORMAT_BGRA8888;
         s.dither = true);
   /* FBO targets: twiddled, dithered RGBA8888, RGB565, RGBA4444 */
   CHECK(PBE_SIDEBAND, 0x00002601, s, s.format = SGX_PBE_FORMAT_RGBA8888;
         s.dither = true; s.twiddled = true);
   CHECK(PBE_SIDEBAND, 0x00002603, s, s.format = SGX_PBE_FORMAT_RGB565;
         s.dither = true; s.twiddled = true);
   CHECK(PBE_SIDEBAND, 0x00002605, s, s.format = SGX_PBE_FORMAT_RGBA4444;
         s.dither = true; s.twiddled = true);
}

static void
test_tex(void)
{
   /* T0: nearest repeat, L8, linear, clamp, mirror, mipmaps */
   CHECK(TEXSTATE_WORD0, 0x07fe0000, t, t.no_mipmaps = true);
   CHECK(TEXSTATE_WORD0, 0x47fe0000, t, t.no_mipmaps = true;
         t.unknown_30 = true);
   CHECK(TEXSTATE_WORD0, 0x07fe1400, t, t.no_mipmaps = true;
         t.min_linear = t.mag_linear = true);
   CHECK(TEXSTATE_WORD0, 0x07fe0090, t, t.no_mipmaps = true;
         t.wrap_s = t.wrap_t = SGX_TEXSTATE_WRAP_CLAMP);
   CHECK(TEXSTATE_WORD0, 0x07fe0048, t, t.no_mipmaps = true;
         t.wrap_s = t.wrap_t = SGX_TEXSTATE_WRAP_MIRROR);
   CHECK(TEXSTATE_WORD0, 0x07fa0000, t, (void)t);
   CHECK(TEXSTATE_WORD0, 0x07fa1600, t,
         t.mip_linear = t.min_linear = t.mag_linear = true);
   /* a strided 736-pixel surface, and a 72-pixel 16-bit one (pitch in 32-bit
    * words) */
   CHECK(TEXSTATE_STRIDED_WORD0, 0x00b60e90, t, t.pitch_lo = 735 & 7;
         t.pitch_hi = 735 >> 3;
         t.wrap_s = t.wrap_t = SGX_TEXSTATE_WRAP_CLAMP);
   CHECK(TEXSTATE_STRIDED_WORD0, 0x00080690, t, t.pitch_lo = 35 & 7;
         t.pitch_hi = 35 >> 3;
         t.wrap_s = t.wrap_t = SGX_TEXSTATE_WRAP_CLAMP);

   /* T1: 8x8 RGBA8888, 12x6 linear, a 16x16 cube, a 720x480 window */
   CHECK(TEXSTATE_WORD1, 0x0d007007, t,
         t.format = SGX_TEXSTATE_FORMAT_RGBA8888;
         t.width = 7; t.height = 7);
   CHECK(
      TEXSTATE_WORD1, 0x6d00b005, t, t.layout = SGX_TEXSTATE_LAYOUT_LINEAR;
      t.format = SGX_TEXSTATE_FORMAT_RGBA8888; t.width = 11; t.height = 5);
   CHECK(TEXSTATE_WORD1, 0x4d00f00f, t, t.layout = SGX_TEXSTATE_LAYOUT_CUBE;
         t.format = SGX_TEXSTATE_FORMAT_RGBA8888; t.width = 15;
         t.height = 15);
   CHECK(TEXSTATE_WORD1, 0xcc2cf1df, t,
         t.layout = SGX_TEXSTATE_LAYOUT_STRIDED;
         t.format = SGX_TEXSTATE_FORMAT_BGRA8888; t.width = 719;
         t.height = 479);
   CHECK(TEXSTATE_WORD2, 0xf0001000, t,
         t.address = SGX_DEV_ADDR(0xf0001000));

   /* T3: tex1, tex2, the background object's load */
   CHECK(TEXSTATE_WORD3, 0x00800800, t, t.unknown_11 = true);
   CHECK(TEXSTATE_WORD3, 0x01028800, t, t.unknown_23 = false;
         t.unknown_11 = t.unknown_15 = true;
         t.unknown_17 = t.unknown_24 = true);
   CHECK(TEXSTATE_WORD3, 0x00800000, t, (void)t);
}

static void
test_viewport(void)
{
   /* the six floats in order, and their unpack */
   uint32_t w[6];
   struct SGX_TA_STATE_VIEWPORT v;

   sgx_pack(w, TA_STATE_VIEWPORT, vp)
   {
      vp.x_offset = 360.0f;
      vp.x_scale = 360.0f;
      vp.y_offset = 240.0f;
      vp.y_scale = -240.0f;
      vp.z_offset = 0.5f;
      vp.z_scale = 0.5f;
   }
   SGX_TA_STATE_VIEWPORT_unpack(w, &v);
   cases++;
   if (w[0] != 0x43b40000 || w[3] != 0xc3700000 || w[5] != 0x3f000000 ||
       v.y_scale != -240.0f) {
      fprintf(stderr, "viewport %08x %08x %08x\n", w[0], w[3], w[5]);
      failures++;
   }
}

int
main(void)
{
   test_vdm();
   test_ta();
   test_pbe();
   test_tex();
   test_viewport();
   printf("%u/%u state words\n", cases - failures, cases);
   return failures ? 1 : 0;
}
