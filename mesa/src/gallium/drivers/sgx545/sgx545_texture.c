/*
 * Copyright © 2026 The sgx545-ce authors
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
   case PIPE_FORMAT_R8G8B8A8_UNORM:
   case PIPE_FORMAT_R8G8B8X8_UNORM:
      /* R, G, B, A bytes in memory.  The DDK tags them 0x0d and libusc folds a
       * zyxw reorder into the sample; our compiler unpacks xyzw, and with 0x0c
       * (BGRA8888) the texels come out GL's way: byte-identical to the DDK
       * on the EA3 (16x16, 64x64, NPOT, nearest and linear) */
      return 0x0c;
   case PIPE_FORMAT_B5G6R5_UNORM:
      return 0x05;
   case PIPE_FORMAT_A8_UNORM:
      return 0x19;
   case PIPE_FORMAT_L8_UNORM:
      return 0x1a;
   case PIPE_FORMAT_L8A8_UNORM:
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

size_t
sgx545_tex_level_offset(const struct pipe_resource *prsc, unsigned level)
{
   unsigned bpp = util_format_get_blocksize(prsc->format);
   size_t off = 0;

   for (unsigned l = 0; l < level; l++)
      off += level_bytes(u_minify(prsc->width0, l), u_minify(prsc->height0, l), bpp) *
             prsc->array_size;
   return off;
}

size_t
sgx545_tex_size(const struct pipe_resource *prsc)
{
   return sgx545_tex_level_offset(prsc, prsc->last_level + 1);
}

/* texel index of (x, y) in a twiddled w x h level */
static uint32_t
twiddle(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
   uint32_t n = MIN3(util_logbase2(w), util_logbase2(h), util_logbase2(TILE)), i = 0;

   for (uint32_t b = 0; b < n; b++)
      i |= (y >> b & 1) << (2 * b) | (x >> b & 1) << (2 * b + 1);
   return i | ((y >> n) * (w >> n) + (x >> n)) << (2 * n);
}

void
sgx545_tex_encode_level(struct sgx545_resource *res, unsigned level)
{
   struct pipe_resource *prsc = &res->base;
   unsigned w = u_minify(prsc->width0, level), h = u_minify(prsc->height0, level);
   unsigned bpp = util_format_get_blocksize(prsc->format);
   unsigned src_stride = util_format_get_stride(prsc->format, w);
   const uint8_t *src = (const uint8_t *)res->data + res->level_offset[level];
   uint8_t *dst = (uint8_t *)res->bo.map + sgx545_tex_level_offset(prsc, level);
   bool rgbx = prsc->format == PIPE_FORMAT_R8G8B8X8_UNORM;

   if (!twiddled(w, h)) {
      size_t pitch = (size_t)align(w, 8) * bpp;

      for (unsigned y = 0; y < h; y++) {
         memcpy(dst + y * pitch, src + y * src_stride, (size_t)w * bpp);
         if (rgbx)
            for (unsigned x = 0; x < w; x++)
               dst[y * pitch + x * 4 + 3] = 0xff;
      }
      return;
   }
   for (unsigned y = 0; y < h; y++) {
      for (unsigned x = 0; x < w; x++) {
         uint8_t *d = dst + (size_t)twiddle(x, y, w, h) * bpp;

         memcpy(d, src + y * src_stride + (size_t)x * bpp, bpp);
         if (rgbx)
            d[3] = 0xff;
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
   bool mips = sampler && sampler->min_mip_filter != PIPE_TEX_MIPFILTER_NONE && prsc->last_level > 0;

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
   /* T1: layout (0 twiddled, 3 linear) | format | width-1 | height-1 */
   t[1] = (twiddled(prsc->width0, prsc->height0) ? 0u : 3u) << 29 | fmt << 24 |
          (prsc->width0 - 1) << 12 | (prsc->height0 - 1);
   t[2] = res->bo.va;
   /* T3: the sample instruction's half; 00800800 for a single 2-D sample [D] */
   t[3] = 0x00800800;
}
