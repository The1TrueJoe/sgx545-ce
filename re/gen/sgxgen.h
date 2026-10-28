/*
 * sgxgen: build SGX545 frames from GL-level parameters, with no DDK code and
 * nothing read from a capture. Every word comes from the formulas in
 * re/state/STATE.md, re/pds/PDS.md and re/isa/ISA.md; re/gen/GEN.md maps each
 * buffer to its source.
 *
 * The API is shaped like what a gallium driver needs underneath it:
 *
 *   sgx_dev       the services connection (re/replay/pvr.c)
 *   sgx_ctx       one GL context: HW render context, CCB, parameter buffer,
 *                 the fixed programs, and the frame being recorded
 *   sgx_surface   a render target (colour buffer + the TA/3D structures
 *                 SGXAddRenderTarget makes for it)
 *   sgx_shader    a vertex or fragment program (USSE code + what the PDS
 *                 programs need to know about it)
 *   sgx_state     the draw-time state objects (depth, cull, viewport)
 *   sgx_clear / sgx_draw record into the frame's VDM stream;
 *   sgx_flush     submits it (two TA kicks, the second renders) and waits.
 *   sgx_surface_destroy / sgx_ctx_destroy release what the microkernel holds.
 */
#ifndef SGXGEN_H
#define SGXGEN_H

#include <stdint.h>

#include "pvr.h"

/* GL enums the state objects take */
#define SGX_NEVER 0x200
#define SGX_LESS 0x201
#define SGX_EQUAL 0x202
#define SGX_LEQUAL 0x203
#define SGX_GREATER 0x204
#define SGX_NOTEQUAL 0x205
#define SGX_GEQUAL 0x206
#define SGX_ALWAYS 0x207
#define SGX_FRONT 0x404
#define SGX_BACK 0x405
#define SGX_CW 0x900
#define SGX_CCW 0x901

#define SGX_TRIANGLES 0
#define SGX_TRIANGLE_STRIP 3

struct sgx_dev {
	struct pvr pvr;
	uint32_t heap_base[16];		/* by SGX heap id */
	int verbose;
};

/* one device allocation, mapped */
struct sgx_bo {
	struct pvr_buf b;
	uint32_t va;
	uint32_t *map;
};

/* a bump allocator inside one device allocation */
struct sgx_ring {
	struct sgx_bo bo;
	uint32_t used;
};

/* where something landed: device address and CPU pointer */
struct sgx_ptr {
	uint32_t va;
	uint32_t *p;
};

struct sgx_rt_numbers {
	uint32_t xt, yt, mode, mx, my, padx, pady;
	uint32_t mtile1, mtile2, te_screen, mt_tiles, region_bytes, tailptr_bytes;
};

struct sgx_surface {
	uint32_t w, h, stride;		/* stride in pixels */
	struct sgx_bo color;		/* BGRA8888, top row first */
	struct sgx_rt_numbers rt;
	struct sgx_bo rtdata, rtword, entword[2], tailptr, ta40, ta50, rgn[2], ta88[2], bgblock;
	struct sgx_bo rd, rd_sync, rd_w8, rd_w4, hwsync, hwsync_access;
	uint32_t eot;			/* end-of-tile USSE program */
	uint32_t pixev, pixev_rows;	/* pixel event PDS program */
	uint32_t bg_pds[3];		/* background object PDS words (secondary, middle, primary) */
};

struct sgx_shader {
	int vertex;
	uint32_t code;			/* USSE main program */
	uint32_t sec;			/* USSE secondary program (fragment), or 0 */
	uint32_t temps;			/* temporary registers */
	/* vertex: */
	uint32_t in_dwords;		/* primary attribute registers the program uses */
	uint32_t out_dwords;		/* outputs per vertex */
	uint32_t attr_dwords;		/* dwords fetched per vertex (stream 0, attribute register 0) */
	/* fragment: */
	uint32_t sec_dwords;		/* uniform dwords DMA'd to the secondary program */
	uint32_t uniform_off;		/* dword of the vec4 FP16 uniform c in that block */
};

struct sgx_state {
	uint32_t depth_func;		/* 0 = depth test off */
	int depth_write;
	uint32_t cull;			/* 0, SGX_FRONT or SGX_BACK */
	uint32_t front;			/* SGX_CCW (default) or SGX_CW */
	float vp_x, vp_y, vp_w, vp_h;	/* glViewport; w = 0: the whole surface */
	float depth_n, depth_f;		/* glDepthRangef; both 0: 0..1 */
};

struct sgx_draw {
	uint32_t prim;			/* SGX_TRIANGLES, SGX_TRIANGLE_STRIP */
	uint32_t count;
	const float *verts;		/* count x vs->attr_dwords floats */
	const struct sgx_shader *vs, *fs;
	const struct sgx_state *state;
	float color[4];			/* the fragment shader's uniform vec4 */
};

struct sgx_ctx {
	struct sgx_dev *dev;
	struct sgx_bo pb, freelist, pbdesc, pbblocks, ccb, ccbctl, scratch, sync3d;
	uint32_t pb_pages;
	IMG_HANDLE hwctx;
	uint32_t hwctx_va;
	uint32_t ccb_woff;
	struct sgx_ring pds_pix, pds_vtx, usse_pix, usse_vtx, gen;
	/* fixed programs */
	uint32_t empty_v, empty_p, copy_fs, clear_vs, pe_ptoff, pe_emit, state_emit[33];
	uint32_t empty_state, terminate;	/* VDM state programs (PDS vertex heap) */
	uint32_t pds_empty_p;			/* pixel PDS program: run the empty task */
	uint32_t clear_pds;			/* pixel PDS program: iterate colour, copy_fs */
	uint32_t idx;				/* identity 16-bit index buffer */
	uint32_t vdm_counter;			/* the DDK's rotating TA-state buffer index */
	/* the frame being recorded */
	struct sgx_surface *fb;
	struct sgx_ring vdm;
	uint32_t vdm_start;
	unsigned frames;
};

int sgx_dev_open(struct sgx_dev *d, const char *node, const char *trace);
void sgx_dev_close(struct sgx_dev *d);

struct sgx_ctx *sgx_ctx_create(struct sgx_dev *d);
struct sgx_surface *sgx_surface_create(struct sgx_ctx *c, uint32_t w, uint32_t h);
/* Release a surface: the microkernel caches its RT data set and must be told to
 * drop it (SGX_FLUSH_HW_RENDER_TARGET), or the next client's TA hangs. */
void sgx_surface_destroy(struct sgx_ctx *c, struct sgx_surface *s);
void sgx_ctx_destroy(struct sgx_ctx *c);
void sgx_set_framebuffer(struct sgx_ctx *c, struct sgx_surface *s);

/* built-in shaders (re/gen/mkusse.py) */
enum sgx_builtin { SGX_VS_XY, SGX_VS_XYZ, SGX_FS_UNIFORM };
struct sgx_shader *sgx_shader_builtin(struct sgx_ctx *c, enum sgx_builtin which);

void sgx_clear(struct sgx_ctx *c, const float rgba[4], float depth);
int sgx_draw(struct sgx_ctx *c, const struct sgx_draw *d);
/* submit the frame; waits up to timeout_ms for the render to finish; 0 = done */
int sgx_flush(struct sgx_ctx *c, unsigned timeout_ms);

#endif
