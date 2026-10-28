/*
 * sgxtex: SGX545 texture memory layout and the transfer-queue blit, as
 * measured from the Cedarview DDK 1.7 on an EA3 (docs/TEX.md).
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
 * bpp bytes per texel exactly as stored (RGBA8888 = R,G,B,A bytes; RGB565 and
 * LA88 = the GL ushort; RGBA4444 / RGBA5551 after sgxtex_convert16). dst
 * points at the level's first byte.
 */
void sgxtex_encode_level(void *dst, const void *src, size_t src_stride, uint32_t w, uint32_t h,
			 uint32_t bpp);

/* a GL RGBA4444 / RGBA5551 ushort as stored (ARGB4444 / ARGB1555); others unchanged */
uint16_t sgxtex_convert16(uint32_t t1fmt, uint16_t v);

/* T1 word of a sampled texture: format code (b24-28), layout (b29-31), size */
#define SGXTEX_FMT_RGBA4444 0x02	/* stored ARGB4444 */
#define SGXTEX_FMT_RGBA5551 0x04	/* stored ARGB1555 */
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

/* ---- general blits: any layout and format on either side, nearest or linear ---- */

/* A surface as a blit sees it. */
struct sgxtex_surface {
	uint32_t addr;		/* device address of (0, 0) */
	uint32_t w, h;
	uint32_t pitch;		/* pixels per row: strided and linear layouts (linear: sgxtex_pitch(w)) */
	uint32_t layout;	/* SGXTEX_LAYOUT_TWIDDLED / _LINEAR / _STRIDED */
	uint32_t t1fmt;		/* format as a source (SGXTEX_FMT_*) */
	uint32_t pbefmt;	/* format as a destination (SGXPBE_FMT_*) */
};

#define SGXTEX_FILTER_NEAREST 0
#define SGXTEX_FILTER_LINEAR 1

/*
 * The source's DOUTT words. Strided sources hold their pitch where the
 * minification filter bit would be, so they sample nearest (or magnify
 * linearly); twiddled and linear ones take either filter. T0 clamps.
 */
void sgxtex_blit_source(uint32_t tex[4], const struct sgxtex_surface *src, int filter);

/* Everything one blit writes, apart from the context's static programs. */
struct sgxtex_blit {
	uint32_t cmd[SGXTEX_BLIT_CMD_WORDS];	/* at the command's CCB offset */
	uint32_t bg[SGXTEX_BLIT_BG_WORDS];	/* at ctx->bg */
	uint32_t pds[SGXTEX_BLIT_PDS_WORDS];	/* at ctx->pds_primary */
	uint32_t eot[SGXPBE_EOT_WORDS];		/* at ctx->eot_use */
};

/*
 * The background object is the triangle (0,0) (W,0) (0,H) over the whole
 * destination; words 3-8 of its block are the texture coordinates (u, v) at
 * those three vertices, interpolated across it (EA3, 2026-10-28). A
 * sub-rectangle copy keeps the triangle, sets the coordinates so destination
 * pixel (x, y) samples source texel (sx - dx + x, sy - dy + y) scaled by
 * sw/dw, sh/dh, clips the PBE to the destination rectangle and the tiles to
 * the ones it touches. T0 clamps, so no read leaves the source.
 *
 * A twiddled destination only clips in y: the PBE takes a twiddled surface's
 * tile row length from the clip width, so a narrower clip writes the right
 * pixels to the wrong places. Full-width bands are exact. For any other
 * rectangle of a twiddled surface: read it back to a linear scratch surface
 * (sgxtex_blit_build), copy the rectangle into that (this function), and
 * upload the scratch again; all three are exact on the EA3.
 */
struct sgxtex_rect {
	uint32_t x, y, w, h;
};

/* src scaled onto all of dst (equal sizes copy; half sizes with LINEAR box-filter a mip level) */
void sgxtex_blit_build(struct sgxtex_blit *b, const struct sgxtex_blit_ctx *c, const struct sgxtex_surface *src,
		       const struct sgxtex_surface *dst, int filter);

/*
 * The whole 0x1c0-byte transfer command at CCB offset `off`: the client words
 * (b->cmd), the size word, the constant tail; the kernel fills the shared part
 * at off + SGXTEX_CMD_SHARED, which is what SGX_SUBMITTRANSFER's
 * ui32SharedCmdCCBOffset names. The client then advances the CCB control's
 * write offset (word 0) by SGXTEX_CMD_BYTES before submitting.
 */
#define SGXTEX_CMD_BYTES 0x1c0
#define SGXTEX_CMD_SHARED 0xac
/* srect of src scaled onto drect of dst; the rest of dst is untouched (glCopyTexSubImage, glBlitFramebuffer).
 * Returns -1 (and builds nothing) for a twiddled dst whose drect is not full width. */
int sgxtex_blit_build_rect(struct sgxtex_blit *b, const struct sgxtex_blit_ctx *c, const struct sgxtex_surface *src,
			    const struct sgxtex_rect *srect, const struct sgxtex_surface *dst,
			    const struct sgxtex_rect *drect, int filter);

void sgxtex_blit_command_full(uint32_t out[SGXTEX_CMD_BYTES / 4], const struct sgxtex_blit *b, int twiddled_dst);

/* ---- a transfer context of one's own (what libsrv_um's SGXCreateTransferContext sets up) ---- */

/*
 * Buffers, as the DDK allocates them (heap id, attribs, size):
 *   CCB          kernel data (3), 0x4f, 0x10000 + 0x4c8     CCB control  kernel data (3), 0x4f, 8
 *   bg block     general (0), 0xd, 0x4800 ("ISP stream")    PDS code     PDS pixel (6), 0xd, 0x1800
 *   pixel PDS    PDS pixel (6), 0xd, 0x400                  USSE code    pixel shader (4), 0xd, 0x800
 * The PDS code buffer holds the secondary program (at +0) and the pixel-event
 * program (at +0x20); the USSE code buffer the end-of-tile, emit, nop and copy
 * programs. Register with SGX_REGISTER_HW_TRANSFER_CONTEXT, PD offset 8.
 */
#define SGXTEX_XCTX_BYTES 0xb4
void sgxtex_xctx_words(uint32_t ctx[SGXTEX_XCTX_BYTES / 4], uint32_t ccb, uint32_t ccb_ctl, uint32_t pid);

/* the secondary PDS program: DOUTU to a `nop [end]` program; 8 words */
#define SGXTEX_SECONDARY_WORDS 8
void sgxtex_xctx_secondary(uint32_t *pds, uint32_t nop_use);

/* the pixel-event PDS program: 5 data rows + 29 code words (PDS.md; tools/gen's pixel_event) */
#define SGXTEX_EVENT_WORDS 49
#define SGXTEX_EVENT_ROWS 5
void sgxtex_xctx_pixel_event(uint32_t *pds, uint32_t eot_use, uint32_t emit_use);

/* small USSE programs the context needs (words, low word first) */
extern const uint32_t sgxtex_nop_shader[4];	/* phas.i #0, #0; nop [end] */
extern const uint32_t sgxtex_emit_shader[6];	/* phas.i #0, #0; limm r0, #0; emit.t0 r0, #0, r0 [end] */

/* DOUTU word 0 for a pixel-shader-heap program (PDS.md): temporaries in fours at b24 */
uint32_t sgxtex_doutu(uint32_t use_addr, uint32_t temps);

#endif
