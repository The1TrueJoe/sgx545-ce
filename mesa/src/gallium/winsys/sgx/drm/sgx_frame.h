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
 *    mapping, an imported dma-buf); a render target (sgxws_rt) is only the
 *    TA/3D structures for one size;
 *  - any number of frames per context and per render target, one kick per
 *    frame;
 *  - the per-frame rings are reset once a frame's render has finished, so a
 *    context runs forever in fixed memory.
 *
 * Nothing here is gallium: the driver in src/gallium/drivers/sgx turns
 * pipe state into the calls in sgxws.h.  This header is the winsys's own.
 *
 * The frames are recorded here, the same for every kernel driver.  How
 * memory is allocated and a recorded frame reaches the GPU is a backend's
 * (struct sgxws_kif):
 *
 *  - sgx_powervr_sgx.c: the powervr_sgx DRM driver (docs/UAPI.md).  The
 *    kernel owns every structure the microkernel reads; a frame is one
 *    SUBMIT_JOBS render job.
 *  - sgx_pvr.c: the DDK's services driver, pvrsrvkm, through libpvr
 *    (the DDK's services stack).  Built only with the
 *    kernel's bridge headers (meson option sgx-pvrsrvkm), so nothing of
 *    kernel/src reaches the rest of the winsys.
 */

#ifndef SGX_FRAME_H
#define SGX_FRAME_H

#include <stdbool.h>
#include <stdint.h>

#include "sgxws.h"

/* where a buffer lives: the SGX heap ids (the low byte of the DDK's heap ID),
 * which index sgxws_dev.heap_base */
enum sgx_heap {
   SGX_HEAP_GENERAL = 0,
   SGX_HEAP_TADATA = 1,
   SGX_HEAP_KDATA = 3,
   SGX_HEAP_PIXSHADER = 4,
   SGX_HEAP_VTXSHADER = 5,
   SGX_HEAP_PDSPIXEL = 6,
   SGX_HEAP_PDSVERTEX = 7,
   SGX_HEAP_SYNC = 8,
   SGX_HEAP_3DPARAMS = 10,
   SGX_HEAP_TEXTURE = 14, /* 0xf0000000 */
};

#ifdef __cplusplus
extern "C" {
#endif

struct sgxws_kif;

/* the connection to the kernel driver; a backend's device embeds it first */
struct sgxws_dev {
   const struct sgxws_kif *kif;
   struct sgxws_device_info info; /* filled by the backend; PCI ids by
                                     sgxws_dev_create */
   uint32_t heap_base[16];        /* device address of each heap, by heap id */
   uint8_t usse_base_index[2];    /* the first code base register of the pixel,
                                     vertex USSE heap */
   int verbose;
   bool flush_rt; /* sgxws_dev_options */
   unsigned frame_gap_us;
   unsigned slots;
   int dry_run;
   unsigned ta_split; /* sgxws_dev_options */
};

/* a buffer's backend side; a backend's buffer embeds it first */
struct sgxws_bo_impl {
   const struct sgxws_kif *kif;
   uint32_t size;
};

/* A ring: the persistent part up to mark, then one equal part per frame in
 * flight (the context's slots); a frame records into the part [lo, hi).  A ring
 * of USSE code may have no CPU mapping (the kernel scans what it runs): map
 * is then a copy the backend uploads, [0, uploaded) on the GPU already. */
#define SGXWS_MAX_SLOTS 8

struct sgxws_ring {
   struct sgxws_bo bo;
   uint32_t size;
   uint32_t used, mark;
   uint32_t part, hi, parts;
   uint32_t uploaded;
   uint32_t last_seq; /* the last kick that runs code from it (code pools) */
};

struct sgxws_rt_numbers {
   uint32_t xt, yt, mode, mx, my, padx, pady;
   uint32_t mtile1, mtile2, te_screen, mt_tiles, region_bytes, tailptr_bytes;
};

/* the TA/3D structures for one w x h target */
struct sgxws_rt {
   uint32_t w, h;
   uint32_t ctx_gen; /* the backend's context it was made in (a reset makes
                        another) */
   struct sgxws_rt_numbers n;
   uint32_t frames;   /* frames rendered into it */
   uint32_t seq;      /* its last render's kick number */
   unsigned seq_slot; /* and the ring slot that render used */
   void *k;           /* the backend's */
};

/* end-of-tile programs, by target (they hold its PBE words) */
#define SGXWS_EOT_CACHE 64

struct sgxws_eot {
   uint32_t va, stride, w, h;
   uint8_t pbe_format, t1_layout;
   uint32_t code;
};

struct sgxws_ctx {
   struct sgxws_dev *dev;
   void *k; /* the backend's */
   /* code (persistent) and per-frame rings */
   struct sgxws_ring usse_pix, usse_vtx;
   struct sgxws_ring pds_pix, pds_vtx, gen, vdm;
   /* the end-of-tile programs: two code pools used in turn, each emptied
    * once no render runs code from it */
   struct sgxws_ring eot_pool[2];
   unsigned eot_cur, n_eot;
   struct sgxws_eot eot[SGXWS_EOT_CACHE];
   /* fixed programs */
   uint32_t empty_v, empty_p, copy_fs, clear_vs, vpgen_vs, pe_ptoff, pe_emit,
      state_emit[33];
   uint32_t sec_wait; /* pixel secondary task: wait for the constants' DMA */
   uint32_t empty_state, terminate; /* VDM state programs (PDS vertex heap) */
   uint32_t pds_empty_p;            /* pixel PDS program: run the empty task */
   uint32_t clear_pds; /* pixel PDS program: iterate colour, copy_fs */
   uint32_t
      clear_pds_masked[16]; /* the same with a colour mask (by byte mask) */
   uint32_t idx;            /* identity 16-bit index buffer */
   uint32_t defaults;       /* what a fetch leaves out (vdm_fetch) */
   uint32_t idx_count;
   uint32_t vdm_counter; /* the DDK's rotating TA-state buffer index */
   /* the frame being recorded */
   struct sgxws_rt *rt;
   struct sgxws_target target;
   uint32_t vdm_start;
   uint32_t bg_pds[3];
   uint32_t pixev, pixev_rows; /* its pixel event program */
   unsigned draws;             /* objects recorded in this frame */
   /* kick_ta: the TA kicks of this frame's scene so far, and the objects
    * since the last one */
   unsigned ta_kicks, seg_draws;
   /* the frame's depth bias (render details +0x14, +0x18), once a draw
    * with it is recorded */
   bool bias_set;
   int32_t bias_units;
   float bias_factor;
   struct sgxws_rect
      clip; /* what the visible bits and the region clip are now */
   /* shared buffers it reads or writes (sgxws_use_bo) */
   unsigned n_use;
   struct {
      const struct sgxws_bo *bo;
      bool write;
   } use[32];
   unsigned room_flushes; /* frames submitted early because a ring filled */
   /* frames in flight: the ring slot recording now, and the kick that last
    * used each slot */
   unsigned slot, slots;
   struct {
      uint32_t seq;
      uint64_t kicked_ns;
      bool scanout;
   } slot_kick[SGXWS_MAX_SLOTS];
   uint32_t kicks, done_seq; /* kicks submitted, the last one known done */
   uint64_t ns_kick, ns_target_wait,
      ns_slot_wait;        /* where sgxws_flush's time goes (stats) */
   uint64_t ns_fence_wait; /* fences waited for (sgxws_wait_seq) */
   uint64_t ns_flush, ns_cmd, ns_sync_wait; /* all of sgxws_flush; the command;
                                               a synchronous flush's wait */
   uint64_t ns_latency;
   unsigned n_latency; /* kick to seen done, renders that were waited for */
   uint64_t ns_latency_scanout;
   unsigned n_latency_scanout; /* of those, the ones into the scanout buffer */
   unsigned frames;
   enum sgxws_reset reset; /* since sgxws_reset_status last asked */
   uint64_t gpu_indices,
      gpu_verts; /* stats: the VDM's indices and the vertices behind them */
};

/*
 * A backend.  Kick numbers are the context's: 1 for its first kick, then
 * one more each; renders finish in kick order.  At most c->slots kicks of a
 * context are unfinished at a time, so seq % c->slots names one.
 */
struct sgxws_kif {
   const char *name;
   /* the target's previous render must be done before the next is kicked
    * (the client writes the target's render details) */
   bool rt_one_in_flight;
   void (*dev_destroy)(struct sgxws_dev *d);

   /* a buffer in a heap; in a USSE heap, a code buffer whose map may be a copy */
   int (*bo_alloc)(struct sgxws_dev *d, struct sgxws_bo *bo, enum sgx_heap heap,
                   uint32_t attribs, uint32_t size, uint32_t align);
   void (*bo_free)(struct sgxws_dev *d, struct sgxws_bo *bo);
   int (*bo_wrap)(struct sgxws_dev *d, struct sgxws_bo *bo, void *cpu,
                  uint32_t size);
   int (*bo_import)(struct sgxws_dev *d, struct sgxws_bo *bo, int fd,
                    uint32_t size);
   int (*bo_export)(struct sgxws_dev *d, const struct sgxws_bo *bo, int *fd);
   int (*bo_kms_handle)(const struct sgxws_bo *bo, uint32_t *handle);
   void (*bo_prepare_target)(struct sgxws_dev *d, struct sgxws_bo *bo);
   int (*bo_wait)(const struct sgxws_bo *bo, unsigned timeout_ms);
   /* code: bytes [off, off + size) of the copy to the GPU (NULL: the map is
    * the buffer); the buffer emptied, at the same address, while no render
    * runs code from it */
   int (*code_upload)(struct sgxws_dev *d, struct sgxws_bo *bo, uint32_t off,
                      uint32_t size);
   int (*code_reset)(struct sgxws_dev *d, struct sgxws_bo *bo);

   int (*ctx_init)(struct sgxws_ctx *c);
   void (*ctx_fini)(struct sgxws_ctx *c);
   int (*rt_init)(struct sgxws_ctx *c, struct sgxws_rt *rt);
   void (*rt_fini)(struct sgxws_ctx *c, struct sgxws_rt *rt);
   /* submit the recorded frame (c->rt, c->target, c->vdm_start ...: its VDM
    * stream ends with vdm_end's words) as kick seq.  A failed kick counts
    * as done. */
   int (*kick)(struct sgxws_ctx *c, uint32_t seq);
   /* optional: a TA kick of the frame recorded so far (c->vdm_start to
    * vdm_end's words), the scene going on in later kicks; the last is kick,
    * with c->ta_kicks > 0 */
   int (*kick_ta)(struct sgxws_ctx *c);
   /* kicks the backend's command ring holds unread (0: no bound) */
   unsigned ring_kicks;
   /* 0 once kick seq is done; timeout 0 polls */
   int (*wait_seq)(struct sgxws_ctx *c, uint32_t seq, unsigned timeout_ms);
   /* the frame line SGX_DEBUG=frame prints, the backend's part */
   void (*frame_info)(struct sgxws_ctx *c, char *buf, unsigned size);
};

/* the backends: 0 and *d, or -ENODEV when fd isn't their kernel driver */
int sgxws_powervr_sgx_dev_create(int fd, const struct sgxws_dev_options *opts,
                                 struct sgxws_dev **d);
int sgxws_pvr_dev_create(int fd, const struct sgxws_dev_options *opts,
                         struct sgxws_dev **d);

/* a buffer in a given heap with given attributes (sgxws_bo_alloc picks them by
 * use) */
int sgxws_bo_alloc_heap(struct sgxws_dev *d, struct sgxws_bo *bo,
                        enum sgx_heap heap, uint32_t attribs, uint32_t size,
                        uint32_t align);
/* the attributes the DDK gives a colour buffer it renders to */
#define SGXWS_ATTRIBS_COLOR 0x10000003

/* Upload a USSE program to the code heaps (phas.i targets in reloc[] are
 * relocated to where it lands).  Returns its device address. */
uint32_t sgxws_usse_upload(struct sgxws_ctx *c, bool vertex, const uint32_t *w,
                           unsigned n, const uint8_t *reloc, unsigned nreloc);

/* the words every VDM stream ends with */
void sgxws_vdm_end(struct sgxws_ctx *c);

uint64_t sgxws_now_ns(void);
uint32_t sgxws_f32(float x);

#define sgxws_err(...) fprintf(stderr, "sgx: " __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif
