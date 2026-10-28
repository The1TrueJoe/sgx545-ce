/* sgxtex: see sgxtex.h and re/tex/TEX.md */
#include <string.h>

#include "sgxtex.h"

#define PDS_BASE 0x2e000000u
#define USE_BASE 0x37000000u
#define TILE 16u

int sgxtex_is_pot(uint32_t n)
{
	return n && !(n & (n - 1));
}

uint32_t sgxtex_pitch(uint32_t w)
{
	return (w + 7) & ~7u;
}

static uint32_t log2u(uint32_t n)
{
	uint32_t l = 0;

	while (n >>= 1)
		l++;
	return l;
}

uint32_t sgxtex_twiddle(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	uint32_t n = log2u(w), i = 0;

	if (log2u(h) < n)
		n = log2u(h);
	if (log2u(TILE) < n)
		n = log2u(TILE);
	for (uint32_t b = 0; b < n; b++)
		i |= (y >> b & 1) << (2 * b) | (x >> b & 1) << (2 * b + 1);
	return i | ((y >> n) * (w >> n) + (x >> n)) << (2 * n);
}

size_t sgxtex_level_bytes(uint32_t w, uint32_t h, uint32_t bpp)
{
	if (sgxtex_is_pot(w) && sgxtex_is_pot(h))
		return (size_t)w * h * bpp;
	return (size_t)sgxtex_pitch(w) * h * bpp;
}

static uint32_t lsize(uint32_t s, uint32_t lv)
{
	return s >> lv ? s >> lv : 1;
}

size_t sgxtex_level_offset(uint32_t w, uint32_t h, uint32_t bpp, uint32_t lv)
{
	size_t o = 0;

	for (uint32_t k = 0; k < lv; k++)
		o += sgxtex_level_bytes(lsize(w, k), lsize(h, k), bpp);
	return o;
}

size_t sgxtex_texel_offset(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t bpp, uint32_t lv)
{
	uint32_t lw = lsize(w, lv), lh = lsize(h, lv);
	size_t i = sgxtex_is_pot(lw) && sgxtex_is_pot(lh) ? sgxtex_twiddle(x, y, lw, lh)
							   : (size_t)y * sgxtex_pitch(lw) + x;

	return sgxtex_level_offset(w, h, bpp, lv) + i * bpp;
}

void sgxtex_encode_level(void *dst, const void *src, size_t src_stride, uint32_t w, uint32_t h,
			 uint32_t bpp)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	if (!(sgxtex_is_pot(w) && sgxtex_is_pot(h))) {
		size_t p = (size_t)sgxtex_pitch(w) * bpp;

		for (uint32_t y = 0; y < h; y++) {
			memcpy(d + y * p, s + y * src_stride, (size_t)w * bpp);
			memset(d + y * p + (size_t)w * bpp, 0, p - (size_t)w * bpp);
		}
		return;
	}
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++)
			memcpy(d + (size_t)sgxtex_twiddle(x, y, w, h) * bpp, s + y * src_stride + (size_t)x * bpp, bpp);
}

uint16_t sgxtex_convert16(uint32_t t1fmt, uint16_t v)
{
	if (t1fmt == SGXTEX_FMT_RGBA4444)
		return (uint16_t)(v >> 4 | v << 12);
	if (t1fmt == SGXTEX_FMT_RGBA5551)
		return (uint16_t)(v >> 1 | v << 15);
	return v;
}

uint32_t sgxtex_t1(uint32_t fmt, uint32_t layout, uint32_t w, uint32_t h)
{
	return layout << 29 | (fmt & 0x1f) << 24 | (w - 1) << 12 | (h - 1);
}

uint32_t sgxtex_t0_stride(uint32_t pitch)
{
	uint32_t s = pitch - 1;

	return (s & 7) << 9 | (s >> 3) << 17;
}

void sgxpbe_words(struct sgxpbe *p, uint32_t w, uint32_t h, uint32_t pitch, uint32_t addr, uint32_t fmt,
		  int twiddled, int dither)
{
	p->w[0] = 0;
	p->w[1] = (h - 1) << 16 | (w - 1);
	p->w[2] = ((pitch - 1) >> 1) << 16 | 0x8000;
	p->w[3] = addr & ~3u;
	p->side = SGXPBE_SIDE_ALWAYS | (fmt & 0x3f) | (twiddled ? SGXPBE_SIDE_TWIDDLED : 0) |
		  (dither ? SGXPBE_SIDE_DITHER : 0);
}

/* LIMM rN, #v (libsrv_um FUN_000301d0) */
static void limm(uint32_t *o, uint32_t reg, uint32_t v)
{
	o[0] = (v & 0x1fffff) | reg << 21;
	o[1] = 0xfca00000 | (v >> 21 & 0x1f) << 4 | (v >> 26) << 12;
}

void sgxpbe_eot_program(uint32_t *o, const struct sgxpbe *p)
{
	o[0] = 0x00000000, o[1] = 0xfa440700;	/* phas.i #0, #0 */
	for (uint32_t r = 0; r < 4; r++)
		limm(o + 2 + 2 * r, r, p->w[r]);
	o[10] = 0x00200080, o[11] = 0xfb200100;	/* emitpix: the four PBE words */
	o[12] = (p->side & 0x3f) << 22 | 0x00208180;
	o[13] = 0xfb240000 | (p->side & 0x3000) << 10 | (p->side >> 3 & 0x1f8);
}

const uint32_t sgxtex_copy_shader[4] = { 0x00000000, 0xfa440700, 0xa0000000, 0x28851001 };

void sgxtex_blit_command(uint32_t *c, const struct sgxtex_blit_ctx *x, uint32_t w, uint32_t h)
{
	uint32_t base = x->bg & 0xfff00000;

	memset(c, 0, SGXTEX_BLIT_CMD_WORDS * 4);
	c[0] = 1u << 25 | (x->bg - base) >> 4;	/* background-object block */
	c[1] = 0x200;
	c[2] = 0x3f800000;			/* background depth 1.0 */
	c[3] = 3;
	c[5] = 0x100;
	c[6] = base;
	c[7] = 0x80000000;
	c[8] = 0x88;
	c[9] = x->pds_event & ~0xfu;		/* pixel-event program, 5 rows, 1 code unit */
	c[10] = 5;
	c[11] = 1;
	c[12] = 0;				/* first tile */
	c[13] = (((w + 31) >> 5) - 1) << 16 | (((h + 15) >> 4) - 1);	/* last tile */
	c[14] = 0x80;
	c[15] = PDS_BASE;
}

void sgxtex_blit_bg(uint32_t *b, const struct sgxtex_blit_ctx *x, uint32_t w, uint32_t h)
{
	memset(b, 0, SGXTEX_BLIT_BG_WORDS * 4);
	b[0] = 1u << 26 | (x->pds_secondary - PDS_BASE) >> 4;
	b[1] = 0x00030001;
	b[2] = 3u << 26 | (x->pds_primary - PDS_BASE) >> 4;
	b[5] = b[8] = 0x3f800000;
	b[10] = 0x202;
	b[15] = 0x100000;
	b[16] = 0x1000;
	b[18] = ((w + 0x1000) * 0x100) << 8 | 0x10;
	b[20] = (h + 0x1000) * 0x100;
	b[21] = 0x1000;
}

void sgxtex_blit_pixel_program(uint32_t *p, const struct sgxtex_blit_ctx *x, uint32_t src, uint32_t w,
			       uint32_t h, uint32_t src_pitch)
{
	/* data rows (DS0 DS0 DS1 DS1): DOUTU, DOUTI, DOUTT T2 T3, DOUTT T0 T1; then the code */
	static const uint32_t code[4] = { 0x07000305, 0x07040202, 0x07082364, 0xaf000000 };

	memset(p, 0, SGXTEX_BLIT_PDS_WORDS * 4);
	p[0] = ((x->copy_use - USE_BASE) >> 3) << 4;
	p[1] = 0x02000004;
	p[4] = 0x40012001;
	p[6] = src;
	p[7] = 0xa0800000;
	p[8] = sgxtex_t0_stride(src_pitch);
	p[9] = sgxtex_t1(SGXTEX_FMT_BGRA8888, SGXTEX_LAYOUT_STRIDED, w, h);
	memcpy(p + 12, code, sizeof code);
}
