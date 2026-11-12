/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * Textures in the SGX545's texture heap, as docs/TEX.md measured them (and
 * the textures session's tools/tex/sgxtex.c checks): power-of-two levels
 * twiddled (square tiles of min(w, h, 16), Morton order with y in the lowest
 * bit), anything else linear with the pitch rounded up to 8 texels; mip
 * level n+1 right after level n.
 */

#include <stdlib.h>
#include <string.h>

#include "util/format/u_format.h"
#include "util/u_math.h"

#include "sgx_core.h"
#include "sgx_csb.h"
#include "sgx_resource.h"
#include "sgx_screen.h"
#include "sgx_texture.h"

#define TILE 16u

unsigned
sgx_tex_format(enum pipe_format format)
{
   switch (format) {
   /* an sRGB format is its linear twin here: the fragment shader decodes
    * what it samples (lower_srgb, sgx_fs_for) */
   /* The 8888 formats are sampled so the texel arrives in the colour
    * output's byte order, B, G, R, A, as the DDK binds them: the compiler
    * unpacks those units zyxw (sgx_compile_options.tex_bgra, sgx_tex_bgra), and
    * a texel that is the colour as it is takes no instruction.  R, G, B, A
    * bytes in memory are 0x0d, B, G, R, A bytes 0x0c (docs/TEX.md §1). */
   case PIPE_FORMAT_R8G8B8A8_UNORM:
   case PIPE_FORMAT_R8G8B8X8_UNORM:
   case PIPE_FORMAT_R8G8B8A8_SRGB:
   case PIPE_FORMAT_R8G8B8X8_SRGB:
   case PIPE_FORMAT_R8_UNORM: /* stored as RGBA8 (sgx_tex_bpp) */
   case PIPE_FORMAT_R8G8_UNORM:
   /* integer bytes, NEAREST: the compiler unpacks them (zero- or
    * sign-extended) from the RGBA8 texel */
   case PIPE_FORMAT_R8G8B8A8_UINT:
   case PIPE_FORMAT_R8G8B8A8_SINT:
   case PIPE_FORMAT_R8_UINT:
   case PIPE_FORMAT_R8_SINT:
   case PIPE_FORMAT_R8G8_UINT:
   case PIPE_FORMAT_R8G8_SINT:
      return 0x0d;
   case PIPE_FORMAT_B8G8R8A8_UNORM:
   case PIPE_FORMAT_B8G8R8X8_UNORM:
   case PIPE_FORMAT_B8G8R8A8_SRGB:
   case PIPE_FORMAT_B8G8R8X8_SRGB:
      return 0x0c;
   case PIPE_FORMAT_B5G6R5_UNORM:
      return 0x05;
   case PIPE_FORMAT_A8_UNORM:
      return 0x19;
   case PIPE_FORMAT_L8_UNORM:
   case PIPE_FORMAT_L8_SRGB:
      return 0x1a;
   case PIPE_FORMAT_L8A8_UNORM:
   case PIPE_FORMAT_L8A8_SRGB:
      return 0x1b;
   /* depths as F32 (sgx_tex_depth) */
   case PIPE_FORMAT_Z16_UNORM:
   case PIPE_FORMAT_Z24X8_UNORM:
   case PIPE_FORMAT_Z24_UNORM_S8_UINT:
   case PIPE_FORMAT_Z32_FLOAT:
   /* 32-bit words returned unchanged (docs/GL3.md, H2): R32 one a texel;
    * RG32 and RGBA32 as an R32 texture two or four times as wide
    * (sgx_tex_raw32), the compiler fetching each word */
   case PIPE_FORMAT_R32_UINT:
   case PIPE_FORMAT_R32_SINT:
   case PIPE_FORMAT_R32_FLOAT:
   case PIPE_FORMAT_R32G32_UINT:
   case PIPE_FORMAT_R32G32_SINT:
   case PIPE_FORMAT_R32G32B32A32_UINT:
   case PIPE_FORMAT_R32G32B32A32_SINT:
      return 0x12;
   default:
      return 0;
   }
}

/* The 32-bit words of a texel of an R32, RG32 or RGBA32 format (1, 2, 4),
 * stored as that many R32 texels in a row; 0 for other formats */
unsigned
sgx_tex_raw32(enum pipe_format format)
{
   switch (format) {
   case PIPE_FORMAT_R32_UINT:
   case PIPE_FORMAT_R32_SINT:
   case PIPE_FORMAT_R32_FLOAT:
      return 1;
   case PIPE_FORMAT_R32G32_UINT:
   case PIPE_FORMAT_R32G32_SINT:
      return 2;
   case PIPE_FORMAT_R32G32B32A32_UINT:
   case PIPE_FORMAT_R32G32B32A32_SINT:
      return 4;
   default:
      return 0;
   }
}

/* stored as F32 words, sampled as T1 0x12 (sgx_tex_depth, sgx_tex_raw32) */
bool
sgx_tex_word32(enum pipe_format format)
{
   return sgx_tex_format(format) == SGX_TEXSTATE_FORMAT_F32;
}

/* a level's width in device texels: an RG32 or RGBA32 texel is two or four */
static unsigned
dev_width(enum pipe_format format, unsigned w)
{
   return w * MAX2(sgx_tex_raw32(format), 1);
}

/* sampled B, G, R, A ordered (sgx_tex_format): the compiler unpacks zyxw */
bool
sgx_tex_bgra(enum pipe_format format)
{
   unsigned f = sgx_tex_format(format);

   return f == 0x0c || f == 0x0d;
}

/* A depth texture is F32 on the device whatever its format, in rows of a
 * multiple of 32 texels read strided (T1 layout 6); the CPU copy keeps its
 * own format.  The TPU doesn't filter it. */
bool
sgx_tex_depth(enum pipe_format format)
{
   return util_format_is_depth_or_stencil(format) && sgx_tex_word32(format);
}

static unsigned
depth_pitch(unsigned w)
{
   return align(w, 32);
}

/* Bytes per texel on the device.  R8 and RG8 (and their integer twins) are
 * stored as RGBA8: R, G (or 0), 0, 255 (1 for an integer format), which
 * the PBE can render into (it writes no 8- or 16-bit format) and the TPU
 * samples as GL's (r, g, 0, 1); the CPU copy keeps their own format, and
 * encode_box/decode_box convert. */
unsigned
sgx_tex_bpp(enum pipe_format format)
{
   if ((sgx_tex_format(format) == 0x0d &&
        util_format_get_blocksize(format) < 4) ||
       sgx_tex_word32(format))
      return 4;
   return util_format_get_blocksize(format);
}

static bool
twiddled(unsigned w, unsigned h)
{
   return util_is_power_of_two_nonzero(w) && util_is_power_of_two_nonzero(h);
}

static size_t
level_bytes(unsigned w, unsigned h, unsigned bpp)
{
   return (size_t)(twiddled(w, h) ? w : align(w, 8)) * h * bpp;
}

/* a level within its mip chain */
static size_t
chain_offset(const struct pipe_resource *prsc, unsigned level)
{
   unsigned bpp = sgx_tex_bpp(prsc->format);
   bool word32 = sgx_tex_word32(prsc->format);
   size_t off = 0;

   for (unsigned l = 0; l < level; l++) {
      unsigned w = u_minify(prsc->width0, l), h = u_minify(prsc->height0, l);

      off += word32 ? (size_t)depth_pitch(dev_width(prsc->format, w)) * h * 4
                    : level_bytes(w, h, bpp);
   }
   return off;
}

/* Cube faces follow one another, each a complete chain of the levels the
 * sampler sees: packed when level 0 is smaller than one 16x16 tile; a chain
 * of mip levels on 0x800 bytes whatever the format (docs/TEX.md: L8
 * 16/32/64 0x800/0x800/0x1800 a face, LA88 and RGBA4444
 * 0x800/0x1000/0x3000, RGBA8888 0x800/0x1800/0x5800); level 0 alone on a
 * tile's bytes (RGBA8888 16x16: 0x400; the other formats unmeasured, [G]) */
static size_t
face_stride_levels(const struct pipe_resource *prsc, unsigned levels)
{
   size_t chain = chain_offset(prsc, levels);

   if (prsc->width0 < TILE)
      return chain;
   return align64(chain,
                  levels > 1 ? 0x800 : TILE * TILE * sgx_tex_bpp(prsc->format));
}

static size_t
face_stride(const struct pipe_resource *prsc)
{
   return face_stride_levels(prsc, prsc->last_level + 1);
}

size_t
sgx_tex_cube0_size(const struct pipe_resource *prsc)
{
   return prsc->array_size * face_stride_levels(prsc, 1);
}

size_t
sgx_tex_offset(const struct pipe_resource *prsc, unsigned level, unsigned layer)
{
   return layer * face_stride(prsc) + chain_offset(prsc, level);
}

/* The last level of a texture's chain on the device.  The TPU takes the
 * level count from the size, so a mipmapped 2D texture with fewer levels
 * than its size has (GL_TEXTURE_MAX_LEVEL below the last: an atlas with a
 * few mip levels) gets the rest, filled from a level (fill_tail); otherwise
 * far-away draws read past the chain. */
unsigned
sgx_tex_dev_last(const struct pipe_resource *prsc)
{
   if (prsc->target != PIPE_TEXTURE_2D || prsc->array_size > 1 ||
       !prsc->last_level || !twiddled(prsc->width0, prsc->height0) ||
       sgx_tex_word32(prsc->format))
      return prsc->last_level;
   return util_logbase2(MAX2(prsc->width0, prsc->height0));
}

size_t
sgx_tex_size(const struct pipe_resource *prsc)
{
   return prsc->array_size > 1 ? prsc->array_size * face_stride(prsc)
                               : chain_offset(prsc, sgx_tex_dev_last(prsc) + 1);
}

/* an R8 or RG8 texel (cbpp bytes) as the device stores it; alpha 0xff, or
 * 1 for an integer format */
static inline void
widen_texel(uint8_t *o, const uint8_t *s, unsigned cbpp, uint8_t alpha)
{
   o[0] = s[0];
   o[1] = cbpp > 1 ? s[1] : 0;
   o[2] = 0;
   o[3] = alpha;
}

/* Copy the box x0..x1, y0..y1 of a level from the CPU copy into the texture
 * heap.  The heap is write-combined: a twiddled tile the box covers whole is
 * put together on the stack and written in one go, only partly covered tiles
 * at the box's edges are written texel by texel. */
static void
encode_rect(struct sgx_resource *res, unsigned level, const uint8_t *src,
            uint8_t *dst, unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
   struct pipe_resource *prsc = &res->base;
   unsigned w = u_minify(prsc->width0, level),
            h = u_minify(prsc->height0, level);
   unsigned bpp = sgx_tex_bpp(prsc->format),
            cbpp = util_format_get_blocksize(prsc->format);
   unsigned src_stride = util_format_get_stride(prsc->format, w);
   bool rgbx = prsc->format == PIPE_FORMAT_R8G8B8X8_UNORM;
   bool widen = cbpp != bpp; /* R8, RG8: the texel made RGBA8 */
   uint8_t alpha = util_format_is_pure_integer(prsc->format) ? 1 : 0xff;

   x1 = MIN2(x1, w);
   y1 = MIN2(y1, h);
   if (x0 >= x1 || y0 >= y1)
      return;
   if (sgx_tex_depth(prsc->format)) {
      size_t pitch = (size_t)depth_pitch(w) * 4;

      for (unsigned y = y0; y < y1; y++)
         util_format_unpack_z_float(
            prsc->format, (float *)(dst + y * pitch) + x0,
            src + y * src_stride + (size_t)x0 * cbpp, x1 - x0);
      return;
   }
   if (sgx_tex_raw32(prsc->format)) {
      /* a texel's words as consecutive R32 texels of a row */
      size_t pitch = (size_t)depth_pitch(dev_width(prsc->format, w)) * 4;

      for (unsigned y = y0; y < y1; y++)
         memcpy(dst + y * pitch + (size_t)x0 * cbpp,
                src + y * src_stride + (size_t)x0 * cbpp,
                (size_t)(x1 - x0) * cbpp);
      return;
   }
   if (!twiddled(w, h)) {
      size_t pitch = (size_t)align(w, 8) * bpp;

      for (unsigned y = y0; y < y1; y++) {
         if (widen) {
            for (unsigned x = x0; x < x1; x++)
               widen_texel(dst + y * pitch + (size_t)x * 4,
                           src + y * src_stride + (size_t)x * cbpp, cbpp,
                           alpha);
            continue;
         }
         memcpy(dst + y * pitch + (size_t)x0 * bpp,
                src + y * src_stride + (size_t)x0 * bpp,
                (size_t)(x1 - x0) * bpp);
         if (rgbx)
            for (unsigned x = x0; x < x1; x++)
               dst[y * pitch + x * 4 + 3] = 0xff;
      }
      return;
   }

   /* a tile is t x t texels, Morton order inside (y in bit 0), tiles row by row
    */
   unsigned n = MIN3(util_logbase2(w), util_logbase2(h), util_logbase2(TILE)),
            t = 1u << n;
   uint32_t spread[TILE];
   uint8_t tile[TILE * TILE * 4];

   for (unsigned i = 0; i < t; i++) {
      spread[i] = 0;
      for (unsigned b = 0; b < n; b++)
         spread[i] |= (i >> b & 1) << (2 * b);
   }
   for (unsigned ty = y0 >> n; ty <= (y1 - 1) >> n; ty++) {
      for (unsigned tx = x0 >> n; tx <= (x1 - 1) >> n; tx++) {
         unsigned bx = tx << n, by = ty << n;
         uint8_t *d = dst + ((size_t)ty * (w >> n) + tx) * t * t * bpp;
         bool whole = bx >= x0 && by >= y0 && bx + t <= x1 && by + t <= y1;
         unsigned xa = MAX2(bx, x0), xb = MIN2(bx + t, x1), ya = MAX2(by, y0),
                  yb = MIN2(by + t, y1);
         uint8_t *out = whole ? tile : d;

         for (unsigned y = ya; y < yb; y++) {
            const uint8_t *s = src + y * src_stride;

            for (unsigned x = xa; x < xb; x++) {
               uint8_t *o =
                  out + (size_t)(spread[y - by] | spread[x - bx] << 1) * bpp;

               if (widen)
                  widen_texel(o, s + (size_t)x * cbpp, cbpp, alpha);
               else
                  memcpy(o, s + (size_t)x * bpp, bpp);
               if (rgbx)
                  o[3] = 0xff;
            }
         }
         if (whole)
            memcpy(d, tile, (size_t)t * t * bpp);
      }
   }
}

/* a box of a level of the CPU copy */
static void
encode_box(struct sgx_resource *res, unsigned level, unsigned layer,
           uint8_t *dst, unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
   const struct pipe_resource *prsc = &res->base;
   unsigned w = u_minify(prsc->width0, level),
            h = u_minify(prsc->height0, level);
   unsigned stride = util_format_get_stride(prsc->format, w);

   encode_rect(res, level,
               (const uint8_t *)res->data + res->level_offset[level] +
                  (size_t)layer * stride * h,
               dst, x0, y0, x1, y1);
}

/* The device levels past tail_from: each a 2x2 box filter of the one before,
 * from tail_from's CPU copy. */
static void
fill_tail(struct sgx_resource *res)
{
   const struct pipe_resource *prsc = &res->base;
   enum pipe_format f = prsc->format;
   unsigned last = res->tail_from, end = sgx_tex_dev_last(prsc);
   unsigned w = u_minify(prsc->width0, last), h = u_minify(prsc->height0, last);
   unsigned cbpp = util_format_get_blocksize(f);
   float *a, *b;
   uint8_t *row;

   if (end == last)
      return;
   a = malloc((size_t)w * h * 4 * sizeof(float));
   b = malloc((size_t)w * h * 4 * sizeof(float));
   row = malloc((size_t)w * h * cbpp);
   if (!a || !b || !row)
      goto out;
   for (unsigned y = 0; y < h; y++)
      util_format_unpack_rgba(f, a + (size_t)y * w * 4,
                              (const uint8_t *)res->data +
                                 res->level_offset[last] +
                                 (size_t)y * util_format_get_stride(f, w),
                              w);
   for (unsigned l = last + 1; l <= end; l++) {
      unsigned nw = MAX2(w / 2, 1), nh = MAX2(h / 2, 1);
      float *t;

      for (unsigned y = 0; y < nh; y++) {
         for (unsigned x = 0; x < nw; x++) {
            unsigned x0 = MIN2(2 * x, w - 1), x1 = MIN2(2 * x + 1, w - 1),
                     y0 = MIN2(2 * y, h - 1), y1 = MIN2(2 * y + 1, h - 1);

            for (unsigned c = 0; c < 4; c++)
               b[((size_t)y * nw + x) * 4 + c] =
                  (a[((size_t)y0 * w + x0) * 4 + c] +
                   a[((size_t)y0 * w + x1) * 4 + c] +
                   a[((size_t)y1 * w + x0) * 4 + c] +
                   a[((size_t)y1 * w + x1) * 4 + c]) *
                  0.25f;
         }
      }
      for (unsigned y = 0; y < nh; y++)
         util_format_pack_rgba(f, row + (size_t)y * nw * cbpp,
                               b + (size_t)y * nw * 4, nw);
      encode_rect(res, l, row,
                  (uint8_t *)res->bo.map + sgx_tex_offset(prsc, l, 0), 0, 0, nw,
                  nh);
      t = a, a = b, b = t;
      w = nw, h = nh;
   }
out:
   free(a);
   free(b);
   free(row);
}

void
sgx_tex_encode_box(struct sgx_resource *res, unsigned level, unsigned layer,
                   unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
   /* past tail_from the device holds the filtered tail instead */
   if (level > res->tail_from && res->base.target == PIPE_TEXTURE_2D)
      return;
   encode_box(res, level, layer,
              (uint8_t *)res->bo.map + sgx_tex_offset(&res->base, level, layer),
              x0, y0, x1, y1);
   if (level == res->tail_from)
      fill_tail(res);
}

/* The tail from level l instead: the levels it covered back from the CPU
 * copy up to l, then the tail past l (the caller saw to renders that read
 * the device copy) */
void
sgx_tex_set_tail(struct sgx_resource *res, unsigned l)
{
   unsigned old = res->tail_from;

   res->tail_from = l;
   for (unsigned k = old + 1; k <= MIN2(l, res->base.last_level); k++)
      encode_box(res, k, 0,
                 (uint8_t *)res->bo.map + sgx_tex_offset(&res->base, k, 0), 0,
                 0, ~0u, ~0u);
   fill_tail(res);
}

void
sgx_tex_encode_cube0(struct sgx_resource *res)
{
   size_t stride = face_stride_levels(&res->base, 1);

   for (unsigned layer = 0; layer < res->base.array_size; layer++)
      encode_box(res, 0, layer, (uint8_t *)res->cube0.map + layer * stride, 0,
                 0, ~0u, ~0u);
}

bool
sgx_tex_uses_cube0(const struct pipe_sampler_view *view,
                   const struct pipe_sampler_state *sampler)
{
   const struct pipe_resource *prsc = view->texture;

   return prsc->target == PIPE_TEXTURE_CUBE && prsc->last_level > 0 &&
          ((const struct sgx_resource *)prsc)->texture &&
          !(sampler && sampler->min_mip_filter != PIPE_TEX_MIPFILTER_NONE);
}

void
sgx_tex_encode_level(struct sgx_resource *res, unsigned level)
{
   for (unsigned layer = 0; layer < res->base.array_size; layer++)
      sgx_tex_encode_box(res, level, layer, 0, 0, ~0u, ~0u);
}

/* what a render left in a box of a level, into the CPU copy (the heap is read
 * across the bus: only for a transfer of a texture rendered into, and only
 * what it covers) */
void
sgx_tex_decode_box(struct sgx_resource *res, unsigned level, unsigned layer,
                   unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
   struct pipe_resource *prsc = &res->base;
   unsigned w = u_minify(prsc->width0, level),
            h = u_minify(prsc->height0, level);
   unsigned bpp = sgx_tex_bpp(prsc->format),
            cbpp = util_format_get_blocksize(prsc->format);
   unsigned stride = util_format_get_stride(prsc->format, w);
   uint8_t *dst = (uint8_t *)res->data + res->level_offset[level] +
                  (size_t)layer * stride * h;
   const uint8_t *src =
      (const uint8_t *)res->bo.map + sgx_tex_offset(prsc, level, layer);

   x1 = MIN2(x1, w);
   y1 = MIN2(y1, h);
   /* (nothing renders into a depth texture: the CPU copy is current) */
   if (x0 >= x1 || y0 >= y1 || sgx_tex_depth(prsc->format))
      return;
   if (sgx_tex_raw32(prsc->format)) {
      size_t pitch = (size_t)depth_pitch(dev_width(prsc->format, w)) * 4;

      for (unsigned y = y0; y < y1; y++)
         memcpy(dst + y * stride + (size_t)x0 * cbpp,
                src + y * pitch + (size_t)x0 * cbpp, (size_t)(x1 - x0) * cbpp);
      return;
   }
   if (!twiddled(w, h)) {
      size_t pitch = (size_t)align(w, 8) * bpp;

      for (unsigned y = y0; y < y1; y++) {
         if (cbpp != bpp) {
            for (unsigned x = x0; x < x1; x++)
               memcpy(dst + y * stride + (size_t)x * cbpp,
                      src + y * pitch + (size_t)x * bpp, cbpp);
            continue;
         }
         memcpy(dst + y * stride + (size_t)x0 * bpp,
                src + y * pitch + (size_t)x0 * bpp, (size_t)(x1 - x0) * bpp);
      }
      return;
   }
   /* a row of tiles in one sequential copy, then reordered in cached memory
    * (a texel at a time from the heap is a bus read each) */
   unsigned n = MIN3(util_logbase2(w), util_logbase2(h), util_logbase2(TILE)),
            t = 1u << n;
   unsigned tx0 = x0 >> n, tx1 = (x1 - 1) >> n;
   size_t tile_bytes = (size_t)t * t * bpp, span = (tx1 - tx0 + 1) * tile_bytes;
   uint32_t spread[TILE];
   uint8_t *tmp = malloc(span);

   for (unsigned i = 0; i < t; i++) {
      spread[i] = 0;
      for (unsigned b = 0; b < n; b++)
         spread[i] |= (i >> b & 1) << (2 * b);
   }
   for (unsigned ty = y0 >> n; ty <= (y1 - 1) >> n; ty++) {
      const uint8_t *s = src + ((size_t)ty * (w >> n) + tx0) * tile_bytes;

      if (tmp) {
         memcpy(tmp, s, span);
         s = tmp;
      }
      for (unsigned y = MAX2(ty << n, y0); y < MIN2((ty + 1) << n, y1); y++) {
         for (unsigned x = x0; x < x1; x++) {
            size_t at =
               ((x >> n) - tx0) * tile_bytes +
               (size_t)(spread[y & (t - 1)] | spread[x & (t - 1)] << 1) * bpp;

            memcpy(dst + y * stride + (size_t)x * cbpp, s + at, cbpp);
         }
      }
   }
   free(tmp);
}

void
sgx_tex_decode_level(struct sgx_resource *res, unsigned level)
{
   for (unsigned layer = 0; layer < res->base.array_size; layer++)
      sgx_tex_decode_box(res, level, layer, 0, 0, ~0u, ~0u);
}

static unsigned
wrap(enum pipe_tex_wrap w)
{
   switch (w) {
   case PIPE_TEX_WRAP_MIRROR_REPEAT:
      return 1;
   case PIPE_TEX_WRAP_CLAMP:
   case PIPE_TEX_WRAP_CLAMP_TO_EDGE:
   case PIPE_TEX_WRAP_CLAMP_TO_BORDER:
      return 2;
   default:
      return 0;
   }
}

void
sgx_tex_words(const struct pipe_sampler_view *view,
              const struct pipe_sampler_state *sampler, uint32_t t[4])
{
   const struct pipe_resource *prsc = view->texture;
   const struct sgx_resource *res = (const struct sgx_resource *)prsc;
   unsigned fmt = sgx_tex_format(prsc->format);
   /* A view from a later base level (GL_TEXTURE_BASE_LEVEL) of a 2D texture
    * starts at that level: the levels after it follow it as they follow
    * level 0 (TEX.md "Mipmaps"), so it reads as a texture of that size */
   unsigned base = prsc->target != PIPE_TEXTURE_CUBE && res->texture
                      ? view->u.tex.first_level
                      : 0;
   unsigned w = u_minify(prsc->width0, base), h = u_minify(prsc->height0, base);
   /* a linear (NPOT) texture's levels aren't a chain the sampler knows (the
    * DDK's GLES2 has no NPOT mipmaps), and sampling one with mipmapping
    * locks the 3D (piglit fbo-generatemipmap-formats NPOT): its base level */
   bool mips = sampler && sampler->min_mip_filter != PIPE_TEX_MIPFILTER_NONE &&
               view->u.tex.last_level > base &&
               (prsc->target == PIPE_TEXTURE_CUBE || twiddled(w, h) ||
                !sgx_screen(prsc->screen)->core->npot_mip_hangs);

   sgx_pack(&t[0], TEXSTATE_WORD0, t0)
   {
      t0.no_mipmaps = !mips;
      t0.unknown_30 = fmt == SGX_TEXSTATE_FORMAT_A8 ||
                      fmt == SGX_TEXSTATE_FORMAT_L8 ||
                      fmt == SGX_TEXSTATE_FORMAT_LA88;
      if (sampler) {
         t0.wrap_t = wrap(sampler->wrap_t);
         t0.wrap_s = wrap(sampler->wrap_s);
         t0.mip_linear =
            mips && sampler->min_mip_filter == PIPE_TEX_MIPFILTER_LINEAR;
         t0.min_linear = sampler->min_img_filter == PIPE_TEX_FILTER_LINEAR;
         t0.mag_linear = sampler->mag_img_filter == PIPE_TEX_FILTER_LINEAR;
      }
   }
   /* T1: a cube map's layout from a DDK capture */
   sgx_pack(&t[1], TEXSTATE_WORD1, t1)
   {
      t1.layout = prsc->target == PIPE_TEXTURE_CUBE ? SGX_TEXSTATE_LAYOUT_CUBE
                  : twiddled(w, h) ? SGX_TEXSTATE_LAYOUT_TWIDDLED
                                   : SGX_TEXSTATE_LAYOUT_LINEAR;
      t1.format = fmt;
      t1.width = w - 1;
      t1.height = h - 1;
   }
   /* (the draw brought the level-0 copy up to date: sgx_resource_cube0) */
   sgx_pack(&t[2], TEXSTATE_WORD2, t2)
   {
      t2.address =
         SGX_DEV_ADDR(sgx_tex_uses_cube0(view, sampler) && res->cube0.va
                         ? res->cube0.va
                         : res->bo.va + chain_offset(prsc, base));
   }
   if (!res->texture) {
      /* A render target, read where it is: strided (T1 layout 6), the pitch
       * - 1 in T0 b9-11 and b17-26 in place of the mipmap and min filter
       * bits, as the DDK's swap blit reads its back buffer (TEX.md section
       * 3), in the format sgx_tex_format gives it. */
      unsigned p = res->pitch - 1;
      bool bgra = prsc->format == PIPE_FORMAT_B8G8R8A8_UNORM ||
                  prsc->format == PIPE_FORMAT_B8G8R8X8_UNORM;

      sgx_pack(&t[0], TEXSTATE_STRIDED_WORD0, t0)
      {
         t0.pitch_lo = p & 7;
         t0.pitch_hi = p >> 3;
         if (sampler) {
            t0.wrap_t = wrap(sampler->wrap_t);
            t0.wrap_s = wrap(sampler->wrap_s);
            t0.mag_linear = sampler->mag_img_filter == PIPE_TEX_FILTER_LINEAR;
         }
      }
      sgx_pack(&t[1], TEXSTATE_WORD1, t1)
      {
         t1.layout = SGX_TEXSTATE_LAYOUT_STRIDED;
         t1.format = bgra ? 0x0c : 0x0d;
         t1.width = prsc->width0 - 1;
         t1.height = prsc->height0 - 1;
      }
   }
   if (sgx_tex_word32(prsc->format)) {
      /* rows of depth_pitch texels, strided as a render target is read
       * (16x16: T0 00060e00, T1 d200f00f), base level only; an RG32 or
       * RGBA32 texture as the R32 one two or four times as wide */
      unsigned dw = dev_width(prsc->format, w);
      unsigned p = depth_pitch(dw) - 1;

      sgx_pack(&t[0], TEXSTATE_STRIDED_WORD0, t0)
      {
         t0.pitch_lo = p & 7;
         t0.pitch_hi = p >> 3;
         if (sampler) {
            t0.wrap_t = wrap(sampler->wrap_t);
            /* an RG32 or RGBA32 one is read at subtexel centres the
             * compiler wraps itself: s repeats (sgx_lower_tex) */
            t0.wrap_s = wrap(sgx_tex_raw32(prsc->format) > 1
                                ? PIPE_TEX_WRAP_REPEAT
                                : sampler->wrap_s);
         }
      }
      sgx_pack(&t[1], TEXSTATE_WORD1, t1)
      {
         t1.layout = SGX_TEXSTATE_LAYOUT_STRIDED;
         t1.format = SGX_TEXSTATE_FORMAT_F32;
         t1.width = dw - 1;
         t1.height = h - 1;
      }
   }
   /* T3: the sample instruction's half; 00800800 for a single 2-D sample [D] */
   sgx_pack(&t[3], TEXSTATE_WORD3, t3) t3.unknown_11 = true;
}
