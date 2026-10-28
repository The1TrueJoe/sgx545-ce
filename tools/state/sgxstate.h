/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

/*
 * sgxstate.h: SGX545 TA/3D state words from GL-level parameters, for the
 * pieces docs/STATE.md decodes past the basic triangle: scissor and partial
 * viewports, lines and points, constant-colour blending, alpha test.
 *
 * Header-only, no allocation, no GL or gallium types: each helper fills words
 * the caller places in its own TA state blocks, vertex streams and USSE code
 * (mesa/src/gallium/winsys/sgx545 does that for draws and clears).  Every
 * value is tagged as docs/STATE.md tags it: [C] proven by a blob-free replay
 * on an EA3, [D] from DDK captures or code, [G] a guess.
 */

#ifndef SGXSTATE_H
#define SGXSTATE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* the TE tile, in pixels [D] */
#define SGXS_TILE_W 32
#define SGXS_TILE_H 16

/* TA state block header bits (STATE.md section 3) */
#define SGXS_HDR_ISPA 0x1
#define SGXS_HDR_ISPB 0x2
#define SGXS_HDR_ISPC 0x4
#define SGXS_HDR_PDS 0x40
#define SGXS_HDR_RGNCLIP 0x80
#define SGXS_HDR_VIEWPORT 0x100
#define SGXS_HDR_W0 0x200
#define SGXS_HDR_OUTSEL 0x400
#define SGXS_HDR_WCLAMP 0x800
#define SGXS_HDR_MTE 0x1000
#define SGXS_HDR_TSP 0xc000

/* A pixel rectangle of the render target, in target memory (y down, the
 * first row at the top): x0 <= x < x1, y0 <= y < y1. */
struct sgxs_rect {
   uint32_t x0, y0, x1, y1;
};

static inline bool
sgxs_rect_empty(const struct sgxs_rect *r)
{
   return r->x1 <= r->x0 || r->y1 <= r->y0;
}

/* A GL window-coordinate rectangle (y up, origin bottom left: glScissor,
 * glViewport) of a target h pixels high, as a target rectangle, clamped to
 * the w x h target. */
static inline struct sgxs_rect
sgxs_rect_from_gl(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t w, uint32_t h)
{
   int64_t x0 = x, x1 = (int64_t)x + width, y0 = (int64_t)h - ((int64_t)y + height), y1 = (int64_t)h - y;
   struct sgxs_rect r;

   r.x0 = x0 < 0 ? 0 : x0 > w ? w : (uint32_t)x0;
   r.x1 = x1 < 0 ? 0 : x1 > w ? w : (uint32_t)x1;
   r.y0 = y0 < 0 ? 0 : y0 > h ? h : (uint32_t)y0;
   r.y1 = y1 < 0 ? 0 : y1 > h ? h : (uint32_t)y1;
   return r;
}

/*
 * Region clip (TA block group 0x80): the tiles an object may land in.
 *   word 0 = 0x80000000 | first tile column << 16 | last tile column
 *   word 1 = first tile row << 16 | last tile row
 * The DDK writes the tiles a rectangle touches: (37,400)-(138,457) gives
 * columns 1..4, rows 25..28 [D: `scissor`, sc1, sc2 captures]; the whole
 * target is 0 .. xtiles-1, 0 .. ytiles-1.  The object is drawn only in these
 * tiles [C: sc1, a set object clipped to columns 3..6 sets pixels only below
 * x = 224].
 */
static inline void
sgxs_region_clip(uint32_t w[2], const struct sgxs_rect *r)
{
   w[0] = 0x80000000u | (r->x0 / SGXS_TILE_W) << 16 | ((r->x1 - 1) / SGXS_TILE_W);
   w[1] = (r->y0 / SGXS_TILE_H) << 16 | ((r->y1 - 1) / SGXS_TILE_H);
}

/*
 * Scissor, the DDK's way (STATE.md "Scissor and partial viewports").
 *
 * The ISP keeps one "visible" bit per pixel.  Pass-type-3 ("viewport
 * generate") objects write it and draw nothing else: ISPC b25-27 = 0 resets
 * the bit under the object, 1 sets it [C: sc1 replays].  Every later object
 * in the render is drawn only where the bit is set, whatever its own region
 * clip says.  So a scissor rectangle is two objects before the draws it
 * applies to:
 *
 *   1. reset: a triangle over the whole target, region clip = the target;
 *   2. set:   a 4-vertex strip (VDM prim 3) over the rectangle, region clip
 *             = the rectangle's tiles.
 *
 * The region clip is TA state that stays until the next object that has a
 * group 0x80 [C: sc1 q1, a draw without one is cut to the tiles of the
 * object before it].  So after the set object every draw is clipped to the
 * rectangle's tiles by the TA, and to its pixels by the visible bit.  The DDK
 * gives a draw its own group 0x80 (header 0xdfc3) when nothing before it in
 * the render set the region clip (sc4: no clear) [D].
 *
 * What the DDK does, per render (captures sc1-sc6, vp1, nc0) [D]:
 *  - scissor or viewport smaller than the target: reset over the whole
 *    target, set over the rectangle (a partial glViewport is clipped the
 *    same way, with the viewport's rectangle);
 *  - the rectangle changes between draws: reset over the old rectangle's
 *    tiles (region clip and geometry the old rectangle's), set the new one;
 *  - a zero-area scissor: the reset only;
 *  - a scissor that covers the target: nothing (sc3 = no scissor);
 *  - its glClear is a rectangle (its vertices) clipped the same way;
 *  - a render that doesn't start with a full-target clear starts with a
 *    load object: a full-target triangle that samples the colour buffer's
 *    previous contents (texture layout 6, T1 format 0xc) (nc0, sc4).  That
 *    is not about the scissor; with one, it is what keeps the pixels outside.
 *  - draws keep their own viewport transform.
 *
 * Both objects use the same TA state block (sgxs_vpgen_block), a vertex
 * stream of screen-space x, y pairs (sgxs_vpgen_verts) and the vertex program
 * sgxs_vpgen_vs: o0-o1 = the position, o2 = o3 = c3 (1.0).  No PDS group, so
 * no pixel program runs.
 */
#define SGXS_VPGEN_BLOCK_DWORDS 9

/* the TA state block of a reset (set = false) or set object; DMA it as the
 * DDK does: 9 dwords, kind 0 [D] */
static inline void
sgxs_vpgen_block(uint32_t blk[SGXS_VPGEN_BLOCK_DWORDS], bool set, const struct sgxs_rect *clip)
{
   blk[0] = SGXS_HDR_ISPA | SGXS_HDR_ISPC | SGXS_HDR_RGNCLIP | SGXS_HDR_OUTSEL | SGXS_HDR_MTE |
            SGXS_HDR_TSP;                        /* 0xd485 [D] */
   blk[1] = 0x07e00100;   /* ISPA: pass type 3, no colour write, depth always + write, ISPC follows [D] */
   blk[2] = set ? 0x02000000 : 0;                /* ISPC b25-27: 1 set, 0 reset [C] */
   sgxs_region_clip(blk + 3, clip);
   blk[5] = 4 << 16;      /* output select: 4 dwords per vertex [D] */
   blk[6] = 0x40;         /* MTE control, as the clear's [D] */
   blk[7] = 0;            /* TSP: no texcoords */
   blk[8] = 0;
}

/* The reset object's vertices: one triangle covering a w x h target (the
 * DDK's (0,0) (2w,0) (0,2h)), as x, y float pairs; 3 vertices, VDM prim 0. */
static inline void
sgxs_vpgen_reset_verts(float v[6], uint32_t w, uint32_t h)
{
   v[0] = 0, v[1] = 0;
   v[2] = 2.0f * w, v[3] = 0;
   v[4] = 0, v[5] = 2.0f * h;
}

/* The set object's vertices: the rectangle as a strip (VDM prim 3, 4
 * vertices: top left, top right, bottom left, bottom right) [D]; pixel
 * centres inside it get the bit [C]. */
static inline void
sgxs_vpgen_set_verts(float v[8], const struct sgxs_rect *r)
{
   v[0] = (float)r->x0, v[1] = (float)r->y0;
   v[2] = (float)r->x1, v[3] = (float)r->y0;
   v[4] = (float)r->x0, v[5] = (float)r->y1;
   v[6] = (float)r->x1, v[7] = (float)r->y1;
}

/* One viewport-generate object: its block, vertices and VDM primitive. */
struct sgxs_vpgen_obj {
   uint32_t blk[SGXS_VPGEN_BLOCK_DWORDS];
   float verts[8];                  /* x, y pairs, screen space */
   uint32_t prim;                   /* VDM primitive: 0 triangles, 3 strip */
   uint32_t count;                  /* vertices */
};

/*
 * The objects that make rect the visible region of a w x h target, the way
 * the DDK orders them: a reset over the whole target (prev == NULL: the
 * first rectangle of the render) or over the previous rectangle's tiles, then
 * a set over rect unless it is empty.  Returns how many (1 or 2).  Emit them
 * before the draws they clip; afterwards the region clip is rect's tiles
 * (or, after a lone reset, prev's), which the draws inherit.  To go back to
 * no clipping, make the whole target the rectangle.
 */
static inline unsigned
sgxs_scissor_objects(struct sgxs_vpgen_obj obj[2], const struct sgxs_rect *prev,
                     const struct sgxs_rect *rect, uint32_t w, uint32_t h)
{
   const struct sgxs_rect all = { 0, 0, w, h };
   unsigned n = 0;

   memset(obj, 0, 2 * sizeof(*obj));
   sgxs_vpgen_block(obj[n].blk, false, prev && !sgxs_rect_empty(prev) ? prev : &all);
   sgxs_vpgen_reset_verts(obj[n].verts, w, h);
   obj[n].prim = 0;
   obj[n++].count = 3;
   if (!sgxs_rect_empty(rect)) {
      sgxs_vpgen_block(obj[n].blk, true, rect);
      sgxs_vpgen_set_verts(obj[n].verts, rect);
      obj[n].prim = 3;
      obj[n++].count = 4;
   }
   return n;
}

/* The vertex program of both objects (the DDK's, libGLESv2), as (lo, hi)
 * USSE word pairs.  Word 0 is `phas.i #pc, #0 [wait=2]`: its lo word is the
 * epilogue's address, (code address + 8 * SGXS_VPGEN_VS_EPILOGUE - code heap
 * base) / 8, which the caller relocates.  2 dwords in (pa0-1), 4 out. */
#define SGXS_VPGEN_VS_WORDS 11
#define SGXS_VPGEN_VS_EPILOGUE 9
static const uint32_t sgxs_vpgen_vs[SGXS_VPGEN_VS_WORDS][2] = {
   { 0x00000000, 0xfa440200 },   /* phas.i #epilogue, #0 [wait=2] (relocate lo) */
   { 0x00000000, 0xf8000940 },   /* nop [nosched] */
   { 0xa00c0000, 0x48831280 },   /* test.and.ne p0, #0, #0 [skipinv nodst=0] */
   { 0xa0000000, 0xe9833084 },   /* (p0) ld.a.d pa0, pa0, #0 [skipinv rpt=4] */
   { 0x00000000, 0xf9200000 },   /* wdf */
   { 0xa0000000, 0x28a11001 },   /* mov o0, pa0 [skipinv rcnt rpt=2] */
   { 0x60400180, 0x28a30001 },   /* mov o2, c3 [skipinv rcnt] */
   { 0x60600180, 0x28a30001 },   /* mov o3, c3 [skipinv rcnt] */
   { 0xa0000000, 0xfb27d400 },   /* emit.t3 r0, #0, #0 [end f12 f10] */
   { 0x00000000, 0xfa440700 },   /* epilogue: phas.i #0, #0 [wait=7] */
   { 0xa0200000, 0xfb275400 },   /* emit.t1 r0, #0, #0 [end f12 f10 f3_20] */
};

/*
 * Primitives (STATE.md "Lines and points").
 *
 * The VDM index-list word: 0x80000000 | 0x00c00000 | prim << 26 | count,
 * and b28 for a triangle fan (prim 0).  The hardware primitives are
 * triangles, lines, points and triangle strips; there is no line strip or
 * loop.  The DDK's own GL_LINE_STRIP is a list over the strip's vertices plus
 * two of whatever follows them (count 2(n-1), identity indices: `lstrip`),
 * the stray lines to the screen centre seen under gl4es; its GL_LINE_LOOP is
 * a list over the right indices, 0 1 1 2 2 3 3 0 (`lloop`).  So strips and
 * loops are expanded to line lists by the driver, as the loop is.
 *
 * The ISP must be told the object type as well (ISPA b15-18): lines drawn as
 * VDM prim 1 with object type 0 draw nothing [C: l3, l4].
 */
enum sgxs_vdm_prim {
   SGXS_PRIM_TRIANGLES = 0,
   SGXS_PRIM_LINES = 1,
   SGXS_PRIM_POINTS = 2,
   SGXS_PRIM_TRIANGLE_STRIP = 3,
};
#define SGXS_VDM_FAN (1u << 28)

enum sgxs_objtype {
   SGXS_OBJ_TRIANGLE = 0,           /* [D] */
   SGXS_OBJ_LINE = 1,               /* [C] */
   SGXS_OBJ_POINT = 3,              /* gl_PointSize points [D] */
   SGXS_OBJ_SPRITE = 4,             /* points whose fragment shader reads gl_PointCoord [D] */
};

/* GL primitive enums (GL_POINTS .. GL_TRIANGLE_FAN) */
#define SGXS_GL_POINTS 0
#define SGXS_GL_LINES 1
#define SGXS_GL_LINE_LOOP 2
#define SGXS_GL_LINE_STRIP 3
#define SGXS_GL_TRIANGLES 4
#define SGXS_GL_TRIANGLE_STRIP 5
#define SGXS_GL_TRIANGLE_FAN 6

/* The hardware primitive for a GL mode: the VDM index-list bits (prim << 26,
 * fan b28) and the ISPA object type.  Returns false for line strips and
 * loops: expand them to GL_LINES first (sgxs_line_list_indices). */
static inline bool
sgxs_prim(unsigned gl_mode, bool sprite, uint32_t *vdm_bits, uint32_t *objtype)
{
   switch (gl_mode) {
   case SGXS_GL_POINTS:
      *vdm_bits = SGXS_PRIM_POINTS << 26;
      *objtype = sprite ? SGXS_OBJ_SPRITE : SGXS_OBJ_POINT;
      return true;
   case SGXS_GL_LINES:
      *vdm_bits = SGXS_PRIM_LINES << 26;
      *objtype = SGXS_OBJ_LINE;
      return true;
   case SGXS_GL_TRIANGLES:
      *vdm_bits = SGXS_PRIM_TRIANGLES << 26;
      *objtype = SGXS_OBJ_TRIANGLE;
      return true;
   case SGXS_GL_TRIANGLE_STRIP:
      *vdm_bits = SGXS_PRIM_TRIANGLE_STRIP << 26;
      *objtype = SGXS_OBJ_TRIANGLE;
      return true;
   case SGXS_GL_TRIANGLE_FAN:
      *vdm_bits = SGXS_PRIM_TRIANGLES << 26 | SGXS_VDM_FAN;
      *objtype = SGXS_OBJ_TRIANGLE;
      return true;
   default:
      return false;
   }
}

/* ISPA with the object type in b15-18 (the DDK replaces those bits per draw) */
static inline uint32_t
sgxs_ispa_objtype(uint32_t ispa, uint32_t objtype)
{
   return (ispa & ~0x78000u) | (objtype & 0xf) << 15;
}

/* A line strip (loop: also back to the first) of n vertices as a line list:
 * writes 2(n-1) indices (2n for a loop) to idx, returns the count. */
static inline unsigned
sgxs_line_list_indices(uint16_t *idx, unsigned n, bool loop, uint16_t first)
{
   unsigned k = 0;

   if (n < 2)
      return 0;
   for (unsigned i = 0; i + 1 < n; i++) {
      idx[k++] = (uint16_t)(first + i);
      idx[k++] = (uint16_t)(first + i + 1);
   }
   if (loop) {
      idx[k++] = (uint16_t)(first + n - 1);
      idx[k++] = first;
   }
   return k;
}

/*
 * Line width: ISPB b8-15 = width * 16 (1.0 -> 0x10, 4 -> 0x40, 9.5 -> 0x98)
 * [C: l1, l2 draw the DDK's own width-1 and width-9.5 frames].  glLineWidth
 * clamps to [1, 16] (libGLESv2 0x255b0).  Points and triangles keep 0x10.
 */
static inline uint32_t
sgxs_ispb_width(uint32_t ispb, float width)
{
   uint32_t w;

   if (!(width >= 1.0f))
      width = 1.0f;
   w = (uint32_t)(width * 16.0f + 0.5f);
   if (w > 0xff)
      w = 0xff;
   return (ispb & ~0xff00u) | w << 8;
}

/*
 * Points.  The vertex program writes the size as one more output dword after
 * the position and the varyings: an F16 pair, the size in the low half and
 * 0x5ffc (511.0) in the high half, as the DDK's compiled gl_PointSize = 8.0
 * writes it (`mov o6, sa10` with sa10 = 0x5ffc4800) [C: size p1].  Output
 * select gets 0x100.  Every point draw also has texcoord set 0 with two
 * components (TSP word 0 = 2, word 1 = 1) ahead of the size: the hardware's
 * point coordinate, which the vertex program doesn't write.  With
 * SGXS_OBJ_SPRITE the ISP generates gl_PointCoord into it: (i + 0.5) / size
 * across the point, origin top left, which the fragment program iterates as
 * tc0 (DOUTI "iterate tc0 x2 f16") [C: sprite sizes p2, p3].
 */
static inline uint32_t
sgxs_f16(float x)
{
   union { float f; uint32_t u; } v = { x };
   uint32_t s = v.u >> 16 & 0x8000, e = v.u >> 23 & 0xff, m = v.u & 0x7fffff;

   if (e == 0xff)
      return s | 0x7c00 | (m ? 0x200 : 0);
   if (e < 113)                     /* below the smallest normal F16: flush */
      return s;
   if (e > 142)
      return s | 0x7bff;            /* clamp to the largest finite */
   m += 0x1000;                     /* round to nearest */
   if (m & 0x800000) {
      m = 0;
      e++;
   }
   return s | (e - 112) << 10 | m >> 13;
}

/* the output dword a vertex program writes for gl_PointSize = size */
static inline uint32_t
sgxs_point_size_word(float size)
{
   return 0x5ffc0000u | sgxs_f16(size);
}

/* output select for a draw: vertex output dwords, 0x3000, and 0x100 when
 * the program writes the point size [D] */
static inline uint32_t
sgxs_outsel(uint32_t out_dwords, bool point_size)
{
   return out_dwords << 16 | 0x3000 | (point_size ? 0x100 : 0);
}

/*
 * Constant-colour blending (STATE.md "Constant-colour blending").
 *
 * sop2's selectors (zero, s1, s2, s1a, s2a, asat) can't name a constant, and
 * the DDK doesn't use k18/k19 for one either.  With any of CONSTANT_COLOR,
 * ONE_MINUS_CONSTANT_COLOR, CONSTANT_ALPHA, ONE_MINUS_CONSTANT_ALPHA among
 * the factors, libGLESv2's blend builder (FUN_000457d0) multiplies each side
 * out with sopwm into r0/r1 and adds them, five instructions after the
 * fragment colour (pa<c>), with o0 = the tile's colour (translucent pass):
 *
 *    sopwm r1, Y, o0      [c1=zero c2=Fd]          dst * Fd, all four bytes
 *    sopwm r1, Y, o0      [mask=8 ... Fd alpha]    dst.a * Fda, the alpha byte
 *    sopwm r0, pa<c>, X   [c1=Fs c2=zero]          src * Fs
 *    sopwm r0, pa<c>, X   [mask=8 ... Fs alpha]    src.a * Fsa
 *    sop2  o0, r0, r1     [end c1=one c2=one cop=add a1=one a2=one aop=add]
 *
 * X and Y are the colour, the tile or the constant, whichever the factor
 * reads: the blend colour (glBlendColor) is a fragment secondary attribute,
 * packed BGRA8 like the colour (the DDK's sa2 = 0x604080bf for 0.25 0.5 0.75
 * 0.375) [D: k1-k4 captures; C: k1 replays b2 b3].  Two temporaries (r0, r1).
 * Products round to nearest: 204 * 64 / 255 = 51.2 -> 51 [D].
 */
enum sgxs_bfactor {                 /* libGLESv2's nibble (ctx +0xfc): glBlendFunc order */
   SGXS_BF_ZERO, SGXS_BF_ONE, SGXS_BF_SRC_COLOR, SGXS_BF_ONE_MINUS_SRC_COLOR,
   SGXS_BF_SRC_ALPHA, SGXS_BF_ONE_MINUS_SRC_ALPHA, SGXS_BF_DST_ALPHA, SGXS_BF_ONE_MINUS_DST_ALPHA,
   SGXS_BF_DST_COLOR, SGXS_BF_ONE_MINUS_DST_COLOR, SGXS_BF_SRC_ALPHA_SATURATE,
   SGXS_BF_CONSTANT_COLOR, SGXS_BF_ONE_MINUS_CONSTANT_COLOR, SGXS_BF_CONSTANT_ALPHA,
   SGXS_BF_ONE_MINUS_CONSTANT_ALPHA,
};

static inline bool
sgxs_bfactor_is_constant(unsigned f)
{
   return f >= SGXS_BF_CONSTANT_COLOR && f <= SGXS_BF_ONE_MINUS_CONSTANT_ALPHA;
}

/* the packed BGRA8 blend colour, as the DDK writes it into its constant */
static inline uint32_t
sgxs_blend_color_word(const float rgba[4])
{
   uint32_t c[4];

   for (int i = 0; i < 4; i++) {
      float v = rgba[i] < 0 ? 0 : rgba[i] > 1 ? 1 : rgba[i];

      c[i] = (uint32_t)(v * 255.0f + 0.5f);
   }
   return c[3] << 24 | c[0] << 16 | c[1] << 8 | c[2];
}

/*
 * The five instructions, (lo, hi) pairs, for src/dst colour and alpha
 * factors (enum sgxs_bfactor), the colour in pa<color_pa> and the blend
 * colour in sa<const_sa>; bit-exact with the DDK's for color_pa 0, const_sa
 * 2 (k1-k4).  The final sop2's ops are cop/aop (sgxws_bop order: 0 add, 1
 * sub, 2 min, 3 max); the DDK always adds, so only add is proven.  With end
 * false the sop2 doesn't end the program (a colour mask follows).
 */
#define SGXS_BLEND_CONST_INSTS 5
static inline void
sgxs_blend_const(uint32_t w[SGXS_BLEND_CONST_INSTS][2], unsigned src_rgb, unsigned dst_rgb,
                 unsigned src_a, unsigned dst_a, unsigned color_pa, unsigned const_sa,
                 unsigned cop, unsigned aop, bool end)
{
   /* libGLESv2 DAT_0006d000/d080 (dst side: src1 bank+number, hi selectors)
    * and DAT_0006d0c0/d100 (src side: src2, hi selectors), by factor */
   static const uint32_t d_lo[15] = { 0x80000000, 0x80000000, 0x80000000, 0x80000000, 0x80000000,
                                      0x80000000, 0x40000000, 0x40000000, 0x40000000, 0x40000000,
                                      0x40000000, 0xc0000100, 0xc0000100, 0xc0000100, 0xc0000100 };
   static const uint32_t d_hi[15] = { 0, 0x8000, 0x10, 0x8010, 0x18, 0x8018, 0x38, 0x8038,
                                      0x30, 0x8030, 0x8, 0x10, 0x8010, 0x18, 0x8018 };
   static const uint32_t s_lo[15] = { 0x20000000, 0x20000000, 0x20000000, 0x20000000, 0x20000000,
                                      0x20000000, 0x10000000, 0x10000000, 0x10000000, 0x10000000,
                                      0x10000000, 0x30000002, 0x30000002, 0x30000002, 0x30000002 };
   static const uint32_t s_hi[15] = { 0, 0x1000000, 0x80, 0x1000080, 0xc0, 0x10000c0, 0x1c0,
                                      0x10001c0, 0x180, 0x1000180, 0x40, 0x180, 0x1000180, 0x1c0,
                                      0x10001c0 };
   const unsigned f[4] = { dst_rgb, dst_a, src_rgb, src_a };

   for (unsigned i = 0; i < 4; i++) {
      unsigned k = f[i] < 15 ? f[i] : 0;
      uint32_t lo, hi;

      if (i < 2) {                  /* r1 = dst * Fd: sopwm r1, <colour|tile|constant>, o0 */
         lo = d_lo[k];
         if (lo == 0x80000000)
            lo |= color_pa << 7;    /* the colour */
         else if (lo == 0xc0000100)
            lo = 0xc0000000 | const_sa << 7;
         lo |= 0x10200000;
         hi = d_hi[k] | (i ? 0x90800800 : 0x90807800);
      } else {                      /* r0 = src * Fs: sopwm r0, pa<c>, <colour|tile|constant> */
         lo = s_lo[k];
         if (lo == 0x20000000)
            lo |= color_pa;
         else if (lo == 0x30000002)
            lo = 0x30000000 | const_sa;
         lo |= color_pa << 7 | 0x80000000;
         hi = s_hi[k] | (i == 3 ? 0x90800800 : 0x90807800);
      }
      w[i][0] = lo;
      w[i][1] = hi;
   }
   /* sop2 o0, r0, r1 [end c1=one c2=one cop a1=one a2=one aop]; without
    * end it writes pa<c> instead (dst bank h1, number l21-27), as the Mesa
    * driver's blend does before a colour mask.  cop l18-19, aop l16-17. */
   w[4][0] = 0x00000001 | (cop & 3u) << 18 | (aop & 3u) << 16 | (end ? 0 : color_pa << 21);
   w[4][1] = 0x81808804 | (end ? 1u << 18 | 1 : 2);
}

#ifdef __cplusplus
}
#endif

#endif
