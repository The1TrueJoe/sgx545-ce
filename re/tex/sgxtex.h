/*
 * sgxtex: SGX545 texture memory layout and the transfer-queue blit, as
 * measured from the Cedarview DDK 1.7 on an EA3 (re/tex/TEX.md).
 *
 * Plain C99, no dependencies: meant to be copied into a driver.
 *
 * Layout:
 *   power-of-two w x h (T1 layout 0): square tiles of min(w, h, 16) texels,
 *   Morton order inside a tile with y in the lowest bit, tiles row-major;
 *   anything else (T1 layout 3): linear rows, pitch rounded up to 8 texels;
 *   mip level n follows level n-1.
 *
 * Blit (SGX_SUBMITTRANSFER): a 3D render whose only object is the background
 * object; its pixel PDS program samples the source (DOUTT, strided BGRA8888)
 * with the shader `phas; mov o0, pa0`, and the end-of-tile program's PBE
 * words name the destination. sgxtex_blit_* build every word libsrv_um
 * writes for it (checked bit for bit against 17 DDK transfers).
 */
#ifndef SGXTEX_H
#define SGXTEX_H

#include <stdint.h>
#include <stddef.h>

/* ---- texel layout ---- */

/* power of two (and > 0) */
int sgxtex_is_pot(uint32_t n);
/* row pitch in texels of a linear (non power-of-two) level */
uint32_t sgxtex_pitch(uint32_t w);
/* texel index of (x, y) in a twiddled w x h level (both powers of two) */
uint32_t sgxtex_twiddle(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
/* bytes of one level, and the byte offset of level lv from the base */
size_t sgxtex_level_bytes(uint32_t w, uint32_t h, uint32_t bpp);
size_t sgxtex_level_offset(uint32_t w, uint32_t h, uint32_t bpp, uint32_t lv);
/* byte offset of texel (x, y) of level lv of a w x h texture */
size_t sgxtex_texel_offset(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t bpp, uint32_t lv);
/*
 * Lay out one level: src is linear, top row first, src_stride bytes per row,
 * bpp bytes per texel exactly as GL uploads it (RGBA8888 = R,G,B,A bytes;
 * 16-bit formats = the GL ushort). dst points at the level's first byte.
 */
void sgxtex_encode_level(void *dst, const void *src, size_t src_stride, uint32_t w, uint32_t h,
			 uint32_t bpp);

/* T1 word of a sampled texture: format code (b24-28), layout (b29-31), size */
#define SGXTEX_FMT_RGBA4444 0x02
#define SGXTEX_FMT_RGBA5551 0x04
#define SGXTEX_FMT_RGB565 0x05
#define SGXTEX_FMT_BGRA8888 0x0c
#define SGXTEX_FMT_RGBA8888 0x0d
#define SGXTEX_FMT_A8 0x19
#define SGXTEX_FMT_L8 0x1a
#define SGXTEX_FMT_LA88 0x1b
#define SGXTEX_LAYOUT_TWIDDLED 0
#define SGXTEX_LAYOUT_LINEAR 3
#define SGXTEX_LAYOUT_STRIDED 6
uint32_t sgxtex_t1(uint32_t fmt, uint32_t layout, uint32_t w, uint32_t h);
/* T0 pitch bits of a strided (layout 6) texture: pitch-1 in b9-11 (low 3) and b17-26 */
uint32_t sgxtex_t0_stride(uint32_t pitch);

/* ---- render target (PBE) ---- */

/* PBE format codes (sideband b0-5), from libGLESv2's format table */
#define SGXPBE_FMT_BGRA8888 0
#define SGXPBE_FMT_RGBA8888 1
#define SGXPBE_FMT_RGB565 3
#define SGXPBE_FMT_RGBA5551 4
#define SGXPBE_FMT_RGBA4444 5
#define SGXPBE_SIDE_ALWAYS 0x200
#define SGXPBE_SIDE_DITHER 0x400
#define SGXPBE_SIDE_TWIDDLED 0x2000

struct sgxpbe {
	uint32_t w[4];	/* clip min, clip max, stride, address */
	uint32_t side;	/* sideband: format | flags */
};

/* w x h at addr, pitch in pixels; twiddled for a power-of-two texture target */
void sgxpbe_words(struct sgxpbe *p, uint32_t w, uint32_t h, uint32_t pitch, uint32_t addr, uint32_t fmt,
		  int twiddled, int dither);
/* the end-of-tile USSE program: 14 words (7 instructions, low word first) */
#define SGXPBE_EOT_WORDS 14
void sgxpbe_eot_program(uint32_t *out, const struct sgxpbe *p);

/* ---- transfer-queue blit ---- */

struct sgxtex_blit_ctx {
	uint32_t bg;		/* background-object block, general heap, 24 words */
	uint32_t pds_secondary;	/* PDS pixel heap: the context's secondary program (1 data row) */
	uint32_t pds_primary;	/* PDS pixel heap: the background pixel program (16 words) */
	uint32_t pds_event;	/* PDS pixel heap: the pixel-event program (5 data rows, 1 code unit) */
	uint32_t copy_use;	/* pixel-shader heap: `phas.i #0, #0; mov o0, pa0 [end]` */
	uint32_t eot_use;	/* pixel-shader heap: the end-of-tile program (14 words) */
};

#define SGXTEX_BLIT_CMD_WORDS 16
#define SGXTEX_BLIT_BG_WORDS 24
#define SGXTEX_BLIT_PDS_WORDS 16

/* the copy shader, 4 words */
extern const uint32_t sgxtex_copy_shader[4];

/* the transfer command's client words 0-15 (at the CCB offset of the command; shared part at +0xac) */
void sgxtex_blit_command(uint32_t *cmd, const struct sgxtex_blit_ctx *c, uint32_t w, uint32_t h);
/* the background-object block for a w x h destination */
void sgxtex_blit_bg(uint32_t *blk, const struct sgxtex_blit_ctx *c, uint32_t w, uint32_t h);
/* the background pixel program sampling a strided BGRA8888 source */
void sgxtex_blit_pixel_program(uint32_t *pp, const struct sgxtex_blit_ctx *c, uint32_t src, uint32_t w,
			       uint32_t h, uint32_t src_pitch);

#endif
