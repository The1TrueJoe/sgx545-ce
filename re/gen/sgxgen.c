/*
 * sgxgen: SGX545 frames from code alone. See sgxgen.h and re/gen/GEN.md.
 *
 * Tags in the comments follow STATE.md: [C] proven by a replay, [D] from a
 * diff or the DDK code that writes it, [G] guess (the value the DDK uses,
 * meaning unknown).
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sgxgen.h"
#include "usse_progs.h"

/* SGX heap ids (low byte of ui32HeapID) */
enum { H_GENERAL = 0, H_TADATA = 1, H_KDATA = 3, H_PIXSHADER = 4, H_VTXSHADER = 5, H_PDSPIXEL = 6,
       H_PDSVERTEX = 7, H_SYNC = 8, H_3DPARAMS = 10 };

/* Placement: every PDS program the DDK makes starts on 32 bytes, every USSE program
 * GL allocates on 128, every DMA source on 32 [G: what the hardware needs is untested] */
#define PDS_ALIGN 0x40
#define USSE_ALIGN 0x80
#define DMA_ALIGN 0x20

#define die(...) do { fprintf(stderr, "sgxgen: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)

static uint32_t f32(float x)
{
	uint32_t u;

	memcpy(&u, &x, 4);
	return u;
}

/* float -> IEEE half, round to nearest even (finite inputs) */
static uint32_t f16(float x)
{
	uint32_t u = f32(x), s = (u >> 16) & 0x8000, m = u & 0x7fffff;
	int e = (int)((u >> 23) & 0xff) - 127 + 15;

	if (e >= 31)
		return s | 0x7c00;
	if (e <= 0) {
		if (e < -10)
			return s;
		m |= 0x800000;
		uint32_t sh = (uint32_t)(14 - e), r = m >> sh, rem = m & ((1u << sh) - 1), half = 1u << (sh - 1);

		if (rem > half || (rem == half && (r & 1)))
			r++;
		return s | r;
	}
	uint32_t r = (uint32_t)e << 10 | m >> 13, rem = m & 0x1fff;

	if (rem > 0x1000 || (rem == 0x1000 && (r & 1)))
		r++;
	return s | r;
}

static uint32_t u8(float x)
{
	return x <= 0 ? 0 : x >= 1 ? 255 : (uint32_t)lrintf(x * 255.0f);
}

/* ------------------------------------------------------------------ memory */

static void bo_alloc(struct sgx_dev *d, struct sgx_bo *bo, uint32_t heap, uint32_t attribs, uint32_t size,
		     uint32_t align)
{
	int e = pvr_alloc(&d->pvr, &bo->b, heap, attribs, size, align);

	if (e)
		die("ALLOC_DEVICEMEM heap %u size %#x failed: %d", heap, size, e);
	if ((e = pvr_map(&d->pvr, &bo->b)))
		die("map of %#x failed: %d", bo->b.devva, e);
	bo->va = bo->b.devva;
	bo->map = bo->b.cpu;
	memset(bo->map, 0, size);
	if (d->verbose)
		fprintf(stderr, "sgxgen: heap %2u %#010x +%#x\n", heap, bo->va, size);
}

static void ring_init(struct sgx_dev *d, struct sgx_ring *r, uint32_t heap, uint32_t size)
{
	bo_alloc(d, &r->bo, heap, 0x1, size, 0x1000);
	/* nothing at a heap's first byte: a PDS state word or DOUTU word of 0 would name it */
	r->used = 0x100;
}

static struct sgx_ptr ring_alloc(struct sgx_ring *r, uint32_t size, uint32_t align)
{
	struct sgx_ptr p;
	uint32_t o = (r->used + align - 1) & ~(align - 1);

	if (o + size > r->bo.b.size)
		die("ring at %#x full (%#x + %#x)", r->bo.va, o, size);
	r->used = o + size;
	p.va = r->bo.va + o;
	p.p = r->bo.map + o / 4;
	return p;
}

/* ------------------------------------------------------------------ USSE */

/* Load a program assembled by mkusse.py; phas.i targets are relocated to the
 * program's place in its code heap (ISA.md: pc is absolute, in instructions). */
static uint32_t usse_load(struct sgx_ctx *c, const struct usse_prog *pr, int vertex)
{
	struct sgx_ring *r = vertex ? &c->usse_vtx : &c->usse_pix;
	struct sgx_ptr p = ring_alloc(r, pr->n * 8, USSE_ALIGN);
	uint32_t pc0 = (p.va - c->dev->heap_base[vertex ? H_VTXSHADER : H_PIXSHADER]) / 8;

	memcpy(p.p, pr->w, pr->n * 8);
	for (unsigned i = 0; i < pr->nreloc; i++)
		p.p[2 * pr->reloc[i]] += pc0;
	return p.va;
}

/* DOUTU word 0 (PDS.md): code base register (pixel 0-9 at the pixel heap +
 * n*512K, vertex 10+), offset/8 in that window at b4, temporaries in fours at b24 */
static uint32_t doutu_w0(struct sgx_ctx *c, uint32_t va, int vertex, uint32_t temps)
{
	uint32_t off = va - c->dev->heap_base[vertex ? H_VTXSHADER : H_PIXSHADER];

	return ((temps + 3) >> 2) << 24 | ((off >> 19) + (vertex ? 10 : 0)) | ((off & 0x7ffff) >> 3) << 4;
}

/* LIMM rN, #v (STATE.md, from the DDK's end-of-tile generator) */
static void usse_limm(uint32_t *w, uint32_t v, uint32_t reg)
{
	w[0] = (v & 0x1fffff) | reg << 21;
	w[1] = 0xfca00000 | ((v >> 21) & 0x1f) << 4 | (v >> 26) << 12;
}

/* ------------------------------------------------------------------ PDS */

struct pds {
	uint32_t ds0[64], ds1[64];
	unsigned n0, n1;		/* words used in each data store */
	uint32_t code[64];
	unsigned nc;
};

/* MOVS (DOUT): src1 = DS0 pair row, src2 = DS1 pair row; selects 0 s1.lo, 1 s1.hi, 2 s2.lo, 3 s2.hi */
#define MOVS(top, r1, r2, a, b, c_, d_, dest) \
	((uint32_t)(top) << 24 | (uint32_t)(r1) << 18 | (uint32_t)(r2) << 13 | (a) << 11 | (b) << 9 | (c_) << 7 | \
	 (d_) << 5 | (dest))
#define DOUTI 2
#define DOUTD 3
#define DOUTT 4
#define DOUTU 5
#define DOUTA 6
#define MV 0x07			/* always */
#define MV_X 0x2f		/* .x2.s27: DOUTD adds word 2 to its address */
#define MV_CC3 0x03		/* the vertex-fetch DOUTU form */
/* ALU: op, sub-op, src1 (S1: 16-bit units, DS0 then inputs), src2 (S2: DS1, 16-bit units), dst (U) */
#define ALU(op, s, s1, s2, dst) (0x07000000u | (uint32_t)(op) << 28 | (uint32_t)(s) << 27 | (s1) << 16 | (s2) << 9 | (dst))
#define S1_DS0(w) (2 * (w))
#define S1_IN(n) (2 * (0x40 + (n)))
#define S2_DS1(w) (2 * (w))
#define U_DS0(w) (w)
#define U_DS1(w) (0x40 + (w))
#define U_IN(n) (0x80 + (n))
#define OP_ADD 4
#define OP_MUL16 6
#define OP_ANDOR 0xc		/* s 1 AND, s 0 OR */
#define OP_XOR 0xd
#define SHL(src, n, dst) (0xf7000000u | (src) << 16 | (n) << 8 | (dst))
#define TST1(s1) (0x87000000u | (s1) << 16)			/* p0 = src1 == 0 */
#define TST2(s2) (0x87000000u | (s2) << 9 | 0x100)		/* p0 = src2 == 0 */
#define BRA_P0(t) (0x90000000u | (t))
#define HALT 0xaf000000u

static void pds_d0(struct pds *p, unsigned i, uint32_t v)
{
	p->ds0[i] = v;
	if (i + 1 > p->n0)
		p->n0 = i + 1;
}

static void pds_d1(struct pds *p, unsigned i, uint32_t v)
{
	p->ds1[i] = v;
	if (i + 1 > p->n1)
		p->n1 = i + 1;
}

/* Lay a program out as 16-byte rows (DS0 DS0 DS1 DS1), code after. */
static uint32_t pds_emit(struct sgx_ring *r, const struct pds *p, uint32_t *rows_out)
{
	uint32_t rows = ((p->n0 > p->n1 ? p->n0 : p->n1) + 1) / 2;
	struct sgx_ptr m = ring_alloc(r, rows * 16 + p->nc * 4, PDS_ALIGN);

	for (uint32_t i = 0; i < rows; i++) {
		m.p[4 * i + 0] = p->ds0[2 * i];
		m.p[4 * i + 1] = p->ds0[2 * i + 1];
		m.p[4 * i + 2] = p->ds1[2 * i];
		m.p[4 * i + 3] = p->ds1[2 * i + 1];
	}
	memcpy(m.p + 4 * rows, p->code, p->nc * 4);
	if (rows_out)
		*rows_out = rows;
	return m.va;
}

#define CODE(p, w) ((p)->code[(p)->nc++] = (w))

/* TA/3D state word for a pixel PDS program: rows<<26 | offset in the PDS pixel heap >> 4 */
static uint32_t pds_word(struct sgx_ctx *c, uint32_t va, uint32_t rows)
{
	return rows << 26 | (va - c->dev->heap_base[H_PDSPIXEL]) >> 4;
}

/* pixel program: DOUTU (+ one DOUTI) -- the DDK's 0x1e970 layout for 0 or 1 iterated input */
static uint32_t pds_pixel(struct sgx_ctx *c, uint32_t task_w0, uint32_t task_w1, uint32_t douti, uint32_t *rows)
{
	struct pds p = { 0 };

	pds_d0(&p, 0, task_w0);
	pds_d0(&p, 1, task_w1);
	pds_d1(&p, 0, 0);
	pds_d1(&p, 1, douti);
	CODE(&p, MOVS(MV, 0, 0, 0, 1, 2, 3, DOUTU));
	if (douti) {
		pds_d0(&p, 2, 0);
		pds_d0(&p, 3, 0);
		CODE(&p, MOVS(MV, 1, 0, 3, 0, 1, 1, DOUTI));
	}
	CODE(&p, HALT);
	return pds_emit(&c->pds_pix, &p, rows);
}

/* secondary/state program (0x1ed20): an optional DOUTD of n dwords, then a task */
static uint32_t pds_secondary(struct sgx_ring *r, uint32_t src, uint32_t dma_ctrl, uint32_t task_w0,
			      uint32_t task_w1, uint32_t *rows)
{
	struct pds p = { 0 };

	if (src) {
		pds_d0(&p, 0, src);
		pds_d0(&p, 1, dma_ctrl);
		pds_d1(&p, 0, 0);
		pds_d0(&p, 2, task_w0);
		pds_d0(&p, 3, task_w1);
		CODE(&p, MOVS(MV, 0, 0x18, 0, 1, 0, 1, DOUTD));
		CODE(&p, MOVS(MV, 1, 0, 0, 1, 2, 2, DOUTU));
	} else {
		pds_d0(&p, 0, task_w0);
		pds_d0(&p, 1, task_w1);
		CODE(&p, MOVS(MV, 0, 0, 0, 1, 2, 2, DOUTU));
	}
	CODE(&p, HALT);
	return pds_emit(r, &p, rows);
}

/* DOUTD control: dwords-1, attribute register at b8, b22-24 [G], b25 = secondary attributes [G] */
static uint32_t dma_ctrl(uint32_t dwords, uint32_t reg, uint32_t kind, int secondary)
{
	return (dwords - 1) | reg << 8 | kind << 22 | (secondary ? 0x2000000 : 0);
}

/* ------------------------------------------------------------------ device */

int sgx_dev_open(struct sgx_dev *d, const char *node, const char *trace)
{
	int e;

	memset(d, 0, sizeof *d);
	if ((e = pvr_open(&d->pvr, node)))
		return e;
	if (trace && (e = pvr_trace_open(&d->pvr, trace)))
		return e;
	if ((e = pvr_connect(&d->pvr)) || (e = pvr_acquire(&d->pvr, 0, 0)) || (e = pvr_memctx(&d->pvr)) ||
	    (e = pvr_heapinfo(&d->pvr)))
		return e;
	for (unsigned i = 0; i < d->pvr.nheaps; i++)
		d->heap_base[d->pvr.heaps[i].ui32HeapID & 0xf] = d->pvr.heaps[i].sDevVAddrBase.uiAddr;
	return 0;
}

void sgx_dev_close(struct sgx_dev *d)
{
	pvr_trace_close(&d->pvr);
}

/* ------------------------------------------------------------------ context */

/* The parameter buffer (FUN_0001e980; STATE.md section 6). */
static void ctx_pb(struct sgx_ctx *c, uint32_t bytes)
{
	struct sgx_dev *d = c->dev;
	uint32_t pages = (bytes + 0x22000) >> 12, first, last, *fl, *pd;

	bo_alloc(d, &c->pbdesc, H_KDATA, 0xf, 0x3c, 0x20);
	bo_alloc(d, &c->pbblocks, H_KDATA, 0xf, 0x10, 4);
	bo_alloc(d, &c->pb, H_3DPARAMS, 0xb, pages << 12, 0x1000);
	first = (c->pb.va - d->heap_base[H_3DPARAMS]) >> 12;
	last = first + pages - 1;
	/* the free list is indexed by page number from the heap base: prev<<16 | next */
	/* the DDK always allocates 0x40000: one u32 per page of the whole 256 MB heap */
	bo_alloc(d, &c->freelist, H_3DPARAMS, 0xb, (last + 1) * 4 > 0x40000 ? (last + 1) * 4 : 0x40000, 0x10);
	fl = c->freelist.map;
	for (uint32_t i = first; i <= last; i++)
		fl[i] = (i == first ? 0xffffu : i - 1) << 16 | (i == last ? 0xffffu : i + 1);
	/* block list: {pages, first | last<<16, PB descriptor, 0} */
	c->pbblocks.map[0] = pages;
	c->pbblocks.map[1] = first | last << 16;
	c->pbblocks.map[2] = c->pbdesc.va;
	pd = c->pbdesc.map;
	uint32_t zls = (pages - 0x22) & 0xfffff, ta = pages + 0x10, pds = zls < ta ? zls : ta;

	pd[1] = pages;
	pd[2] = last << 16 | first;		/* free list head */
	pd[3] = (last - 1) << 16;
	pd[4] = c->freelist.va;
	pd[6] = pd[2];
	pd[7] = pd[3];
	pd[10] = ta;				/* TA threshold */
	pd[11] = zls;				/* ZLS threshold */
	pd[12] = zls;				/* global threshold */
	pd[13] = pds > 0x100 ? pds - 0x100 : 0;	/* PDS threshold */
	pd[14] = c->pbblocks.va;
	c->pb_pages = pages;
}

struct sgx_ctx *sgx_ctx_create(struct sgx_dev *d)
{
	struct sgx_ctx *c = calloc(1, sizeof *c);
	struct sgx_bo dummy;
	uint32_t hw[17] = { 0 }, rows;
	struct sgx_ptr p;
	struct pds t = { 0 };
	int e;

	c->dev = d;
	ctx_pb(c, 0x400000);
	bo_alloc(d, &c->ccb, H_KDATA, 0x4f, 0x10420, 0x1000);
	bo_alloc(d, &c->ccbctl, H_KDATA, 0x4f, 8, 0x20);
	bo_alloc(d, &dummy, H_SYNC, 0x3, 4, 0x10);		/* SGXCreateRenderContext's own sync word */
	bo_alloc(d, &c->scratch, H_3DPARAMS, 0xb, 0x40, 0x40);
	bo_alloc(d, &dummy, H_SYNC, 0x4b, 0x80, 0x20);		/* and its 0x80-byte buffer for GL */
	/* The kernel-data and sync allocations of the DDK's two transfer contexts, which we
	 * don't use: they only keep every later structure at the address the DDK gives it.
	 * The microkernel keeps pointers to the previous client's render details and RT
	 * data and reads them when the next client's TA starts; after a DDK client, a
	 * frame with any other layout hangs the TA (re/gen/GEN.md, "Layout"). */
	for (int i = 0; i < 2; i++) {
		bo_alloc(d, &dummy, H_KDATA, 0x4f, 0x104c8, 0x1000);
		bo_alloc(d, &dummy, H_KDATA, 0x4f, 8, 0x20);
		bo_alloc(d, &dummy, H_SYNC, 0x3, 4, 0x10);
		bo_alloc(d, i ? &dummy : &c->sync3d, H_SYNC, 0x3, 4, 0x10);	/* the DDK's 3D sync */
		bo_alloc(d, &dummy, H_KDATA, 0x4f, 4, 4);
	}

	/* HW render context (SGXCreateRenderContext): the kernel fills the PD at word 2 */
	hw[0] = 0x21;			/* b0, b5 = per-context PB [D] */
	hw[3] = c->ccb.va;
	hw[4] = c->ccbctl.va;
	hw[6] = c->pbdesc.va;
	hw[7] = c->scratch.va;
	hw[16] = (uint32_t)getpid();
	if ((e = pvr_register_render_ctx(&d->pvr, hw, sizeof hw, 8, &c->hwctx, &c->hwctx_va)))
		die("SGX_REGISTER_HW_RENDER_CONTEXT failed: %d", e);

	ring_init(d, &c->usse_pix, H_PIXSHADER, 0x8000);
	ring_init(d, &c->usse_vtx, H_VTXSHADER, 0x8000);
	ring_init(d, &c->pds_pix, H_PDSPIXEL, 0x10000);
	ring_init(d, &c->pds_vtx, H_PDSVERTEX, 0x10000);
	ring_init(d, &c->gen, H_GENERAL, 0x40000);
	ring_init(d, &c->vdm, H_GENERAL, 0x5000);

	/* fixed USSE programs */
	c->empty_v = usse_load(c, &usse_empty, 1);
	c->empty_p = usse_load(c, &usse_empty, 0);
	c->copy_fs = usse_load(c, &usse_copy_fs, 0);
	c->clear_vs = usse_load(c, &usse_clear_vs, 1);
	c->pe_ptoff = usse_load(c, &usse_pe_ptoff, 0);
	c->pe_emit = usse_load(c, &usse_pe_emit, 0);
	for (int n = 1; n <= 32; n++)
		c->state_emit[n] = 0;	/* loaded on first use */

	/* VDM state programs that every frame ends with */
	c->empty_state = pds_secondary(&c->pds_vtx, 0, 0, doutu_w0(c, c->empty_v, 1, 0), 0, NULL);
	/* TA terminate (VDM type 6, FUN_0001fd60): DOUTA (0x2000, 0), 3 x (0x16000e, 0x100), then
	 * a state-emit task for the two attributes [D; the values' meaning G] */
	c->state_emit[2] = usse_load(c, &usse_state_emit_2, 1);
	pds_d0(&t, 0, 0x2000);
	pds_d0(&t, 1, 0);
	pds_d0(&t, 2, 0x0016000e);
	pds_d0(&t, 3, 0x100);
	pds_d0(&t, 4, doutu_w0(c, c->state_emit[2], 1, 0));
	pds_d0(&t, 5, 0);
	CODE(&t, MOVS(MV, 0, 0x18, 0, 1, 2, 2, DOUTA));
	for (int i = 0; i < 3; i++)
		CODE(&t, MOVS(MV, 1, 0x18, 0, 1, 2, 2, DOUTA));
	CODE(&t, MOVS(MV, 2, 0, 0, 1, 2, 2, DOUTU));
	CODE(&t, HALT);
	c->terminate = pds_emit(&c->pds_vtx, &t, &rows);

	/* pixel programs shared by every object without inputs, and the clear's */
	c->pds_empty_p = pds_secondary(&c->pds_pix, 0, 0, doutu_w0(c, c->empty_p, 0, 0), 0, NULL);
	/* DOUTI: iterate the colour (source 0x20 at b11), 4 components, plain iteration, last [D] */
	c->clear_pds = pds_pixel(c, doutu_w0(c, c->copy_fs, 0, 0), 0x01000004,
				 0x20u << 11 | 3u << 22 | 1u << 24 | 1u << 29, NULL);

	/* identity index buffer: glDrawArrays draws through it (STATE.md section 2) */
	p = ring_alloc(&c->gen, 1024 * 2, 4);
	for (int i = 0; i < 1024; i += 2)
		p.p[i / 2] = (uint32_t)i | (uint32_t)(i + 1) << 16;
	c->idx = p.va;
	return c;
}

/* ------------------------------------------------------------------ render target */

/* SGXAddRenderTarget's numbers (STATE.md section 6; re/state/build.py rt_numbers) */
static void rt_numbers(struct sgx_rt_numbers *r, uint32_t w, uint32_t h)
{
	uint32_t n, big, m;

	r->xt = (w + 31) >> 5;
	r->yt = (h + 15) >> 4;
	if (w * h < 0x4b000) {
		r->mode = 0;
		r->mx = (((r->xt + 1) >> 1) + 3) & ~3u;
		r->my = (((r->yt + 1) >> 1) + 3) & ~3u;
		r->mtile1 = r->mx << 22 | r->mx << 12 | r->mx;
		r->mtile2 = r->my << 22 | r->my << 12 | r->my;
		n = 2;
	} else {
		r->mode = 1;
		r->mx = (((r->xt + 3) >> 2) + 3) & ~3u;
		r->my = (((r->yt + 3) >> 2) + 3) & ~3u;
		r->mtile1 = 1u << 31 | r->mx << 22 | 2 * r->mx << 12 | 3 * r->mx;
		r->mtile2 = r->my << 22 | 2 * r->my << 12 | 3 * r->my;
		n = 4;
	}
	r->padx = r->mx * n;
	r->pady = r->my * n;
	r->te_screen = (r->yt - 1) << 12 | (r->xt - 1);
	r->mt_tiles = r->mx * r->my;
	r->region_bytes = r->padx * r->pady * 12;
	m = (r->padx > r->pady ? r->padx : r->pady) - 1;
	for (big = 1; m; m >>= 1)
		big <<= 1;
	r->tailptr_bytes = big * big * 4;
}

/* The end-of-tile program (FUN_000301d0): LIMM r0-r3 = the four PBE words, EMITPIX them,
 * EMITPIX the sideband word (STATE.md section 4) */
static uint32_t eot_program(struct sgx_ctx *c, struct sgx_surface *s)
{
	struct sgx_ptr p = ring_alloc(&c->usse_pix, 7 * 8, USSE_ALIGN);
	uint32_t pbe[4], side;

	pbe[0] = 0;					/* clip min y<<16 | x [D] */
	pbe[1] = (s->h - 1) << 16 | (s->w - 1);		/* clip max [C] */
	pbe[2] = ((s->stride - 1) >> 1) << 16 | 0x8000;	/* stride/2-1 [C]; b15: not the scanout buffer [D] */
	pbe[3] = s->color.va & ~3u;			/* address [D] */
	side = 0x200 | 0 /* BGRA8888 */ | 0x400 /* dither */;
	p.p[0] = 0;					/* phas.i #0, #0 [wait=7] */
	p.p[1] = 0xfa440700;
	for (uint32_t r = 0; r < 4; r++)
		usse_limm(p.p + 2 + 2 * r, pbe[r], r);
	p.p[10] = 0x00200080;				/* emit the four PBE words */
	p.p[11] = 0xfb200100;
	p.p[12] = (side & 0x3f) << 22 | 0x00208180;	/* emit the sideband word, end */
	p.p[13] = 0xfb240000 | (side & 0x3000) << 10 | ((side >> 3) & 0x1f8);
	return p.va;
}

/* The pixel event program (libsrv_um's static template, PDS.md "pixel-event program decoded"):
 *   in1 & 8 -> task ds0[0];  in1 & 2 -> (in1 ^ ds1[0]) & 0x100 ? DOUTA + end-of-tile task : task ds0[2];
 *   in1 & 4 -> task ds0[2]. */
static uint32_t pixel_event(struct sgx_ctx *c, uint32_t eot, uint32_t *rows)
{
	struct pds p = { 0 };

	pds_d0(&p, 0, doutu_w0(c, c->pe_ptoff, 0, 0));
	pds_d0(&p, 1, 0);
	pds_d1(&p, 0, 0);
	pds_d1(&p, 1, doutu_w0(c, eot, 0, 4));
	pds_d0(&p, 2, doutu_w0(c, c->pe_emit, 0, 4));
	pds_d0(&p, 3, 0x00c00000);
	pds_d1(&p, 2, 8);
	pds_d1(&p, 3, 2);
	pds_d0(&p, 4, 0x100);
	pds_d0(&p, 5, 0xff8);
	pds_d1(&p, 4, 4);
	pds_d1(&p, 5, 0);
	pds_d0(&p, 6, 0x00800000);
	pds_d0(&p, 7, 0x003ff000);
	pds_d1(&p, 6, 0);
	pds_d1(&p, 7, 0);
	pds_d0(&p, 8, 0x00400000);
	pds_d0(&p, 9, 0);
	pds_d1(&p, 8, 0);
	pds_d1(&p, 9, 0);
	/*  0 */ CODE(&p, ALU(OP_ANDOR, 1, S1_IN(1), S2_DS1(2), U_DS1(0x30)));
	/*  1 */ CODE(&p, TST2(S2_DS1(0x30)));
	/*  2 */ CODE(&p, BRA_P0(5));
	/*  3 */ CODE(&p, MOVS(MV, 0, 0, 0, 1, 0, 0, DOUTU));
	/*  4 */ CODE(&p, HALT);
	/*  5 */ CODE(&p, ALU(OP_ANDOR, 1, S1_IN(1), S2_DS1(3), U_DS1(0x30)));
	/*  6 */ CODE(&p, TST2(S2_DS1(0x30)));
	/*  7 */ CODE(&p, BRA_P0(24));
	/*  8 */ CODE(&p, ALU(OP_XOR, 1, S1_IN(1), S2_DS1(0), U_DS1(0x30)));
	/*  9 */ CODE(&p, ALU(OP_ANDOR, 1, S1_DS0(4), S2_DS1(0x30), U_DS1(0x30)));
	/* 10 */ CODE(&p, TST2(S2_DS1(0x30)));
	/* 11 */ CODE(&p, BRA_P0(27));
	/* 12 */ CODE(&p, MOVS(MV, 0x21, 0, 0, 2, 0, 0, DOUTA));	/* (in2, ds1[0]) */
	/* 13 */ CODE(&p, SHL(U_IN(0), 3, U_DS1(0x30)));
	/* 14 */ CODE(&p, ALU(OP_ANDOR, 1, S1_DS0(5), S2_DS1(0x30), U_DS1(0x30)));
	/* 15 */ CODE(&p, ALU(OP_ANDOR, 0, S1_DS0(6), S2_DS1(0x30), U_DS0(0x30)));
	/* 16 */ CODE(&p, SHL(U_IN(0), 3, U_DS1(0x30)));
	/* 17 */ CODE(&p, ALU(OP_ANDOR, 1, S1_DS0(7), S2_DS1(0x30), U_DS1(0x30)));
	/* 18 */ CODE(&p, ALU(OP_ANDOR, 0, S1_DS0(0x30), S2_DS1(0x30), U_DS0(0x30)));
	/* 19 */ CODE(&p, SHL(U_IN(1), 20, U_DS1(0x30)));
	/* 20 */ CODE(&p, ALU(OP_ANDOR, 1, S1_DS0(8), S2_DS1(0x30), U_DS1(0x30)));
	/* 21 */ CODE(&p, ALU(OP_ANDOR, 0, S1_DS0(0x30), S2_DS1(0x30), U_DS0(0x30)));
	/* 22 */ CODE(&p, MOVS(MV, 0x18, 0, 3, 0, 0, 0, DOUTU));	/* (ds1[1], ds0[0x30], ...) */
	/* 23 */ CODE(&p, HALT);
	/* 24 */ CODE(&p, ALU(OP_ANDOR, 1, S1_IN(1), S2_DS1(4), U_DS1(0x30)));
	/* 25 */ CODE(&p, TST2(S2_DS1(0x30)));
	/* 26 */ CODE(&p, BRA_P0(28));
	/* 27 */ CODE(&p, MOVS(MV, 1, 0, 0, 1, 0, 0, DOUTU));
	/* 28 */ CODE(&p, HALT);
	return pds_emit(&c->pds_pix, &p, rows);
}

/* The background object: every tile starts by loading the render target's old
 * contents (a sample of the surface as a strided texture) [D]. */
static void background(struct sgx_ctx *c, struct sgx_surface *s)
{
	struct pds p = { 0 };
	uint32_t rows, va;
	uint32_t *b = s->bgblock.map;

	pds_d0(&p, 0, doutu_w0(c, c->copy_fs, 0, 0));
	pds_d0(&p, 1, 0x02000004);		/* DOUTU w1: samples (b25), b2 */
	pds_d1(&p, 0, 0);
	/* DOUTI: sample texcoord set 0 (b0-4 = set+1), pure sample (b13, b16), last sample (b30) */
	pds_d1(&p, 1, 1u | 0x12000u | 1u << 30);
	pds_d0(&p, 2, 0);
	pds_d0(&p, 3, 0);
	/* DOUTT: T0 = stride/16-1 at b18 [G], 0x20e90 (clamp S/T b7/b4; the rest G);
	 * T1 = layout 6 (strided) | format 0x0c (BGRA8888) | w-1 | h-1; T2 = address; T3 [G] */
	pds_d1(&p, 2, (s->stride / 16 - 1) << 18 | 0x20e90);
	pds_d1(&p, 3, 6u << 29 | 0x0cu << 24 | (s->w - 1) << 12 | (s->h - 1));
	pds_d0(&p, 4, s->color.va);
	pds_d0(&p, 5, 0x00800000);
	CODE(&p, MOVS(MV, 0, 0, 0, 1, 2, 3, DOUTU));
	CODE(&p, MOVS(MV, 1, 0, 3, 0, 1, 1, DOUTI));
	CODE(&p, MOVS(MV, 2, 1, 2, 3, 0, 1, DOUTT));
	CODE(&p, HALT);
	va = pds_emit(&c->pds_pix, &p, &rows);
	s->bg_pds[0] = pds_word(c, c->pds_empty_p, 1);
	s->bg_pds[1] = 0x00030001;		/* [G] */
	s->bg_pds[2] = pds_word(c, va, rows);

	/* the background object's vertices in the PB (FUN_0001f990): fixed point, offset 0x1000 */
	b[5] = f32(1.0f);
	b[8] = f32(1.0f);
	b[10] = 0x202;
	b[15] = 0x100000;
	b[16] = 0x1000;
	b[17] = 0x3f80;
	b[18] = ((s->w + 0x1000) * 0x100) << 8 | 0x10;
	b[19] = f32(1.0f);
	b[20] = (s->h + 0x1000) * 0x100;
	b[21] = 0x1000;
	b[22] = 0x3f80;
}

struct sgx_surface *sgx_surface_create(struct sgx_ctx *c, uint32_t w, uint32_t h)
{
	struct sgx_dev *d = c->dev;
	struct sgx_surface *s = calloc(1, sizeof *s);
	struct sgx_rt_numbers *r = &s->rt;
	uint32_t *rd, *rt;

	s->w = w;
	s->h = h;
	s->stride = (w + 31) & ~31u;
	rt_numbers(r, w, h);
	bo_alloc(d, &s->color, H_GENERAL, 0x10000003, s->stride * h * 4, 0x1000);

	/* SGXAddRenderTarget: RT data (2 entries), tail pointers, region headers (double
	 * buffered), TA data blocks, the background object block */
	/* three render-details sets, as the DDK makes them; the third renders (see the
	 * transfer-context note in sgx_ctx_create: this keeps the DDK's addresses) */
	for (int i = 0; i < 3; i++) {
		bo_alloc(d, &s->rd, H_KDATA, 0xf, 0x1ec, 0x20);
		bo_alloc(d, &s->rd_sync, H_SYNC, 0xf, 8, 4);
		bo_alloc(d, &s->rd_w8, H_KDATA, 0xf, 8, 0x20);
		bo_alloc(d, &s->rd_w4, H_KDATA, 0xf, 4, 0x20);
		bo_alloc(d, &s->hwsync, H_KDATA, 0xf, 0x28, 0x20);
		bo_alloc(d, &s->hwsync_access, H_SYNC, 0xf, 8, 4);
	}
	bo_alloc(d, &s->rtword, H_KDATA, 0x4f, 4, 0x20);
	bo_alloc(d, &s->rtdata, H_KDATA, 0x4f, 2 * 0x38 + 0x18, 0x20);
	bo_alloc(d, &s->tailptr, H_TADATA, 0xb, r->tailptr_bytes, 0x40);
	bo_alloc(d, &s->ta40, H_TADATA, 0xb, 0x40, 0x10);
	bo_alloc(d, &s->ta50, H_TADATA, 0xb, 0x50, 0x1000);
	bo_alloc(d, &s->bgblock, H_3DPARAMS, 0x4b, 0x200, 0x1000);
	for (int i = 0; i < 2; i++) {
		bo_alloc(d, &s->rgn[i], H_TADATA, 0xb, r->region_bytes, 0x40);
		bo_alloc(d, &s->ta88[i], H_TADATA, 0xb, 0x88, 0x10);
		bo_alloc(d, &s->entword[i], H_KDATA, 0x4f, 4, 0x20);
	}

	s->eot = eot_program(c, s);
	s->pixev = pixel_event(c, s->eot, &s->pixev_rows);
	background(c, s);

	/* RT data: header, then per entry {HW ctx, render details in flight (the microkernel
	 * fills it), tiles/16, tiles*12, bg block, TA data 0x88/0x40/0x50, tail pointers and
	 * size, RTs, 0, kdata word, 0x10} */
	s->rtword.map[0] = 1;			/* the frame counter: entry (1 & 1) renders first */
	rt = s->rtdata.map;
	rt[0] = s->rtword.va;
	rt[2] = r->padx;
	rt[5] = 2;
	for (int i = 0; i < 2; i++) {
		uint32_t *e = rt + 6 + 14 * i;

		e[0] = c->hwctx_va;
		e[2] = r->mt_tiles >> 4;
		e[3] = r->mt_tiles * 0xc;
		e[4] = s->bgblock.va;
		e[5] = s->ta88[i].va;
		e[6] = s->ta40.va;
		e[7] = s->ta50.va;
		e[8] = s->tailptr.va;
		e[9] = r->tailptr_bytes;
		e[10] = 1;
		e[12] = s->entword[i].va;
		e[13] = 0x10;
	}

	/* render details (STATE.md section 6) */
	rd = s->rd.map;
	rd[0x00 / 4] = c->pb.va & 0xfff00000;	/* 3D request base */
	rd[0x08 / 4] = 0x80;			/* [G] */
	rd[0x0c / 4] = 0x000f0000;		/* [G] */
	rd[0x10 / 4] = 0x80000000;
	rd[0x20 / 4] = s->rgn[1].va;		/* region headers of this render */
	rd[0x28 / 4] = s->rgn[0].va;		/* and of the next */
	rd[0x38 / 4] = 0x80000000;		/* no depth load */
	rd[0x3c / 4] = 0x80000000;		/* no stencil load */
	rd[0x40 / 4] = f32(1.0f);		/* ISP background depth [C] */
	rd[0x44 / 4] = 0x300;			/* [G] */
	rd[0x48 / 4] = 0x2000000 | (s->bgblock.va - (c->pb.va & 0xfff00000)) >> 4;
	rd[0x50 / 4] = 0x88;
	rd[0x58 / 4] = 0x30;
	rd[0x5c / 4] = s->pixev;
	rd[0x60 / 4] = s->pixev_rows;
	rd[0x64 / 4] = 1;			/* [G] */
	rd[0x68 / 4] = 1;
	rd[0xcc / 4] = 0x100;
	rd[0xd0 / 4] = s->rd_sync.va;
	rd[0xd8 / 4] = s->rtdata.va;
	rd[0xdc / 4] = s->rtdata.va + 0x18 + 0x38;
	rd[0x104 / 4] = 1;			/* [G] */
	rd[0x110 / 4] = s->rd_w8.va;
	rd[0x114 / 4] = s->rd_w4.va;
	rd[0x13c / 4] = 0x07c00000;		/* [G] */
	s->rd_w4.map[0] = s->bg_pds[2];
	s->rd_sync.map[0] = 1;
	s->hwsync.map[0] = s->hwsync_access.va;	/* SGXMKIF_HWDEVICE_SYNC_LIST.sAccessDevAddr */
	s->hwsync_access.map[0] = 1;
	return s;
}

void sgx_surface_destroy(struct sgx_ctx *c, struct sgx_surface *s)
{
	/* SGX_FLUSH_HW_RENDER_TARGET is what the DDK sends here, but on the EA3
	 * (srvinit-started microkernel) the cleanup never completes and the GPU
	 * locks up after it (re/gen/GEN.md); only on request */
	int e = getenv("SGXGEN_FLUSH_RT") ? pvr_flush_rt(&c->dev->pvr, s->rtdata.va) : 0;

	if (e)
		fprintf(stderr, "sgxgen: SGX_FLUSH_HW_RENDER_TARGET %#x failed: %d\n", s->rtdata.va, e);
	if (c->fb == s)
		c->fb = NULL;
	free(s);	/* device memory goes with the process (resman) */
}

void sgx_ctx_destroy(struct sgx_ctx *c)
{
	int e = pvr_unregister_render_ctx(&c->dev->pvr, c->hwctx);

	if (e)
		fprintf(stderr, "sgxgen: SGX_UNREGISTER_HW_RENDER_CONTEXT failed: %d\n", e);
	free(c);
}

void sgx_set_framebuffer(struct sgx_ctx *c, struct sgx_surface *s)
{
	c->fb = s;
	c->vdm.used = 0;
	c->vdm_start = c->vdm.bo.va;
}

/* ------------------------------------------------------------------ shaders */

struct sgx_shader *sgx_shader_builtin(struct sgx_ctx *c, enum sgx_builtin which)
{
	struct sgx_shader *s = calloc(1, sizeof *s);

	switch (which) {
	case SGX_VS_XY:
		s->vertex = 1;
		s->code = usse_load(c, &usse_vs_xy, 1);
		s->in_dwords = 4;
		s->out_dwords = 4;
		s->attr_dwords = 2;
		break;
	case SGX_VS_XYZ:
		s->vertex = 1;
		s->code = usse_load(c, &usse_vs_xyz, 1);
		s->in_dwords = 4;
		s->out_dwords = 4;
		s->attr_dwords = 3;
		break;
	case SGX_FS_UNIFORM:
		s->sec = usse_load(c, &usse_fs_uniform_sec, 0);
		s->code = usse_load(c, &usse_fs_uniform, 0);
		s->sec_dwords = 11;
		s->uniform_off = 9;
		break;
	}
	return s;
}

/* ------------------------------------------------------------------ recording */

static void vdm(struct sgx_ctx *c, uint32_t w)
{
	*ring_alloc(&c->vdm, 4, 4).p = w;
}

/* A TA state block goes to the MTE through a VDM state program: DMA the block into
 * USSE attributes, run a task that emits them (STATE.md section 2) */
static void state_block(struct sgx_ctx *c, const uint32_t *blk, uint32_t n, uint32_t dma_dwords, uint32_t kind)
{
	struct sgx_ptr b = ring_alloc(&c->pds_vtx, dma_dwords * 4, DMA_ALIGN);
	struct pds p = { 0 };
	uint32_t rows, va;

	memcpy(b.p, blk, n * 4);
	if (!c->state_emit[n])
		c->state_emit[n] = usse_load(c, usse_state_emit[n], 1);
	/* the DDK's static state program: tst; bra; doutd; tst; bra; doutd; doutu */
	pds_d0(&p, 0, b.va);
	pds_d0(&p, 1, dma_ctrl(dma_dwords, 0, kind, 1));
	pds_d0(&p, 2, 0);
	pds_d0(&p, 3, 0);
	pds_d0(&p, 4, doutu_w0(c, c->state_emit[n], 1, 0));
	pds_d0(&p, 5, 0);
	CODE(&p, TST1(S1_DS0(0)));
	CODE(&p, BRA_P0(6));
	CODE(&p, MOVS(MV, 0, 0, 0, 1, 0, 0, DOUTD));
	CODE(&p, TST1(S1_DS0(2)));
	CODE(&p, BRA_P0(6));
	CODE(&p, MOVS(MV, 1, 0, 0, 1, 0, 0, DOUTD));
	CODE(&p, MOVS(MV, 2, 0, 0, 1, 2, 0, DOUTU));
	CODE(&p, HALT);
	va = pds_emit(&c->pds_vtx, &p, &rows);
	vdm(c, 0x40000000 | va >> 4);
	/* info (libGLESv2 0x397f0): rows, the rotating buffer index + 0x2000, 0x2020400, dwords */
	vdm(c, rows << 26 | ((c->vdm_counter + 0x2000) | 0x2020400 | ((n + 3) >> 2)));
	c->vdm_counter = (c->vdm_counter + 1) & 3;
}

/* Vertices per USSE task and index batch (libGLESv2 FUN_00017c20 limits them by the
 * USSE attribute space; the two values here fit every draw seen [G]). */
static uint32_t vertices_per_task(uint32_t out_dwords)
{
	uint32_t o = (out_dwords + 3) & ~3u, v = 64 / (o ? o : 4);

	return v > 16 ? 16 : v;
}

/* a draw: index list + vertex fetch program (STATE.md section 2; PDS.md 0x1f3d0) */
static void vdm_draw(struct sgx_ctx *c, uint32_t prim, uint32_t count, uint32_t idx, uint32_t vb,
		     uint32_t stride, uint32_t attr_dwords, uint32_t vs, uint32_t in_dwords, uint32_t out_dwords)
{
	struct pds p = { 0 };
	uint32_t rows, va, vpt = vertices_per_task(out_dwords), batch = 16;

	pds_d0(&p, 0, vb);
	pds_d0(&p, 1, attr_dwords - 1);		/* DOUTD: dwords-1, attribute register 0 */
	pds_d1(&p, 0, stride);
	pds_d1(&p, 1, 0);
	pds_d0(&p, 2, doutu_w0(c, vs, 1, 0));
	pds_d0(&p, 3, 0);
	CODE(&p, ALU(OP_MUL16, 0, S1_IN(0), S2_DS1(0), U_DS1(0x32)));	/* index * stride */
	CODE(&p, MOVS(MV_X, 0, 0x19, 0, 1, 2, 2, DOUTD));		/* + ds1[0x32] */
	CODE(&p, MOVS(MV_CC3, 1, 0, 0, 1, 3, 3, DOUTU));
	CODE(&p, HALT);
	va = pds_emit(&c->pds_vtx, &p, &rows);
	vdm(c, 0x80000000 | 0x00c00000 | prim << 26 | count);
	vdm(c, idx & ~1u);				/* 16-bit indices */
	vdm(c, (batch - 1) << 28);
	vdm(c, 0x003fffff);
	vdm(c, (vpt - 1) << 28 | va >> 4);
	vdm(c, (out_dwords + 3) >> 2 << 24 | ((in_dwords * 4 + 0xf) >> 4) << 7 |
	       (vpt - 1 < 3 ? vpt - 1 : 3) << 22 | rows);
}

/* glClear(COLOR): a screen-covering triangle, the DDK's way (NOTES.md "Clear") */
void sgx_clear(struct sgx_ctx *c, const float rgba[4], float depth)
{
	struct sgx_surface *s = c->fb;
	struct sgx_ptr v = ring_alloc(&c->gen, 3 * 16, DMA_ALIGN);
	uint32_t argb = u8(rgba[3]) << 24 | u8(rgba[0]) << 16 | u8(rgba[1]) << 8 | u8(rgba[2]);
	float xy[3][2] = { { 0, 0 }, { 2.0f * s->w, 0 }, { 0, 2.0f * s->h } };
	uint32_t blk[12];

	for (int i = 0; i < 3; i++) {
		v.p[4 * i + 0] = f32(xy[i][0]);
		v.p[4 * i + 1] = f32(xy[i][1]);
		v.p[4 * i + 2] = f32(depth);
		v.p[4 * i + 3] = argb;
	}
	/* TA state block: ISPA ISPB PDS region-clip output-select MTE TSP (header 0xd4c3) */
	blk[0] = 0x1 | 0x2 | 0x40 | 0x80 | 0x400 | 0x1000 | 0x4000 | 0x8000;
	blk[1] = depth == 1.0f ? 0x01d00200 : 0x01c00200;	/* always; depth write only for a depth clear */
	blk[2] = 0xf << 4;					/* ISPB: colour mask */
	blk[3] = pds_word(c, c->pds_empty_p, 1);
	blk[4] = 0x00030001;					/* [G] */
	blk[5] = pds_word(c, c->clear_pds, 2);
	blk[6] = 0x80000000 | 0 << 16 | (s->rt.xt - 1);		/* region clip: whole surface, in tiles */
	blk[7] = 0 << 16 | (s->rt.yt - 1);
	blk[8] = 8 << 16 | 0x1800;				/* 8 output dwords [D], 0x1800 [G] */
	blk[9] = 0x40;						/* MTE ctrl of the clear [G] */
	blk[10] = 5;						/* one 4-component set */
	blk[11] = 0;
	state_block(c, blk, 12, 12, 3);
	vdm_draw(c, SGX_TRIANGLES, 3, c->idx, v.va, 16, 4, c->clear_vs, 4, 8);
}

static uint32_t ispa(const struct sgx_state *st)
{
	uint32_t v = 0x200;			/* b9: ISPB follows */

	if (!st->depth_func)
		v |= 7u << 22 | 0x100000;	/* always, no write */
	else
		v |= (st->depth_func - 0x200) << 22 | (st->depth_write ? 0 : 0x100000);
	return v;
}

static uint32_t mte_ctrl(const struct sgx_state *st)
{
	uint32_t ccw = st->front != SGX_CW, k = 0;

	if (st->cull == SGX_BACK)
		k = ccw ? 1 : 2;
	else if (st->cull == SGX_FRONT)
		k = ccw ? 2 : 1;
	return 0x1555620 | k;
}

int sgx_draw(struct sgx_ctx *c, const struct sgx_draw *d)
{
	struct sgx_surface *s = c->fb;
	const struct sgx_state *st = d->state;
	const struct sgx_shader *vs = d->vs, *fs = d->fs;
	uint32_t blk[18], n = 0, rows, sec, prim, vrows;
	uint32_t vw = vs->attr_dwords * 4;
	struct sgx_ptr v = ring_alloc(&c->gen, d->count * vw, DMA_ALIGN), u, vsec;
	float vx = st->vp_x, vy = st->vp_y, vwid = st->vp_w, vh = st->vp_h, zn = st->depth_n, zf = st->depth_f;

	if (vwid == 0) {
		vx = vy = 0;
		vwid = (float)s->w;
		vh = (float)s->h;
	}
	if (zn == 0 && zf == 0)
		zf = 1.0f;
	memcpy(v.p, d->verts, d->count * vw);

	/* fragment: the uniforms (FP16 pairs) and the secondary program that DMAs them,
	 * then the primary program that runs the shader */
	u = ring_alloc(&c->pds_pix, fs->sec_dwords * 4, DMA_ALIGN);
	memset(u.p, 0, fs->sec_dwords * 4);
	u.p[fs->uniform_off] = f16(d->color[0]) | f16(d->color[1]) << 16;
	u.p[fs->uniform_off + 1] = f16(d->color[2]) | f16(d->color[3]) << 16;
	sec = pds_secondary(&c->pds_pix, u.va, dma_ctrl(fs->sec_dwords, 0, 2, 1),
			    doutu_w0(c, fs->sec, 0, 0), 4, &rows);
	blk[n++] = 0;
	blk[n++] = ispa(st);
	blk[n++] = 0x10 << 8 | 0xf << 4;			/* ISPB: line width 1.0, colour mask */
	blk[n++] = pds_word(c, sec, rows);
	/* PDS middle word: b18-24 = (secondary dwords+3)>>2 [D]; the rest from the DDK's
	 * shader object (b0, b16-17 constant; b25-28 = 0xb for this shader) [G] */
	blk[n++] = 0x00030001 | ((fs->sec_dwords + 3) >> 2) << 18 | 0xbu << 25;
	prim = pds_pixel(c, doutu_w0(c, fs->code, 0, fs->temps), 4, 0, &rows);
	blk[n++] = pds_word(c, prim, rows);
	/* viewport (window: y flipped) [C x offset, D rest] */
	blk[n++] = f32(vx + vwid * 0.5f);
	blk[n++] = f32(vwid * 0.5f);
	blk[n++] = f32((float)s->h - (vy + vh * 0.5f));
	blk[n++] = f32(-vh * 0.5f);
	blk[n++] = f32((zn + zf) * 0.5f);
	blk[n++] = f32((zf - zn) * 0.5f);
	blk[n++] = 0;						/* header 0x200 */
	blk[n++] = vs->out_dwords << 16 | 0x3000;		/* output select */
	blk[n++] = 0x3727c5ac;					/* W clamp 1e-5 */
	blk[n++] = mte_ctrl(st);
	blk[n++] = 0;						/* TSP: no texcoords */
	blk[n++] = 0;
	blk[0] = 0x1 | 0x2 | 0x40 | 0x100 | 0x200 | 0x400 | 0x800 | 0x1000 | 0x4000 | 0x8000;
	/* the DDK DMAs 25 dwords for a draw's block (kind 0); the header says where it ends */
	state_block(c, blk, n, 25, 0);

	/* vertex secondary attributes: the block's own address, then the driver constants
	 * 2/255, 1/255, 2/65535, 1/65535 [D] */
	vsec = ring_alloc(&c->pds_vtx, 10 * 4, DMA_ALIGN);
	memset(vsec.p, 0, 40);
	vsec.p[0] = vsec.va;
	vsec.p[2] = f32(2.0f / 255);
	vsec.p[3] = f32(1.0f / 255);
	vsec.p[4] = f32(2.0f / 65535);
	vsec.p[5] = f32(1.0f / 65535);
	sec = pds_secondary(&c->pds_vtx, vsec.va, dma_ctrl(10, 0, 1, 1), doutu_w0(c, c->empty_v, 1, 0), 0, &vrows);
	vdm(c, 0x40000000 | sec >> 4);
	vdm(c, vrows << 26 | 0xe100 | (10 + 3) >> 2);

	vdm_draw(c, d->prim, d->count, c->idx, v.va, vw, vs->attr_dwords, vs->code, vs->in_dwords, vs->out_dwords);
	return 0;
}

/* ------------------------------------------------------------------ submission */

/* the end of a kick's stream: an empty secondary update, the TA terminate, VDM terminate */
static uint32_t vdm_end(struct sgx_ctx *c)
{
	uint32_t at = c->vdm.bo.va + c->vdm.used;

	vdm(c, 0x40000000 | c->empty_state >> 4);
	vdm(c, 1u << 26 | 0xe100);
	vdm(c, 0x60000000 | c->terminate >> 4);
	vdm(c, 3u << 26 | 0x2022401);
	vdm(c, 0xc0000000);
	return at;
}

/* SGXMKIF_CMDTA, client part (STATE.md section 1) */
static void ta_command(struct sgx_ctx *c, uint32_t vdm_base, int last)
{
	struct sgx_surface *s = c->fb;
	uint32_t *cmd = c->ccb.map + c->ccb_woff / 4, *t = cmd + 0x238 / 4;

	memset(cmd, 0, 0x2c0);
	cmd[0x00 / 4] = 0x2c0;				/* size, rounded to 64 */
	cmd[0x04 / 4] = last ? 0x62 : 0x71;		/* TA flags [D] */
	cmd[0x08 / 4] = s->bg_pds[0];
	cmd[0x0c / 4] = s->bg_pds[1];
	cmd[0x10 / 4] = s->bg_pds[2];
	cmd[0x14 / 4] = 0x50000000;
	cmd[0x18 / 4] = 0x50000000;
	cmd[0x1c / 4] = last ? 0x400 : 0;		/* 3D flags: render */
	cmd[0x24 / 4] = c->pbdesc.va;
	cmd[0x38 / 4] = s->rd.va;
	cmd[0x3c / 4] = s->hwsync.va;
	cmd[0x40 / 4] = s->rtdata.va;
	cmd[0x44 / 4] = s->rtdata.va + 0x18 + 0x38;	/* entry 1 (frame counter 1) */
	t[0x00 / 4] = 0x20000000;			/* TE AA */
	t[0x04 / 4] = s->rt.mtile1;
	t[0x08 / 4] = s->rt.mtile2;
	t[0x0c / 4] = s->rt.te_screen;
	t[0x10 / 4] = s->rt.mt_tiles;
	t[0x14 / 4] = last ? 0x400000 : 0;		/* TA flags 2 */
	t[0x18 / 4] = vdm_base;
	t[0x1c / 4] = 0xa2;				/* [G] */
	t[0x20 / 4] = s->rgn[1].va;
	t[0x24 / 4] = s->tailptr.va;
	t[0x28 / 4] = f32(1e-6f);
	t[0x2c / 4] = f32(1e-6f);
	t[0x30 / 4] = (s->h - 1) << 16 | (s->w - 1);
	t[0x3c / 4] = c->pb.va & 0xfff00000;
	t[0x6c / 4] = 0x88;
}

static void kick(struct sgx_ctx *c, int last)
{
	struct sgx_surface *s = c->fb;
	SGX_CCB_KICK k;
	IMG_HANDLE dst[1] = { s->color.b.sync };
	int e;

	memset(&k, 0, sizeof k);
	k.hDevMemContext = c->dev->pvr.memctx;
	k.hCCBKernelMemInfo = c->ccb.b.mem;
	k.ui32CCBOffset = c->ccb_woff + 0x58;
	k.sCommand.ui32Data[1] = c->hwctx_va;
	k.hKernelHWSyncListMemInfo = s->hwsync.b.mem;
	k.ui32NumDstSyncObjects = 1;
	k.pahDstSyncHandles = dst;
	if (last) {
		k.bLastInScene = IMG_TRUE;
		k.h3DSyncInfo = c->sync3d.b.sync;
	} else {
		k.bFirstKickOrResume = IMG_TRUE;
	}
	c->ccb_woff += 0x2c0;
	c->ccbctl.map[0] = c->ccb_woff;		/* CCB write offset */
	__sync_synchronize();
	if ((e = pvr_kick(&c->dev->pvr, &k)))
		die("SGX_DOKICK failed: %d", e);
}

static int wait_render(struct sgx_ctx *c, unsigned ms)
{
	struct pvr_buf sb = { .mem = c->fb->color.b.sync };
	volatile PVRSRV_SYNC_DATA *sd;
	unsigned t = 0;

	if (pvr_map(&c->dev->pvr, &sb))
		die("map of the render target's sync data failed");
	sd = sb.cpu;
	while (sd->ui32WriteOpsComplete < sd->ui32WriteOpsPending && t < ms) {
		struct timespec ts = { 0, 1000000 };

		nanosleep(&ts, NULL);
		t++;
	}
	if (c->dev->verbose)
		fprintf(stderr, "sgxgen: render target write ops %u/%u after %u ms\n", sd->ui32WriteOpsComplete,
			sd->ui32WriteOpsPending, t);
	return sd->ui32WriteOpsComplete < sd->ui32WriteOpsPending ? -ETIMEDOUT : 0;
}

int sgx_flush(struct sgx_ctx *c, unsigned timeout_ms)
{
	uint32_t k2;
	int e;
	struct timespec ts = { 0, 200 * 1000000 };

	vdm_end(c);
	k2 = vdm_end(c);
	ta_command(c, c->vdm_start, 0);
	kick(c, 0);
	nanosleep(&ts, NULL);
	if (c->dev->verbose)	/* words the microkernel writes once the TA has run */
		fprintf(stderr, "sgxgen: after kick 1: PB pages in use %#x, RT data +4 %#x, frame word %#x\n",
			c->pbdesc.map[5], c->fb->rtdata.map[1], c->fb->rtword.map[0]);
	if (getenv("SGXGEN_KICK1_ONLY"))
		return 0;
	ta_command(c, k2, 1);
	kick(c, 1);
	c->frames++;
	e = wait_render(c, timeout_ms);
	if (c->dev->verbose)
		fprintf(stderr, "sgxgen: after kick 2: PB pages in use %#x, RT data +4 %#x, frame word %#x\n",
			c->pbdesc.map[5], c->fb->rtdata.map[1], c->fb->rtword.map[0]);
	return e;
}
