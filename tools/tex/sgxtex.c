/* sgxtex: see sgxtex.h and docs/TEX.md */
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

/* ---- general blits ---- */

uint32_t sgxtex_doutu(uint32_t use_addr, uint32_t temps)
{
	uint32_t off = use_addr - USE_BASE;

	return ((temps + 3) >> 2) << 24 | off >> 19 | ((off & 0x7ffff) >> 3) << 4;
}

void sgxtex_blit_source(uint32_t tex[4], const struct sgxtex_surface *src, int filter)
{
	/* T0: clamp S and T, no mipmaps (b17-20 = 0xf); min/mag linear at b10/b12 */
	uint32_t t0 = 0x001e0090 | (filter == SGXTEX_FILTER_LINEAR ? 0x1400 : 0);

	if (src->layout == SGXTEX_LAYOUT_STRIDED)	/* the pitch takes b9-11 and b17-26 */
		t0 = sgxtex_t0_stride(src->pitch) | (filter == SGXTEX_FILTER_LINEAR ? 0x1000 : 0);
	tex[0] = t0;
	tex[1] = sgxtex_t1(src->t1fmt, src->layout, src->w, src->h);
	tex[2] = src->addr;
	tex[3] = 0xa0800000;
}

static void pixel_program_tex(uint32_t *p, const struct sgxtex_blit_ctx *x, const uint32_t tex[4])
{
	static const uint32_t code[4] = { 0x07000305, 0x07040202, 0x07082364, 0xaf000000 };

	memset(p, 0, SGXTEX_BLIT_PDS_WORDS * 4);
	p[0] = sgxtex_doutu(x->copy_use, 0);
	p[1] = 0x02000004;
	p[4] = 0x40012001;	/* DOUTI: the texture coordinate */
	p[6] = tex[2];
	p[7] = tex[3];
	p[8] = tex[0];
	p[9] = tex[1];
	memcpy(p + 12, code, sizeof code);
}

void sgxtex_blit_build(struct sgxtex_blit *b, const struct sgxtex_blit_ctx *c, const struct sgxtex_surface *src,
		       const struct sgxtex_surface *dst, int filter)
{
	uint32_t tex[4];
	struct sgxpbe p;
	int tw = dst->layout == SGXTEX_LAYOUT_TWIDDLED;

	sgxtex_blit_command(b->cmd, c, dst->w, dst->h);
	sgxtex_blit_bg(b->bg, c, dst->w, dst->h);
	sgxtex_blit_source(tex, src, filter);
	pixel_program_tex(b->pds, c, tex);
	sgxpbe_words(&p, dst->w, dst->h, tw ? dst->w : dst->pitch, dst->addr, dst->pbefmt, tw, 0);
	sgxpbe_eot_program(b->eot, &p);
}

static uint32_t f32(float f)
{
	uint32_t u;

	memcpy(&u, &f, 4);
	return u;
}

int sgxtex_blit_build_rect(struct sgxtex_blit *b, const struct sgxtex_blit_ctx *c, const struct sgxtex_surface *src,
			    const struct sgxtex_rect *sr, const struct sgxtex_surface *dst, const struct sgxtex_rect *dr,
			    int filter)
{
	/* texel coordinate per destination pixel: s = sx + (x - dx) * sw / dw (same in y) */
	float ku = (float)sr->w / dr->w, kv = (float)sr->h / dr->h;
	float u0 = (sr->x - dr->x * ku) / src->w, v0 = (sr->y - dr->y * kv) / src->h;
	float u1 = u0 + dst->w * ku / src->w, v2 = v0 + dst->h * kv / src->h;
	int tw = dst->layout == SGXTEX_LAYOUT_TWIDDLED;
	uint32_t x1 = dr->x + dr->w - 1, y1 = dr->y + dr->h - 1;
	struct sgxpbe p;

	if (tw && (dr->x || dr->w != dst->w))
		return -1;
	sgxtex_blit_build(b, c, src, dst, filter);
	/* vertex texture coordinates: (0,0) -> (u0, v0), (W,0) -> (u1, v0), (0,H) -> (u0, v2) */
	b->bg[3] = f32(u0), b->bg[4] = f32(v0);
	b->bg[5] = f32(u1), b->bg[6] = f32(v0);
	b->bg[7] = f32(u0), b->bg[8] = f32(v2);
	/* tiles the rectangle touches (32x16) */
	b->cmd[12] = (dr->x >> 5) << 16 | dr->y >> 4;
	b->cmd[13] = (x1 >> 5) << 16 | y1 >> 4;
	sgxpbe_words(&p, dst->w, dst->h, tw ? dst->w : dst->pitch, dst->addr, dst->pbefmt, tw, 0);
	p.w[0] = dr->y << 16 | dr->x;
	p.w[1] = y1 << 16 | x1;
	sgxpbe_eot_program(b->eot, &p);
	return 0;
}

void sgxtex_blit_command_full(uint32_t out[SGXTEX_CMD_BYTES / 4], const struct sgxtex_blit *b, int twiddled_dst)
{
	memset(out, 0, SGXTEX_CMD_BYTES);
	memcpy(out, b->cmd, sizeof b->cmd);
	out[0xa4 / 4] = SGXTEX_CMD_BYTES;
	out[0xa8 / 4] = twiddled_dst ? 8 : 0;	/* as the DDK's uploads (8) and swaps (0) */
	out[0x194 / 4] = 0x07c00000;
}

/* ---- render to texture ---- */

void sgxtex_rt_surface(struct sgxtex_surface *s, uint32_t addr, uint32_t w, uint32_t h, uint32_t t1fmt,
		       uint32_t pbefmt)
{
	int tw = sgxtex_is_pot(w) && sgxtex_is_pot(h);

	s->addr = addr;
	s->w = w;
	s->h = h;
	s->layout = tw ? SGXTEX_LAYOUT_TWIDDLED : SGXTEX_LAYOUT_LINEAR;
	s->pitch = tw ? w : sgxtex_pitch(w);
	s->t1fmt = t1fmt;
	s->pbefmt = pbefmt;
}

void sgxtex_rt_pbe(struct sgxpbe *p, const struct sgxtex_surface *s, int dither)
{
	sgxpbe_words(p, s->w, s->h, s->pitch, s->addr, s->pbefmt, s->layout == SGXTEX_LAYOUT_TWIDDLED, dither);
}

size_t sgxtex_rt_bytes(const struct sgxtex_surface *s, uint32_t bpp)
{
	return (size_t)s->pitch * s->h * bpp;
}

/* ---- the transfer context's own buffers ---- */

void sgxtex_xctx_words(uint32_t ctx[SGXTEX_XCTX_BYTES / 4], uint32_t ccb, uint32_t ccb_ctl, uint32_t pid)
{
	memset(ctx, 0, SGXTEX_XCTX_BYTES);
	ctx[0] = 1;		/* flags */
	/* ctx[2]: the page directory, filled in by the kernel (PD offset 8) */
	ctx[3] = ccb;
	ctx[4] = ccb_ctl;
	ctx[0x2a] = pid;
}

void sgxtex_xctx_secondary(uint32_t *p, uint32_t nop_use)
{
	memset(p, 0, SGXTEX_SECONDARY_WORDS * 4);
	p[0] = sgxtex_doutu(nop_use, 0);
	p[1] = 0x00000004;
	p[4] = 0x07000305;	/* DOUTU ds0[0..1] */
	p[5] = 0xaf000000;	/* HALT */
}

void sgxtex_xctx_pixel_event(uint32_t *p, uint32_t eot_use, uint32_t emit_use)
{
	/* data rows DS0 DS0 DS1 DS1; the code is the 29 instructions PDS.md decodes */
	static const uint32_t code[29] = {
		0xcf820870, 0x8700c100, 0x90000005, 0x07000205, 0xaf000000, 0xcf820c70, 0x8700c100, 0x90000018,
		0xdf820070, 0xcf08c070, 0x8700c100, 0x9000001b, 0x07840406, 0xf7800370, 0xcf0ac070, 0xc70cc030,
		0xf7800370, 0xcf0ec070, 0xc760c030, 0xf7811470, 0xcf10c070, 0xc760c030, 0x07601805, 0xaf000000,
		0xcf821070, 0x8700c100, 0x9000001c, 0x07040205, 0xaf000000,
	};
	const uint32_t eot = sgxtex_doutu(eot_use, 4), emit = sgxtex_doutu(emit_use, 4);
	const uint32_t data[20] = {
		eot, 0x00800000, 0, eot,
		emit, 0x00c00000, 8, 2,
		0x100, 0xff8, 4, 0,
		0x00800000, 0x003ff000, 0, 0,
		0x00400000, 0, 0, 0,
	};

	memcpy(p, data, sizeof data);
	memcpy(p + 20, code, sizeof code);
}

const uint32_t sgxtex_nop_shader[4] = { 0x00000000, 0xfa440700, 0x00000000, 0xf8040140 };
const uint32_t sgxtex_emit_shader[6] = { 0x00000000, 0xfa440700, 0x00000000, 0xfca00000, 0x80200000, 0xfb260040 };
