/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * sgxws: SGX545 frames from GL-level parameters.
 *
 * This is tools/gen/sgxgen.c as library code under the gallium driver: every
 * word comes from the formulas in docs/STATE.md, docs/PDS.md and
 * docs/ISA.md (docs/GEN.md maps each buffer to its source).  What it
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
 * pipe state into the calls in sgxws.h.  This header is the winsys's own:
 * it holds the services (libpvr) side, which the driver never includes.
 */

#ifndef SGX545_FRAME_H
#define SGX545_FRAME_H

#include <stdbool.h>
#include <stdint.h>

#include "pvr.h"
#include "sgxws.h"

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

/* the services connection (libpvr) */
struct sgxws_dev {
   struct pvr pvr;
   uint32_t heap_base[16];          /* device address of each heap, by heap id */
   int verbose;
   bool flush_rt;                   /* sgxws_dev_options */
   unsigned frame_gap_us;
   int dry_run;
};

/* a buffer's services side */
struct sgxws_bo_impl {
   struct pvr_buf b;
   struct pvr_buf sync_map;         /* the mapping of its sync data, if any */
   volatile PVRSRV_SYNC_DATA *sd;   /* render targets: the sync data the CPU waits on */
};

/* a bump allocator inside one device allocation; reset() returns to mark */
/* A ring: the persistent part up to mark, then SGXWS_SLOTS equal parts,
 * one per frame in flight; a frame records into the part [lo, hi). */
#define SGXWS_SLOTS 3

struct sgxws_ring {
   struct sgxws_bo bo;
   uint32_t used, mark;
   uint32_t part, hi;
};

/* a submitted render: done when its sync's completed writes reach value */
struct sgxws_token {
   volatile PVRSRV_SYNC_DATA *sd;
   uint32_t value;
   uint32_t seq;                    /* the context's kick number (renders finish in order) */
   uint64_t kicked_ns;              /* when it was kicked (stats) */
   bool scanout;                    /* a render into the scanout buffer (stats) */
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
   uint32_t seq;                    /* its last render's kick number: one in flight per target */
   unsigned seq_slot;               /* and the ring slot that render used */
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
   uint32_t empty_v, empty_p, copy_fs, clear_vs, vpgen_vs, pe_ptoff, pe_emit, state_emit[33];
   uint32_t sec_wait;                /* pixel secondary task: wait for the constants' DMA */
   uint32_t empty_state, terminate;  /* VDM state programs (PDS vertex heap) */
   uint32_t pds_empty_p;             /* pixel PDS program: run the empty task */
   uint32_t clear_pds;               /* pixel PDS program: iterate colour, copy_fs */
   uint32_t clear_pds_masked[16];    /* the same with a colour mask (by byte mask), on demand */
   uint32_t idx;                     /* identity 16-bit index buffer */
   uint32_t idx_count;
   uint32_t vdm_counter;             /* the DDK's rotating TA-state buffer index */
   /* the frame being recorded */
   struct sgxws_rt *rt;
   struct sgxws_target target;
   uint32_t vdm_start;
   uint32_t bg_pds[3];
   unsigned draws;                   /* objects recorded in this frame */
   struct sgxws_rect clip;           /* what the visible bits and the region clip are now */
   unsigned room_flushes;            /* frames submitted early because a ring filled */
   /* frames in flight: the ring slot recording now, and the render that last
    * used each slot */
   unsigned slot;
   struct sgxws_token slot_token[SGXWS_SLOTS];
   uint32_t kicks, done_seq;         /* kicks submitted, the last one known done */
   uint64_t ns_kick, ns_target_wait, ns_slot_wait;   /* where sgxws_flush's time goes (stats) */
   uint64_t ns_fence_wait;           /* fences waited for (sgxws_wait_seq) */
   uint64_t ns_flush, ns_cmd, ns_sync_wait;   /* all of sgxws_flush; the TA command; a synchronous flush's wait */
   uint64_t ns_latency; unsigned n_latency;   /* kick to seen done, renders that were waited for */
   uint64_t ns_latency_scanout; unsigned n_latency_scanout;   /* of those, the ones into the scanout buffer */
   uint32_t pixev, pixev_rows;       /* the recording frame's pixel event, for its kick */
   unsigned frames;
};

int sgxws_dev_init(struct sgxws_dev *d, int fd, const struct sgxws_dev_options *opts);
void sgxws_dev_fini(struct sgxws_dev *d);

/* a buffer in a given heap with given attributes (sgxws_bo_alloc picks them by use) */
int sgxws_bo_alloc_heap(struct sgxws_dev *d, struct sgxws_bo *bo, enum sgx545_heap heap,
                        uint32_t attribs, uint32_t size, uint32_t align);
/* the attributes the DDK gives a colour buffer it renders to */
#define SGXWS_ATTRIBS_COLOR 0x10000003

/* Upload a USSE program to the code heaps (phas.i targets in reloc[] are
 * relocated to where it lands).  Returns its device address. */
uint32_t sgxws_usse_upload(struct sgxws_ctx *c, bool vertex, const uint32_t *w, unsigned n,
                           const uint8_t *reloc, unsigned nreloc);

/* wait for the renders that write a buffer (its sync's pending writes) */
int sgxws_wait_sync(volatile PVRSRV_SYNC_DATA *sd, unsigned timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
