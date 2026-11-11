/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */

/*
 * sgxgen: build SGX545 frames from GL-level parameters, with no DDK code and
 * nothing read from a capture. Every word comes from the formulas in
 * docs/STATE.md, docs/PDS.md and docs/ISA.md; docs/GEN.md maps each
 * buffer to its source.
 *
 * The API is shaped like what a gallium driver needs underneath it:
 *
 *   sgx_dev       the services connection (libpvr/pvr.c)
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
 *
 * Built with -DSGXGEN_DRM it talks to the powervr_sgx DRM driver instead of the
 * DDK's services: the kernel then owns the firmware structures (HW render
 * context, PB, RT data, render details) and a frame is one SUBMIT_JOBS render
 * job of one TA kick, described by CMD_RENDER and CMD_TA_KICK streams.
 */
#ifndef SGXGEN_H
#define SGXGEN_H

#include <stdint.h>

#ifdef SGXGEN_DRM
#include <drm/drm.h>
#include "powervr_sgx_drm.h"
#else
#include "pvr.h"
#endif

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
#ifdef SGXGEN_DRM
	int fd;
	uint32_t vm;			/* VM context handle */
	uint32_t heap_next[16];		/* bump allocator per heap */
#else
	struct pvr pvr;
#endif
	uint32_t heap_base[16];		/* by SGX heap id */
	int verbose;
};

/* one device allocation, mapped */
struct sgx_bo {
#ifdef SGXGEN_DRM
	uint32_t handle;
	int code;			/* a code object: map is a CPU copy, uploaded to the GPU */
	uint32_t uploaded;		/* code: bytes [0, uploaded) are on the GPU */
#else
	struct pvr_buf b;
#endif
	uint32_t va, size;
	uint32_t *map;
};

/* a bump allocator inside one device allocation */
struct sgx_ring {
	struct sgx_bo bo;
	uint32_t used;
	uint32_t mark;	/* sgx_ctx_rewind() goes back to here */
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
	uint32_t flags;
	struct sgx_bo color;		/* BGRA8888, top row first */
	struct sgx_rt_numbers rt;
	struct sgx_bo rtdata, rtword, entword[2], tailptr, ta40, ta50, rgn[2], ta88[2], bgblock;
	struct sgx_bo rd, rd_sync, rd_w8, rd_w4, hwsync, hwsync_access;
#ifdef SGXGEN_DRM
	uint32_t rtds;			/* RT dataset handle */
#endif
	uint32_t eot;			/* end-of-tile USSE program */
	uint32_t pixev, pixev_rows;	/* pixel event PDS program */
	uint32_t bg_pds[3];		/* background object PDS words (secondary, middle, primary) */
	uint32_t frames;		/* renders so far (SGXGEN_ALTERNATE) */
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
	/* test programs (tools/iso), all 0 otherwise: */
	uint32_t w0;			/* DOUTU word 0 as given, instead of the computed one */
	const uint32_t *sec_init;	/* first words of the DMA'd block: fragment sa0-8; vertex sa6-9 */
	uint32_t sec_init_n;
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
#ifdef SGXGEN_DRM
	uint32_t ctx;			/* render context handle */
	uint32_t syncobj;		/* signalled by each frame */
	int ta_kicks;			/* 1: one kick renders; 2 (default): stream, then terminate */
#else
	struct sgx_bo pb, freelist, pbdesc, pbblocks, ccb, ccbctl, scratch, sync3d;
	uint32_t pb_pages;
	IMG_HANDLE hwctx;
	uint32_t hwctx_va;
	uint32_t ccb_woff;
#endif
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
/* flags: SGX_SURFACE_Y_UP lays rows out bottom first (an EGL pbuffer; a window is
 * top row first) */
#define SGX_SURFACE_Y_UP 1
struct sgx_surface *sgx_surface_create(struct sgx_ctx *c, uint32_t w, uint32_t h, uint32_t flags);
/* Release a surface: the microkernel caches its RT data set and must be told to
 * drop it (SGX_FLUSH_HW_RENDER_TARGET), or the next client's TA hangs. */
void sgx_surface_destroy(struct sgx_ctx *c, struct sgx_surface *s);
void sgx_ctx_destroy(struct sgx_ctx *c);
void sgx_set_framebuffer(struct sgx_ctx *c, struct sgx_surface *s);

/* built-in shaders (tools/gen/mkusse.py) */
enum sgx_builtin { SGX_VS_XY, SGX_VS_XYZ, SGX_FS_UNIFORM };
struct sgx_shader *sgx_shader_builtin(struct sgx_ctx *c, enum sgx_builtin which);

/* for test programs: load code into the context's shader heap; phas.i relocations as in mkusse.py */
uint32_t sgx_usse_load(struct sgx_ctx *c, int vertex, const uint32_t *w, unsigned n, const unsigned *reloc,
		       unsigned nreloc);
void sgx_bo_alloc(struct sgx_dev *d, struct sgx_bo *bo, uint32_t heap, uint32_t attribs, uint32_t size,
		  uint32_t align);

void sgx_clear(struct sgx_ctx *c, const float rgba[4], float depth);
int sgx_draw(struct sgx_ctx *c, const struct sgx_draw *d);
/* submit the frame; waits up to timeout_ms for the render to finish; 0 = done */
int sgx_flush(struct sgx_ctx *c, unsigned timeout_ms);
/* Rewind the context's program and data rings to the last mark, so a long run
 * reuses them; only when every frame recorded since the mark has completed.
 * sgx_ctx_create() marks; mark again after making anything that must last. */
void sgx_ctx_mark(struct sgx_ctx *c);
void sgx_ctx_rewind(struct sgx_ctx *c);
#ifdef SGXGEN_DRM
/* The frame's render job (signals c->syncobj), for a SUBMIT_JOBS of the caller's
 * own; *j must live until the ioctl returns. sgx_flush is this, submit and wait. */
struct sgx_job {
	uint64_t render[10], kick_buf[2][3];
	struct drm_powervr_sgx_ta_kick kick[2];
	struct drm_powervr_sgx_sync_op op;
	struct drm_powervr_sgx_job job;
};
void sgx_render_job(struct sgx_ctx *c, struct sgx_job *j);
/* A surface whose colour buffer is a GEM handle the caller holds (an imported
 * dma-buf), stride in pixels; map is the caller's CPU view of it, or NULL. */
struct sgx_surface *sgx_surface_import(struct sgx_ctx *c, uint32_t w, uint32_t h, uint32_t stride,
				       uint32_t handle, uint32_t size, uint32_t *map);
/* A code object's bytes [off, off + size) to the GPU (UPLOAD_CODE); 0 or -errno. */
int sgx_bo_upload(struct sgx_dev *d, struct sgx_bo *bo, uint32_t off, uint32_t size);
#endif

#endif
