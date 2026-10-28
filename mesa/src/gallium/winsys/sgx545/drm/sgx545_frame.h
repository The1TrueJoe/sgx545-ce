/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

/*
 * sgxws: SGX545 frames from GL-level parameters.
 *
 * This is re/gen/sgxgen.c as library code under the gallium driver: every
 * word comes from the formulas in re/state/STATE.md, re/pds/PDS.md and
 * re/isa/ISA.md (re/gen/GEN.md maps each buffer to its source).  What it
 * adds to sgxgen:
 *
 *  - colour buffers are the caller's (a pipe_resource, a wrapped fbdev
 *    mapping); a render target (sgxws_rt) is only the TA/3D structures for
 *    one size;
 *  - any number of frames per context and per render target, the way the
 *    DDK's swaps do it: one kick per frame (first and last in scene), the
 *    frame number in the TA command, the RT data entry and the region headers
 *    alternating (re/log, "more than one frame");
 *  - the per-frame rings are reset once a frame's render has finished, so a
 *    context runs forever in fixed memory.
 *
 * Nothing here is gallium: the driver in src/gallium/drivers/sgx545 turns
 * pipe state into these calls.
 */

#ifndef SGX545_FRAME_H
#define SGX545_FRAME_H

#include <stdbool.h>
#include <stdint.h>

#include "pvr.h"

/* SGX heap ids: the low byte of PVRSRV_HEAP_INFO::ui32HeapID */
enum sgx545_heap {
   SGX545_HEAP_GENERAL = 0,
   SGX545_HEAP_TADATA = 1,
   SGX545_HEAP_KDATA = 3,
   SGX545_HEAP_PIXSHADER = 4,
   SGX545_HEAP_VTXSHADER = 5,
   SGX545_HEAP_PDSPIXEL = 6,
   SGX545_HEAP_PDSVERTEX = 7,
   SGX545_HEAP_SYNC = 8,
   SGX545_HEAP_3DPARAMS = 10,
   SGX545_HEAP_TEXTURE = 14,     /* 0xf0000000 */
};

#ifdef __cplusplus
extern "C" {
#endif

/* GL enums the state takes */
#define SGXWS_NEVER 0x200
#define SGXWS_LESS 0x201
#define SGXWS_EQUAL 0x202
#define SGXWS_LEQUAL 0x203
#define SGXWS_GREATER 0x204
#define SGXWS_NOTEQUAL 0x205
#define SGXWS_GEQUAL 0x206
#define SGXWS_ALWAYS 0x207
#define SGXWS_FRONT 0x404
#define SGXWS_BACK 0x405

/* VDM primitive types */
#define SGXWS_TRIANGLES 0
#define SGXWS_LINES 1
#define SGXWS_POINTS 2
#define SGXWS_TRIANGLE_STRIP 3

/* the services connection (libpvr) */
struct sgxws_dev {
   struct pvr pvr;
   uint32_t heap_base[16];          /* device address of each heap, by heap id */
   int verbose;
   unsigned frame_gap_us;           /* SGX545_FRAME_GAP_US: idle time after each render */
};

/* one device allocation, mapped */
struct sgxws_bo {
   struct pvr_buf b;
   uint32_t va;
   uint32_t *map;
};

/* a bump allocator inside one device allocation; reset() returns to mark */
struct sgxws_ring {
   struct sgxws_bo bo;
   uint32_t used, mark;
};

struct sgxws_rt_numbers {
   uint32_t xt, yt, mode, mx, my, padx, pady;
   uint32_t mtile1, mtile2, te_screen, mt_tiles, region_bytes, tailptr_bytes;
};

/* The TA/3D structures SGXAddRenderTarget makes for one w x h target. */
struct sgxws_rt {
   uint32_t w, h;
   struct sgxws_rt_numbers n;
   struct sgxws_bo rtdata, rtword, entword[2], tailptr, ta40, ta50, rgn[2], ta88[2], bgblock;
   struct sgxws_bo rd, rd_sync, rd_w8, rd_w4, hwsync, hwsync_access;
   uint32_t frames;                 /* frames rendered into it */
};

/* where a frame's pixels go */
struct sgxws_target {
   uint32_t va;                     /* BGRA8888, top row first */
   uint32_t stride;                 /* in pixels, a multiple of 32 */
   IMG_HANDLE sync;                 /* the buffer's kernel sync info */
   volatile PVRSRV_SYNC_DATA *sync_data;
};

/* A shader as the hardware runs it: USSE code plus what the PDS programs
 * need to know about it. */
struct sgxws_prog {
   bool vertex;
   uint32_t code;                   /* USSE main program (device address) */
   uint32_t sec;                    /* USSE secondary program (fragment), or 0 */
   uint32_t sec_temps;              /* its temporaries */
   uint32_t temps;                  /* temporary registers */
   /* vertex: */
   uint32_t in_dwords;              /* primary attribute registers the program uses */
   uint32_t out_dwords;             /* outputs per vertex */
   uint32_t attr_dwords;            /* dwords fetched per vertex into pa0.. */
   uint32_t tsp_sizes;              /* TA state: texcoord sizes, 3 bits per set */
   uint32_t tsp_prec;               /* TA state: sets with precision value <= 2 */
   /* fragment: */
   uint32_t sec_dwords;             /* constant dwords DMA'd for the secondary program */
   uint32_t douti[10];              /* DOUTI words, one per iterated varying */
   uint32_t n_douti;
   uint32_t pds_middle;             /* TA state PDS middle word (0: the default) */
   /* both: */
   uint32_t const_dwords;           /* vertex: constants DMA'd after the driver block */
};

struct sgxws_state {
   uint32_t depth_func;             /* 0 = depth test off */
   bool depth_write;
   uint32_t cull;                   /* 0, SGXWS_FRONT or SGXWS_BACK */
   bool front_ccw;                  /* front faces wind CCW in the target's memory
                                     * (gallium's sense: y down) */
   uint32_t color_mask;             /* ISPB colour mask: R8 G4 B2 A1 */
   /* the viewport transform, as gallium gives it */
   float vp_scale[3], vp_translate[3];
};

struct sgxws_draw {
   uint32_t prim;                   /* SGXWS_* */
   uint32_t count;                  /* vertices */
   const uint32_t *verts;           /* count x vs->attr_dwords dwords */
   const struct sgxws_prog *vs, *fs;
   const struct sgxws_state *state;
   const uint32_t *fs_consts;       /* fs->sec_dwords dwords */
   const uint32_t *vs_consts;       /* vs->const_dwords dwords */
};

struct sgxws_ctx {
   struct sgxws_dev *dev;
   struct sgxws_bo pb, freelist, pbdesc, pbblocks, ccb, ccbctl, scratch, sync3d;
   uint32_t pb_pages;
   IMG_HANDLE hwctx;
   uint32_t hwctx_va;
   uint32_t ccb_woff;
   /* code (persistent) and per-frame rings */
   struct sgxws_ring usse_pix, usse_vtx;
   struct sgxws_ring pds_pix, pds_vtx, gen, vdm, usse_frame;
   /* fixed programs */
   uint32_t empty_v, empty_p, copy_fs, clear_vs, pe_ptoff, pe_emit, state_emit[33];
   uint32_t sec_wait;                /* pixel secondary task: wait for the constants' DMA */
   uint32_t empty_state, terminate;  /* VDM state programs (PDS vertex heap) */
   uint32_t pds_empty_p;             /* pixel PDS program: run the empty task */
   uint32_t clear_pds;               /* pixel PDS program: iterate colour, copy_fs */
   uint32_t idx;                     /* identity 16-bit index buffer */
   uint32_t idx_count;
   uint32_t vdm_counter;             /* the DDK's rotating TA-state buffer index */
   /* the frame being recorded */
   struct sgxws_rt *rt;
   struct sgxws_target target;
   uint32_t vdm_start;
   uint32_t bg_pds[3];
   unsigned draws;                   /* objects recorded in this frame */
   unsigned frames;
};

int sgxws_dev_init(struct sgxws_dev *d, int fd);
void sgxws_dev_fini(struct sgxws_dev *d);

int sgxws_bo_alloc(struct sgxws_dev *d, struct sgxws_bo *bo, enum sgx545_heap heap,
                   uint32_t attribs, uint32_t size, uint32_t align);
void sgxws_bo_free(struct sgxws_dev *d, struct sgxws_bo *bo);
/* the attributes the DDK gives a colour buffer it renders to */
#define SGXWS_ATTRIBS_COLOR 0x10000003

struct sgxws_ctx *sgxws_ctx_create(struct sgxws_dev *d);
void sgxws_ctx_destroy(struct sgxws_ctx *c);

struct sgxws_rt *sgxws_rt_create(struct sgxws_ctx *c, uint32_t w, uint32_t h);
void sgxws_rt_destroy(struct sgxws_ctx *c, struct sgxws_rt *rt);

/* Upload a USSE program to the code heaps (phas.i targets in reloc[] are
 * relocated to where it lands).  Returns its device address. */
uint32_t sgxws_usse_upload(struct sgxws_ctx *c, bool vertex, const uint32_t *w, unsigned n,
                           const uint8_t *reloc, unsigned nreloc);

/*
 * Compiled programs (mesa/src/sgx545/compiler: flat USSE words, low word
 * first), finalised the way the DDK's GL driver finalises a USP (docs/ISA.md
 * "Phases, visibility feedback and discard") and uploaded:
 *  - vertex: phase 0 = the DDK's prologue; phase 1 = the shader (temps
 *    temporaries), then emit the vertex; then the epilogue phase every
 *    vertex program needs (docs/GEN.md);
 *  - fragment: phas, the shader, then `mov o0, pa<color_pa> [end]` (output
 *    mode 1: the packed BGRA8 colour to the PBE);
 *  - fragment secondary (uniform-only code): phas, the code, `nop [end]`.
 */
uint32_t sgxws_upload_vs(struct sgxws_ctx *c, const uint64_t *code, unsigned n, unsigned temps);
uint32_t sgxws_upload_fs(struct sgxws_ctx *c, const uint64_t *code, unsigned n, unsigned color_pa);
uint32_t sgxws_upload_fs_secondary(struct sgxws_ctx *c, const uint64_t *code, unsigned n);

/* built-in shaders (tools/gen/mkusse.py) */
enum sgxws_builtin {
   SGXWS_VS_XY,         /* gl_Position = vec4(a0.xy, 0, 1) */
   SGXWS_VS_XYZ,        /* gl_Position = vec4(a0.xyz, 1) */
   SGXWS_FS_UNIFORM,    /* gl_FragColor = mediump uniform vec4, FP16 at fs_consts[9..10] */
};
void sgxws_prog_builtin(struct sgxws_ctx *c, enum sgxws_builtin which, struct sgxws_prog *p);
#define SGXWS_FS_UNIFORM_DWORDS 11
#define SGXWS_FS_UNIFORM_OFF 9
uint32_t sgxws_f16(float x);

/* Start a frame into rt / target, unless it is the one being recorded.
 * Recording into another target first flushes the current frame. */
int sgxws_begin(struct sgxws_ctx *c, struct sgxws_rt *rt, const struct sgxws_target *t);
bool sgxws_recording(struct sgxws_ctx *c);

/* glClear(COLOR) of the whole target: a screen-covering object (NOTES.md "Clear") */
int sgxws_clear(struct sgxws_ctx *c, const float rgba[4], float depth, bool write_depth);
int sgxws_draw(struct sgxws_ctx *c, const struct sgxws_draw *d);
/* Submit the frame and wait up to timeout_ms for its render; 0 when done.
 * A no-op when nothing is recorded. */
int sgxws_flush(struct sgxws_ctx *c, unsigned timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
