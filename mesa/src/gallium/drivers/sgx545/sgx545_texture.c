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

#include "sgx545_resource.h"
#include "sgx545_texture.h"

#define TILE 16u

unsigned
sgx545_tex_format(enum pipe_format format)
{
   switch (format) {
   /* an sRGB format is its linear twin here: the fragment shader decodes
    * what it samples (lower_srgb, sgx545_fs_for) */
   case PIPE_FORMAT_R8G8B8A8_UNORM:
   case PIPE_FORMAT_R8G8B8X8_UNORM:
   case PIPE_FORMAT_R8G8B8A8_SRGB:
   case PIPE_FORMAT_R8G8B8X8_SRGB:
      /* R, G, B, A bytes in memory.  The DDK tags them 0x0d and libusc folds a
       * zyxw reorder into the sample; our compiler unpacks xyzw, and with 0x0c
       * (BGRA8888) the texels come out GL's way: byte-identical to the DDK
       * on the EA3 (16x16, 64x64, NPOT, nearest and linear) */
      return 0x0c;
   case PIPE_FORMAT_B8G8R8A8_UNORM:
   case PIPE_FORMAT_B8G8R8X8_UNORM:
   case PIPE_FORMAT_B8G8R8A8_SRGB:
   case PIPE_FORMAT_B8G8R8X8_SRGB:
      /* B, G, R, A bytes in memory: the format that swaps R and B against
       * 0x0c, as a strided back buffer is sampled (sgx545_tex_words) */
      return 0x0d;
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
   default:
      return 0;
   }
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
   unsigned bpp = util_format_get_blocksize(prsc->format);
   size_t off = 0;

   for (unsigned l = 0; l < level; l++)
      off += level_bytes(u_minify(prsc->width0, l), u_minify(prsc->height0, l), bpp);
   return off;
}

/* Cube faces follow one another, each a complete chain of the levels the
 * sampler sees: packed when level 0 is smaller than one 16x16 tile; a chain
 * of mip levels on 0x800 bytes whatever the format (the DDK's cube storage,
 * EA1: L8 16/32/64 0x800/0x800/0x1800 a face, LA88 and RGBA4444
 * 0x800/0x1000/0x3000, RGBA8888 0x800/0x1800/0x5800); level 0 alone on a
 * tile's bytes (RGBA8888 16x16: 0x400; the other formats unmeasured, [G]) */
static size_t
face_stride_levels(const struct pipe_resource *prsc, unsigned levels)
{
   size_t chain = chain_offset(prsc, levels);

   if (prsc->width0 < TILE)
      return chain;
   return align64(chain, levels > 1 ? 0x800 : TILE * TILE * util_format_get_blocksize(prsc->format));
}

static size_t
face_stride(const struct pipe_resource *prsc)
{
   return face_stride_levels(prsc, prsc->last_level + 1);
}

size_t
sgx545_tex_cube0_size(const struct pipe_resource *prsc)
{
   return prsc->array_size * face_stride_levels(prsc, 1);
}

size_t
sgx545_tex_offset(const struct pipe_resource *prsc, unsigned level, unsigned layer)
{
   return layer * face_stride(prsc) + chain_offset(prsc, level);
}

size_t
sgx545_tex_size(const struct pipe_resource *prsc)
{
   return prsc->array_size > 1 ? prsc->array_size * face_stride(prsc) : chain_offset(prsc, prsc->last_level + 1);
}

/* Copy the box x0..x1, y0..y1 of a level from the CPU copy into the texture
 * heap.  The heap is write-combined: a twiddled tile the box covers whole is
 * put together on the stack and written in one go, only partly covered tiles
 * at the box's edges are written texel by texel. */
static void
encode_box(struct sgx545_resource *res, unsigned level, unsigned layer, uint8_t *dst, unsigned x0,
           unsigned y0, unsigned x1, unsigned y1)
{
   struct pipe_resource *prsc = &res->base;
   unsigned w = u_minify(prsc->width0, level), h = u_minify(prsc->height0, level);
   unsigned bpp = util_format_get_blocksize(prsc->format);
   unsigned src_stride = util_format_get_stride(prsc->format, w);
   const uint8_t *src = (const uint8_t *)res->data + res->level_offset[level] + (size_t)layer * src_stride * h;
   bool rgbx = prsc->format == PIPE_FORMAT_R8G8B8X8_UNORM;

   x1 = MIN2(x1, w);
   y1 = MIN2(y1, h);
   if (x0 >= x1 || y0 >= y1)
      return;
   if (!twiddled(w, h)) {
      size_t pitch = (size_t)align(w, 8) * bpp;

      for (unsigned y = y0; y < y1; y++) {
         memcpy(dst + y * pitch + (size_t)x0 * bpp, src + y * src_stride + (size_t)x0 * bpp,
                (size_t)(x1 - x0) * bpp);
         if (rgbx)
            for (unsigned x = x0; x < x1; x++)
               dst[y * pitch + x * 4 + 3] = 0xff;
      }
      return;
   }

   /* a tile is t x t texels, Morton order inside (y in bit 0), tiles row by row */
   unsigned n = MIN3(util_logbase2(w), util_logbase2(h), util_logbase2(TILE)), t = 1u << n;
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
         unsigned xa = MAX2(bx, x0), xb = MIN2(bx + t, x1), ya = MAX2(by, y0), yb = MIN2(by + t, y1);
         uint8_t *out = whole ? tile : d;

         for (unsigned y = ya; y < yb; y++) {
            const uint8_t *s = src + y * src_stride;

            for (unsigned x = xa; x < xb; x++) {
               uint8_t *o = out + (size_t)(spread[y - by] | spread[x - bx] << 1) * bpp;

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

void
sgx545_tex_encode_box(struct sgx545_resource *res, unsigned level, unsigned layer, unsigned x0,
                      unsigned y0, unsigned x1, unsigned y1)
{
   encode_box(res, level, layer, (uint8_t *)res->bo.map + sgx545_tex_offset(&res->base, level, layer), x0, y0,
              x1, y1);
}

void
sgx545_tex_encode_cube0(struct sgx545_resource *res)
{
   size_t stride = face_stride_levels(&res->base, 1);

   for (unsigned layer = 0; layer < res->base.array_size; layer++)
      encode_box(res, 0, layer, (uint8_t *)res->cube0.map + layer * stride, 0, 0, ~0u, ~0u);
}

bool
sgx545_tex_uses_cube0(const struct pipe_sampler_view *view, const struct pipe_sampler_state *sampler)
{
   const struct pipe_resource *prsc = view->texture;

   return prsc->target == PIPE_TEXTURE_CUBE && prsc->last_level > 0 &&
          ((const struct sgx545_resource *)prsc)->texture &&
          !(sampler && sampler->min_mip_filter != PIPE_TEX_MIPFILTER_NONE);
}

void
sgx545_tex_encode_level(struct sgx545_resource *res, unsigned level)
{
   for (unsigned layer = 0; layer < res->base.array_size; layer++)
      sgx545_tex_encode_box(res, level, layer, 0, 0, ~0u, ~0u);
}

/* what a render left in a level, into the CPU copy (uncached reads: only for a
 * transfer of a texture rendered into) */
void
sgx545_tex_decode_level(struct sgx545_resource *res, unsigned level)
{
   struct pipe_resource *prsc = &res->base;
   unsigned w = u_minify(prsc->width0, level), h = u_minify(prsc->height0, level);
   unsigned bpp = util_format_get_blocksize(prsc->format);
   unsigned stride = util_format_get_stride(prsc->format, w);

   for (unsigned layer = 0; layer < prsc->array_size; layer++) {
      uint8_t *dst = (uint8_t *)res->data + res->level_offset[level] + (size_t)layer * stride * h;
      const uint8_t *src = (const uint8_t *)res->bo.map + sgx545_tex_offset(prsc, level, layer);

      if (!twiddled(w, h)) {
         size_t pitch = (size_t)align(w, 8) * bpp;

         for (unsigned y = 0; y < h; y++)
            memcpy(dst + y * stride, src + y * pitch, stride);
         continue;
      }
      unsigned n = MIN3(util_logbase2(w), util_logbase2(h), util_logbase2(TILE)), t = 1u << n;
      uint32_t spread[TILE];

      for (unsigned i = 0; i < t; i++) {
         spread[i] = 0;
         for (unsigned b = 0; b < n; b++)
            spread[i] |= (i >> b & 1) << (2 * b);
      }
      for (unsigned y = 0; y < h; y++) {
         for (unsigned x = 0; x < w; x++) {
            size_t tile = (size_t)(y >> n) * (w >> n) + (x >> n);
            size_t at = tile * t * t + (spread[y & (t - 1)] | spread[x & (t - 1)] << 1);

            memcpy(dst + y * stride + (size_t)x * bpp, src + at * bpp, bpp);
         }
      }
   }
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
sgx545_tex_words(const struct pipe_sampler_view *view, const struct pipe_sampler_state *sampler,
                 uint32_t t[4])
{
   const struct pipe_resource *prsc = view->texture;
   const struct sgx545_resource *res = (const struct sgx545_resource *)prsc;
   unsigned fmt = sgx545_tex_format(prsc->format);
   /* A view from a later base level (GL_TEXTURE_BASE_LEVEL) of a 2D texture
    * starts at that level: the levels after it follow it as they follow
    * level 0 (TEX.md "Mipmaps"), so it reads as a texture of that size */
   unsigned base = prsc->target != PIPE_TEXTURE_CUBE && res->texture ? view->u.tex.first_level : 0;
   unsigned w = u_minify(prsc->width0, base), h = u_minify(prsc->height0, base);
   /* a linear (NPOT) texture's levels aren't a chain the sampler knows (the
    * DDK's GLES2 has no NPOT mipmaps), and sampling one with mipmapping
    * locks the 3D (piglit fbo-generatemipmap-formats NPOT): its base level */
   bool mips = sampler && sampler->min_mip_filter != PIPE_TEX_MIPFILTER_NONE && view->u.tex.last_level > base &&
               (prsc->target == PIPE_TEXTURE_CUBE || twiddled(w, h));

   /* T0: wrap T b3-5, wrap S b6-8, mip linear b9, min linear b10, mag linear
    * b12, b17-20 0xf (0xd with mipmaps), b21-26 0x3f, b30 for L8/A8/LA88 */
   t[0] = 0x3fu << 21 | (mips ? 0xdu : 0xfu) << 17 |
          (fmt == 0x19 || fmt == 0x1a || fmt == 0x1b ? 1u << 30 : 0);
   if (sampler) {
      t[0] |= wrap(sampler->wrap_t) << 3 | wrap(sampler->wrap_s) << 6;
      if (mips && sampler->min_mip_filter == PIPE_TEX_MIPFILTER_LINEAR)
         t[0] |= 1u << 9;
      if (sampler->min_img_filter == PIPE_TEX_FILTER_LINEAR)
         t[0] |= 1u << 10;
      if (sampler->mag_img_filter == PIPE_TEX_FILTER_LINEAR)
         t[0] |= 1u << 12;
   }
   /* T1: layout (0 twiddled, 3 linear, 2 a cube map: DDK capture) | format | width-1 | height-1 */
   t[1] = (prsc->target == PIPE_TEXTURE_CUBE ? 2u : twiddled(w, h) ? 0u : 3u) << 29 | fmt << 24 |
          (w - 1) << 12 | (h - 1);
   /* (the draw brought the level-0 copy up to date: sgx545_resource_cube0) */
   t[2] = sgx545_tex_uses_cube0(view, sampler) && res->cube0.va ? res->cube0.va
                                                               : res->bo.va + chain_offset(prsc, base);
   if (!res->texture) {
      /* A render target, read where it is: strided (T1 layout 6), the pitch
       * - 1 in T0 b9-11 and b17-26 in place of the mipmap and min filter
       * bits, as the DDK's swap blit reads its back buffer (TEX.md section
       * 3).  B, G, R, A in memory is 0x0d, the format that swaps R and B
       * against 0x0c. */
      unsigned p = res->pitch - 1;
      bool bgra = prsc->format == PIPE_FORMAT_B8G8R8A8_UNORM || prsc->format == PIPE_FORMAT_B8G8R8X8_UNORM;

      t[0] = (p & 7) << 9 | (p >> 3) << 17;
      if (sampler) {
         t[0] |= wrap(sampler->wrap_t) << 3 | wrap(sampler->wrap_s) << 6;
         if (sampler->mag_img_filter == PIPE_TEX_FILTER_LINEAR)
            t[0] |= 1u << 12;
      }
      t[1] = 6u << 29 | (bgra ? 0x0du : 0x0cu) << 24 | (prsc->width0 - 1) << 12 | (prsc->height0 - 1);
   }
   /* T3: the sample instruction's half; 00800800 for a single 2-D sample [D] */
   t[3] = 0x00800800;
}
