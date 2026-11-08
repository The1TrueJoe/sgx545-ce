/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * sgxws: the interface between the gallium driver and the SGX545 winsys.
 *
 * Everything here is GL-level state and the hardware's own words as docs/
 * describes them (STATE.md, PDS.md, ISA.md): nothing from the kernel's
 * services headers.  How a buffer, a context or a submission reaches the
 * kernel is the winsys's business (sgx_frame.h: the powervr_sgx DRM
 * driver, or the DDK's services through libpvr); the driver sees opaque
 * handles.
 */

#ifndef SGXWS_H
#define SGXWS_H

#include <stdbool.h>
#include <stdint.h>

#include "sgx_pds.h"

#ifdef __cplusplus
extern "C" {
#endif

struct sgxws_dev;               /* the connection to the GPU's kernel driver */
struct sgxws_ctx;               /* a hardware render context and its rings */
struct sgxws_rt;                /* the TA/3D structures of one target size */
struct sgxws_bo_impl;           /* the winsys's side of a buffer */

/* one device allocation, mapped */
struct sgxws_bo {
   uint32_t va;                 /* GPU address */
   uint32_t *map;               /* CPU mapping */
   struct sgxws_bo_impl *impl;  /* NULL: not allocated */
};

/* what a buffer is for: where it lives and how it is mapped */
enum sgxws_mem {
   SGXWS_MEM_RENDER_TARGET,     /* a colour buffer the GPU renders to and the CPU may read */
   SGXWS_MEM_TEXTURE,           /* the texture heap (twiddled or strided texels) */
   SGXWS_MEM_VERTEX,            /* vertices and indices the VDM reads */
};

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

/* where a frame's pixels go */
struct sgxws_target {
   uint32_t va;                     /* BGRA8888, top row first */
   uint32_t stride;                 /* in pixels; even, or 1 */
   const struct sgxws_bo *bo;       /* the buffer (its renders are waited for through it) */
   bool scanout;                    /* the scanout buffer (stats only) */
   /* its format and layout (TEX.md sections 4, 11): the PBE's format code, and
    * how every tile's load samples it, T1's format and layout (6 strided,
    * 3 linear with the pitch rounded up to 8 texels, 0 twiddled, which the
    * PBE then writes twiddled too) */
   uint8_t pbe_format, t1_format, t1_layout;
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
   bool point_size;                 /* vertex: the last output dword is the point size */
   /* fragment: */
   uint32_t sec_dwords;             /* constant dwords DMA'd for the secondary program */
   struct sgx_pds_douti douti[10];   /* DOUTIs, one per iterated input or sample */
   uint8_t douti_tex[10];           /* 0, or the sampler unit + 1 whose DOUTT follows it */
   uint32_t n_douti;
   uint32_t pds_middle;             /* TA state PDS middle word (0: the default) */
   uint32_t pa_dwords;              /* fragment: primary attribute registers the program uses */
   /* both: */
   uint32_t const_dwords;           /* vertex: constants DMA'd after the driver block */
};

/* Blending is fragment code (STATE.md section 3): in a translucent pass o0
 * holds the tile's colour, and the colour epilogue is
 *    sop2 o0, pa<c>, o0 [skipinv end c1 c2 cop a1 a2 aop]
 * dst.rgb = (s1 * c1) cop (s2 * c2), dst.a = (s1.a * a1) aop (s2.a * a2),
 * s1 the shader's colour, s2 the tile's; with swap the sources trade places
 * (sop2 o0, o0, pa<c>), for a reverse subtract.  The DDK's SRC_ALPHA,
 * ONE_MINUS_SRC_ALPHA draw is c1=a1=s1a, c2=a2=1-s1a, add (EA3). */
enum sgxws_bsel {
   SGXWS_BS_ZERO, SGXWS_BS_S1, SGXWS_BS_S2, SGXWS_BS_S1A, SGXWS_BS_S2A,
   SGXWS_BS_ASAT,                   /* colour only: min(s1.a, 1 - s2.a) */
   SGXWS_BS_INV = 8,                /* or'd in: 1 - the selection */
};
enum sgxws_bop { SGXWS_BOP_ADD, SGXWS_BOP_SUB, SGXWS_BOP_MIN, SGXWS_BOP_MAX };

/* The colour epilogue: blend (sop2), then the colour mask.  A colour mask is
 *    sopwm o0, pa<c>, pa<c> [skipinv end mask=wmask c1=zero c2=zero cop=max aop=max]
 * after the colour, a bit per byte of the packed BGRA8 (B b0, G b1, R b2,
 * A b3), as the DDK's glColorMask draws (EA3); with a blend before it, the
 * sop2 writes pa<c> back and doesn't end the program. */
struct sgxws_blend {
   uint8_t blend;                   /* the sop2 below */
   uint8_t c1, c2, a1, a2;          /* sgxws_bsel; a1/a2 only zero, s1a, s2a (+ inv) */
   uint8_t cop, aop;                /* sgxws_bop */
   uint8_t swap;
   uint8_t wmask;                   /* bytes written; 0xf: no sopwm */
   /* With a constant factor sop2's selectors can't name it (STATE.md
    * "Constant-colour blending"): each side is multiplied out with sopwm into
    * r0, r1 and the two added, in place of the sop2 above.  Factors as
    * enum sgxws_bfactor; the blend colour, packed BGRA8, in sa2. */
   uint8_t constant;
   uint8_t fs, fd, fsa, fda;
   /* the alpha side's products trade r0 and r1, so its subtract runs the
    * other way from the colour's (SUBTRACT against REVERSE_SUBTRACT) */
   uint8_t atrade;
};

/* a constant-colour blend's factors, in glBlendFunc's order */
enum sgxws_bfactor {
   SGXWS_BF_ZERO, SGXWS_BF_ONE, SGXWS_BF_SRC_COLOR, SGXWS_BF_ONE_MINUS_SRC_COLOR,
   SGXWS_BF_SRC_ALPHA, SGXWS_BF_ONE_MINUS_SRC_ALPHA, SGXWS_BF_DST_ALPHA, SGXWS_BF_ONE_MINUS_DST_ALPHA,
   SGXWS_BF_DST_COLOR, SGXWS_BF_ONE_MINUS_DST_COLOR, SGXWS_BF_SRC_ALPHA_SATURATE,
   SGXWS_BF_CONSTANT_COLOR, SGXWS_BF_ONE_MINUS_CONSTANT_COLOR, SGXWS_BF_CONSTANT_ALPHA,
   SGXWS_BF_ONE_MINUS_CONSTANT_ALPHA,
};
#define SGXWS_BLEND_CONST_SA 2

/* A pixel rectangle of the target in memory order (the first row on top):
 * x0 <= x < x1, y0 <= y < y1 */
struct sgxws_rect {
   uint32_t x0, y0, x1, y1;
};

struct sgxws_state {
   uint32_t depth_func;             /* 0 = depth test off */
   bool depth_write;
   uint32_t cull;                   /* 0, SGXWS_FRONT or SGXWS_BACK */
   bool front_ccw;                  /* front faces wind CCW in the target's memory
                                     * (gallium's sense: y down) */
   uint32_t color_mask;             /* ISPB colour mask: R8 G4 B2 A1 */
   uint32_t pass_type;              /* ISPA b25-27: 0 opaque, 1 translucent (o0
                                     * starts as the tile's colour: blending),
                                     * 2 translucent with discard, 4 punch-through
                                     * (discard) */
   /* the viewport transform, as gallium gives it */
   float vp_scale[3], vp_translate[3];
   /* draws reach only these pixels: the scissor and the viewport's rectangle
    * (STATE.md "Scissor and partial viewports"); never empty */
   struct sgxws_rect clip;
   float line_width;
   bool sprite;                     /* points that generate gl_PointCoord: ISPA object type 4 */
   /* stencil (STATE.md "ISPC"): ISPC words and references, face 0 the block's
    * first group and 1 its second, which two_sided sends */
   bool stencil, two_sided;
   uint32_t ispc[2];
   uint8_t stencil_ref[2];
};

#define SGXWS_MAX_INDICES 0xffff

/* One vertex DMA (PDS DOUTD, PDS.md "What the DDK generates"): dwords (1-16)
 * of a vertex from va + index * stride into attribute register reg; with
 * defaults, the components past dwords up to 4 read 0, 0, 0, 1.  block > 0:
 * va is an offset into the draw's copy block block - 1. */
struct sgxws_fetch {
   uint32_t va, stride;
   uint8_t dwords, reg, block;
   bool defaults;
};

/* the DMAs a draw may have, its defaults counted */
#define SGXWS_MAX_FETCH 30

/* CPU memory the draw copies into its frame as is (a client array's bytes) */
struct sgxws_block {
   const void *cpu;
   uint32_t bytes;
};

struct sgxws_draw {
   uint32_t prim;                   /* SGXWS_* */
   uint32_t count;                  /* vertices */
   const uint32_t *verts;           /* count x vs->attr_dwords dwords */
   /* NULL: the vertices in order; else n_indices 16-bit indices into them
    * (at most SGXWS_MAX_INDICES) */
   const uint16_t *indices;
   uint32_t n_indices;
   /* or both already in device memory (the driver's vertex cache): their
    * addresses, nothing copied (verts_va 16-byte aligned, idx_va even) */
   uint32_t verts_va, idx_va;
   /* or n_fetch DMAs (the vertex buffers where they are, or copies of client
    * arrays): no vertices copied, verts and verts_va unused */
   const struct sgxws_fetch *fetch;
   unsigned n_fetch;
   const struct sgxws_block *blocks;
   unsigned n_blocks;
   const struct sgxws_prog *vs, *fs;
   const struct sgxws_state *state;
   const uint32_t *fs_consts;       /* fs->sec_dwords dwords */
   const uint32_t *vs_consts;       /* vs->const_dwords dwords */
   const uint32_t (*tex)[4];        /* per sampler unit: the DOUTT words T0-T3 */
   /* A program's constants that don't fit in SA (the compiler's ld.a.d from
    * a memory block): copied to frame memory, a new block each draw, whose
    * address + 512 * j goes in the program's SA register base[j] (an index
    * into fs_consts, or into vs_consts) */
   struct sgxws_const_mem {
      const uint32_t *words;
      uint32_t dwords;              /* 0: none */
      uint8_t n_bases, base[8];
   } vs_mem, fs_mem;
};

/* where the winsys's time goes (SGX_STATS) */
struct sgxws_stats {
   uint64_t ns_flush, ns_cmd, ns_kick;        /* all of sgxws_flush; the TA command; the kick */
   uint64_t ns_target_wait, ns_slot_wait;     /* a target's or ring slot's last render */
   uint64_t ns_sync_wait, ns_fence_wait;      /* a synchronous flush; fences */
   uint64_t ns_latency, ns_latency_scanout;   /* kick to seen done; of those, into the scanout buffer */
   unsigned n_latency, n_latency_scanout;
   unsigned room_flushes;                     /* frames submitted early because a ring filled */
   uint64_t gpu_indices, gpu_verts;           /* what the VDM was given: indices, and the vertices they index */
};

/* debugging aids the driver passes on (its SGX_DEBUG options) */
struct sgxws_dev_options {
   bool verbose;                /* print every frame submitted */
   bool flush_rt;               /* SGX_FLUSH_HW_RENDER_TARGET at a target's destruction */
   const char *trace;           /* a pvrtrace log of this client, as tools/decode reads a DDK capture */
   unsigned trace_kicks;        /* and memory copies for its first n kicks only (0: all) */
   int dry_run;                 /* frames from the n-th on are built (and traced) but never sent; -1: off */
   unsigned frame_gap_us;       /* idle time after each render */
   unsigned slots;              /* frames in flight, 2 to SGXWS_MAX_SLOTS (0: 3) */
};

/* fd: the GPU's kernel driver; opts may be NULL */
struct sgxws_dev *sgxws_dev_create(int fd, const struct sgxws_dev_options *opts);
void sgxws_dev_destroy(struct sgxws_dev *d);

int sgxws_bo_alloc(struct sgxws_dev *d, struct sgxws_bo *bo, enum sgxws_mem mem, uint32_t size,
                   uint32_t align);
/* a CPU mapping (the fbdev framebuffer) made a render target; services only (-ENOTSUP) */
int sgxws_bo_wrap(struct sgxws_dev *d, struct sgxws_bo *bo, void *cpu, uint32_t size);
void sgxws_bo_free(struct sgxws_dev *d, struct sgxws_bo *bo);

/* Sharing (powervr_sgx only; -ENOTSUP otherwise).  A buffer imported or
 * exported is shared: renders that read or write it are ordered against
 * other users through its dma-buf's fences (implicit sync), when the frame
 * names it (sgxws_use_bo; a frame's target is named by sgxws_begin). */
bool sgxws_dev_has_dmabuf(const struct sgxws_dev *d);
/* a dma-buf (linear; the GPU reads and writes it in place, the CPU through its mapping) */
int sgxws_bo_import(struct sgxws_dev *d, struct sgxws_bo *bo, int dmabuf_fd, uint32_t size);
/* a new dma-buf fd of the buffer (the caller closes it) */
int sgxws_bo_export(struct sgxws_dev *d, const struct sgxws_bo *bo, int *dmabuf_fd);
/* its GEM handle on the device's fd */
int sgxws_bo_kms_handle(const struct sgxws_bo *bo, uint32_t *handle);
static inline bool
sgxws_bo_valid(const struct sgxws_bo *bo)
{
   return bo->impl != NULL;
}
/* wait for the submitted renders that write a render target (0 when done) */
/* a buffer to be rendered into: its sync data, for sgxws_bo_wait */
void sgxws_bo_prepare_target(struct sgxws_dev *d, struct sgxws_bo *bo);
/* the buffer the frame being recorded renders into, NULL when none is */
const struct sgxws_bo *sgxws_target_bo(const struct sgxws_ctx *c);
int sgxws_bo_wait(const struct sgxws_bo *bo, unsigned timeout_ms);

struct sgxws_ctx *sgxws_ctx_create(struct sgxws_dev *d);
void sgxws_ctx_destroy(struct sgxws_ctx *c);

struct sgxws_rt *sgxws_rt_create(struct sgxws_ctx *c, uint32_t w, uint32_t h);
void sgxws_rt_destroy(struct sgxws_ctx *c, struct sgxws_rt *rt);

/*
 * Compiled programs (mesa/src/sgx/compiler: flat USSE words, low word
 * first), finalised the way the DDK's GL driver finalises a USP (docs/ISA.md
 * "Phases, visibility feedback and discard") and uploaded:
 *  - vertex: phase 0 = the DDK's prologue; phase 1 = the shader (temps
 *    temporaries), then emit the vertex; then the epilogue phase every
 *    vertex program needs (docs/GEN.md);
 *  - fragment: phas, the shader, then `mov o0, pa<color_pa> [end]` (output
 *    mode 1: the packed BGRA8 colour to the PBE), or the blend and colour
 *    mask epilogue (sgxws_blend; a blend draws with sgxws_state.translucent);
 *  - fragment secondary (uniform-only code): phas, the code, `nop [end]`.
 */
uint32_t sgxws_upload_vs(struct sgxws_ctx *c, const uint64_t *code, unsigned n, unsigned temps);
uint32_t sgxws_upload_fs(struct sgxws_ctx *c, const uint64_t *code, unsigned n, unsigned color_pa,
                         const struct sgxws_blend *blend, int discard_pa, unsigned temps);
void sgxws_blend_words(const struct sgxws_blend *b, unsigned color_pa, bool end, uint32_t w[2]);
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

/* a shared buffer the frame being recorded reads (a texture) or writes */
void sgxws_use_bo(struct sgxws_ctx *c, const struct sgxws_bo *bo, bool write);

/* glClear(COLOR) of the whole target: a screen-covering object (NOTES.md "Clear") */
int sgxws_clear(struct sgxws_ctx *c, const float rgba[4], float depth, bool write_depth, unsigned wmask,
                int stencil);
int sgxws_draw(struct sgxws_ctx *c, const struct sgxws_draw *d);
/* Submit the frame; with timeout_ms, wait up to that long for its render (0
 * when done), else return once it is submitted.  A no-op when nothing is
 * recorded. */
int sgxws_flush(struct sgxws_ctx *c, unsigned timeout_ms);
/* wait for every submitted render */
int sgxws_wait_idle(struct sgxws_ctx *c, unsigned timeout_ms);
/* wait for the renders up to kick number seq (0 when done; timeout 0 polls) */
int sgxws_wait_seq(struct sgxws_ctx *c, uint32_t seq, unsigned timeout_ms);
/* the last kick known done, looking without waiting; the kick number the
 * frame being recorded will get (renders finish in kick order) */
uint32_t sgxws_done_seq(struct sgxws_ctx *c);
uint32_t sgxws_next_seq(const struct sgxws_ctx *c);
static inline bool
sgxws_seq_after(uint32_t a, uint32_t b)
{
   return (int32_t)(a - b) > 0;
}
/* objects recorded in the frame being recorded */
unsigned sgxws_frame_objects(const struct sgxws_ctx *c);
/* the winsys's counters; reset: start them again */
void sgxws_get_stats(struct sgxws_ctx *c, struct sgxws_stats *s, bool reset);

#ifdef __cplusplus
}
#endif

#endif
