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

#ifdef __cplusplus
}
#endif

#endif
