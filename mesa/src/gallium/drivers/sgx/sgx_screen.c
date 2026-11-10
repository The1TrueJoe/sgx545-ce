/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <unistd.h>

#include "drm-uapi/drm.h"
#include "renderonly/renderonly.h"
#include "util/format/u_format.h"
#include "util/log.h"
#include "util/u_debug.h"
#include "util/u_memory.h"
#include "util/u_screen.h"

#include "sgx_context.h"
#include "sgx_device.h"
#include "sgx_program.h"
#include "sgx_resource.h"
#include "sgx_screen.h"
#include "sgx_texture.h"

uint32_t sgx_drv_debug;

static const struct debug_named_value sgx_drv_debug_options[] = {
   {"frame", SGX_DEBUG_FRAME, "print every frame the winsys submits"},
   {"nir", SGX_DEBUG_NIR, "print every shader's NIR"},
   {"draw", SGX_DEBUG_DRAW, "print every draw"},
   {"sync", SGX_DEBUG_SYNC, "print allocations and waits"},
   {"stats", SGX_DEBUG_STATS, "print where the time goes, every 2 s"},
   {"serial", SGX_DEBUG_SERIAL,
    "wait for every render after submitting it (no overlap)"},
   {"hangabort", SGX_DEBUG_HANG_ABORT,
    "abort at the first render that does not finish"},
   {"flushall", SGX_DEBUG_FLUSH_ALL, "submit at every flush (none deferred)"},
   {"novcache", SGX_DEBUG_NO_VCACHE, "no vertex cache"},
   {"nof16", SGX_DEBUG_NO_F16, "no F16/C10 fragment arithmetic"},
   {"nodrvconsts", SGX_DEBUG_NO_DRV_CONSTS,
    "no driver constant formats (lowp as F16, literals)"},
   {"fsconst", SGX_DEBUG_FS_CONST,
    "every fragment shader a constant colour per program (measuring)"},
   {"attrs", SGX_DEBUG_ATTRS,
    "describe the first draws with 5 or more vertex inputs, and clears"},
   {"blits", SGX_DEBUG_BLITS, "describe every 255th blit and its caller"},
   {"novs", SGX_DEBUG_NO_VS_COMPILER,
    "the fixed vertex programs instead of the compiler"},
   {"nofs", SGX_DEBUG_NO_FS_COMPILER,
    "the fixed fragment programs instead of the compiler"},
   {"noffmediump", SGX_DEBUG_NO_FF_MEDIUMP,
    "fixed-function fragment programs at highp (F32), not mediump (F16; texture coordinates F32)"},
   {"zerocopy", SGX_DEBUG_ZERO_COPY,
    "vertex inputs DMAed where they are (buffers in place, client arrays copied as they are), where it isn't the default"},
   {"nozerocopy", SGX_DEBUG_NO_ZERO_COPY,
    "vertex inputs gathered into f32 x 4, where zero-copy is the default"},
   {"nolazyscanout", SGX_DEBUG_NO_LAZY_SCANOUT,
    "scanout buffers on a dumb-buffer display (kmsro) are the display's from the start, not when a KMS handle is asked for"},
   {"nophasesplit", SGX_DEBUG_NO_PHASE_SPLIT,
    "a fragment program that discards runs whole before the visibility feedback, not only its test (its colour after, as the DDK's alpha-tested programs)"},
   {"nof16samples", SGX_DEBUG_NO_F16_SAMPLES,
    "texels a mediump program reads come back RGBA8 to unpack, not F16 (T3 01028800)"},
   {"tspf16", SGX_DEBUG_TSP_F16,
    "the TSP precision mask marks the sets the fragment program iterates as F16 and doesn't sample from (the DDK's, for the TA's parameter buffer)"},
   {"nozls", SGX_DEBUG_NO_ZLS,
    "depth and stencil not kept across renders (no ZLS load or store)"},
   {"flushrt", SGX_DEBUG_FLUSH_RT,
    "send SGX_FLUSH_HW_RENDER_TARGET at a target's destruction (hangs a srvinit microkernel)"},
   DEBUG_NAMED_VALUE_END};

DEBUG_GET_ONCE_FLAGS_OPTION(sgx_drv_debug, "SGX_DEBUG", sgx_drv_debug_options,
                            0)
DEBUG_GET_ONCE_OPTION(sgx_dump_code, "SGX_DUMP_CODE", NULL)
/* the winsys's debugging aids: a pvrtrace log of this client (and memory
 * copies for its first n kicks), frames built but never sent from the n-th
 * on, idle time after every render */
DEBUG_GET_ONCE_OPTION(sgx_trace, "SGX_TRACE", NULL)
DEBUG_GET_ONCE_NUM_OPTION(sgx_trace_kicks, "SGX_TRACE_KICKS", 0)
DEBUG_GET_ONCE_NUM_OPTION(sgx_dry_run, "SGX_DRY_RUN", -1)
DEBUG_GET_ONCE_NUM_OPTION(sgx_frame_gap_us, "SGX_FRAME_GAP_US", 0)
DEBUG_GET_ONCE_NUM_OPTION(sgx_slots, "SGX_SLOTS", 0)
DEBUG_GET_ONCE_NUM_OPTION(sgx_ta_split, "SGX_TA_SPLIT", 0)

const char *
sgx_dump_code_dir(void)
{
   return debug_get_option_sgx_dump_code();
}

static const char *
sgx_get_name(struct pipe_screen *pscreen)
{
   return sgx_screen(pscreen)->name;
}

static const char *
sgx_get_vendor(struct pipe_screen *pscreen)
{
   return "Mesa";
}

static const char *
sgx_get_device_vendor(struct pipe_screen *pscreen)
{
   return "Imagination Technologies";
}

static int
sgx_get_screen_fd(struct pipe_screen *pscreen)
{
   return sgx_screen(pscreen)->fd;
}

static void
sgx_init_shader_caps(struct pipe_screen *pscreen)
{
   const struct sgx_device_features *f = sgx_screen(pscreen)->features;

   for (unsigned s = MESA_SHADER_VERTEX; s <= MESA_SHADER_FRAGMENT;
        s += MESA_SHADER_FRAGMENT) {
      struct pipe_shader_caps *caps =
         (struct pipe_shader_caps *)&pscreen->shader_caps[s];

      caps->max_instructions = caps->max_alu_instructions =
         caps->max_tex_instructions = 16384;
      /* 8: st then doesn't pack varyings (st_extensions.c), so every varying
       * keeps a texture coordinate set of its own, as the DDK's compiler
       * allocates them; a projected or plain read of a whole varying is then
       * sampled by the PDS before the shader runs, not by the shader */
      caps->max_tex_indirections = 8;
      caps->max_control_flow_depth = 32;
      /* ISA.md: 16 attributes, 10 texture coordinate sets on the SGX545 */
      caps->max_inputs = s == MESA_SHADER_VERTEX ? f->vs_inputs : f->fs_inputs;
      caps->max_outputs = s == MESA_SHADER_VERTEX ? f->vs_outputs : 1;
      /* Constants live in SA registers, sa9 (sa10 in a vertex program) to
       * sa127.  How many uniforms that holds depends on the program: only
       * the constants it uses go there, and mediump ones two to a register
       * (dEQP's nested struct arrays of mat4 fit in a fragment program, and
       * failed at a limit of 24 vec4).  The constants that don't fit are
       * loaded from a block in memory (ld.a.d, sgxws_const_mem), so the
       * limit is GLES2's vertex minimum, 128 vec4, in both stages; a vertex
       * program gets the 9 vec4 st keeps for lowered clip planes and point
       * size on top, which leaves GL's 512 components to the application */
      caps->max_const_buffer0_size =
         (s == MESA_SHADER_VERTEX ? 128 + 9 : 128) * 4 * sizeof(float);
      caps->max_const_buffers = 1;
      /* u[i]: indexed from SA, or from the block when the array doesn't fit
       * (without it GLSL selects over the whole array, a load per element) */
      caps->indirect_const_addr = true;
      caps->max_temps = f->max_temps;
      if (s == MESA_SHADER_FRAGMENT) {
         caps->max_texture_samplers = caps->max_sampler_views = f->samplers;
         /* mediump and lowp in 16 bits (F16 iteration, fmad16, F16/C10
          * uniforms), as the DDK computes them: the varyings, texture and
          * clipping A/B cases are byte-identical to the DDK with it
          * (sgx_varyab.sh, _texproj, _texshape, _nearclip), and the bench
          * world runs 24-28 FPS against 20-27 in F32.  SGX_F16=0 keeps F32 */
         caps->fp16 = caps->glsl_16bit_consts =
            f->f16 && !(sgx_drv_debug & SGX_DEBUG_NO_F16);
      }
   }
}

static void
sgx_init_screen_caps(struct pipe_screen *pscreen)
{
   struct sgx_screen *screen = sgx_screen(pscreen);
   const struct sgx_device_features *f = screen->features;
   struct pipe_caps *caps = (struct pipe_caps *)&pscreen->caps;

   u_init_pipe_screen_caps(pscreen, 1);
   caps->graphics = true;
   caps->npot_textures = true;
   /* sampler-view swizzles, applied in the fragment shader (sgx_fs_for):
    * EXT_texture_swizzle, and the swizzle parameters glamor sets on GLES
    * (patch 0005) */
   caps->texture_swizzle = true;
   /* GPU resets reported (sgx_get_device_reset_status): ARB_robustness */
   caps->device_reset_status_query = true;
   /* blending is shader code (STATE.md section 3): any equation per channel */
   caps->blend_equation_separate = true;
   caps->uma = true;
   /* the kernel's scheduler takes a context's priority (glamor asks for
    * high, so X's copies don't queue behind a game's frames) */
   if (screen->info->priorities)
      caps->context_priority_mask = PIPE_CONTEXT_PRIORITY_LOW |
                                    PIPE_CONTEXT_PRIORITY_MEDIUM |
                                    PIPE_CONTEXT_PRIORITY_HIGH;
   /* a texture can be a render target: no bigger than the kernel takes one */
   caps->max_texture_2d_size =
      MIN3(f->max_texture_size, screen->info->rt_max_width,
           screen->info->rt_max_height);
   caps->max_texture_cube_levels = f->max_cube_levels;
   caps->max_texture_3d_levels = 0;
   caps->max_render_targets = 1;
   caps->glsl_feature_level = 120;
   caps->glsl_feature_level_compatibility = 120;
   caps->essl_feature_level = 100;
   caps->max_viewports = 1;
   caps->vendor_id = screen->info->pci_vendor;
   caps->device_id = screen->info->pci_device;
   caps->fs_coord_origin_upper_left = true;
   caps->fs_coord_pixel_center_half_integer = true;
   caps->min_line_width = caps->min_line_width_aa = 1;
   caps->min_point_size = caps->min_point_size_aa = 1;
   caps->max_line_width = caps->max_line_width_aa = 16;
   caps->max_point_size = caps->max_point_size_aa = 511;
   caps->point_size_granularity = caps->line_width_granularity = 0.1f;
   caps->max_texture_lod_bias = 16;
   /* What desktop GL has and the hardware does in the shader: st lowers the
    * alpha test to a discard, two-sided colour to a select on the face, and
    * gives every vertex program a point size (fixed function's points; a GLES
    * vertex program writes its own) */
   caps->alpha_test = false;
   caps->two_sided_color = false;
   caps->point_size_fixed = PIPE_POINT_SIZE_LOWER_ALWAYS;
   caps->clip_planes = 0;
   caps->texture_transfer_modes = 0;
   caps->max_vertex_element_src_offset = 2047;
   /* the draw gathers vertices from wherever they are, so client arrays
    * needn't be copied into upload buffers first (u_vbuf's copy would be a
    * second memcpy of every client-array draw) */
   caps->user_vertex_buffers = true;
   /* linear render targets, over powervr_sgx */
   if (sgxws_dev_has_dmabuf(sgx_screen(pscreen)->dev))
      caps->dmabuf = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT;
}

static bool
sgx_is_format_supported(struct pipe_screen *pscreen, enum pipe_format format,
                        enum pipe_texture_target target, unsigned sample_count,
                        unsigned storage_sample_count, unsigned usage)
{
   if (sample_count > 1 ||
       MAX2(1, sample_count) != MAX2(1, storage_sample_count))
      return false;

   if (usage &
       (PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SCANOUT | PIPE_BIND_SHARED)) {
      /* windows and pbuffers: the PBE's BGRA8888 (STATE.md section 4) */
      if (format != PIPE_FORMAT_B8G8R8A8_UNORM &&
          format != PIPE_FORMAT_B8G8R8X8_UNORM)
         return false;
   }
   if (usage & PIPE_BIND_RENDER_TARGET) {
      /* the PBE's formats (sgx_resource_target).  Not 565: st would then
       * pick it for GL_RGB565 textures, and T1 0x05 hands the compiler's
       * unpack R and B swapped; RGBA8 stands in */
      if (format != PIPE_FORMAT_B8G8R8A8_UNORM &&
          format != PIPE_FORMAT_B8G8R8X8_UNORM &&
          format != PIPE_FORMAT_R8G8B8A8_UNORM &&
          format != PIPE_FORMAT_R8G8B8X8_UNORM &&
          /* stored as RGBA8, drawn into with R (and G) written alone
           * (sgx_tex_bpp): GL_ARB_texture_rg, which X's glamor needs on GLES
           * for its 8-bit pixmaps (glyphs, masks) */
          format != PIPE_FORMAT_R8_UNORM && format != PIPE_FORMAT_R8G8_UNORM)
         return false;
   }
   if (usage & PIPE_BIND_DEPTH_STENCIL) {
      /* The ISP keeps depth and stencil per tile and no buffer is loaded or
       * stored (no ZLS yet), so they are good within one render. */
      if (format != PIPE_FORMAT_Z16_UNORM &&
          format != PIPE_FORMAT_Z24X8_UNORM &&
          format != PIPE_FORMAT_Z24_UNORM_S8_UINT)
         return false;
   }
   if (usage & PIPE_BIND_VERTEX_BUFFER) {
      /* gathered on the CPU, so anything util_format can unpack */
      const struct util_format_description *desc =
         util_format_description(format);

      if (!desc || desc->layout != UTIL_FORMAT_LAYOUT_PLAIN)
         return false;
   }
   if (usage & PIPE_BIND_INDEX_BUFFER) {
      if (format != PIPE_FORMAT_R8_UINT && format != PIPE_FORMAT_R16_UINT &&
          format != PIPE_FORMAT_R32_UINT)
         return false;
   }
   /* textures: the formats with a T1 code (docs/TEX.md); render targets
    * (an imported dma-buf among them: EGLImage, glamor's screen pixmap) are
    * sampled in place through the strided layout */
   if ((usage & PIPE_BIND_SAMPLER_VIEW) && !sgx_tex_format(format) &&
       !(usage & PIPE_BIND_RENDER_TARGET))
      return false;
   switch (target) {
   case PIPE_BUFFER:
   case PIPE_TEXTURE_1D: /* a 2D texture a texel high, sampled on that row
                            (sgx_program.c) */
   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_RECT:
   case PIPE_TEXTURE_CUBE:
      return true;
   default:
      return false;
   }
}

/* a destroyed screen's connection and hardware context, for the next screen */
static struct {
   int fd;
   struct sgxws_dev *dev;
   struct sgxws_ctx *hw;
} kept = {.fd = -1};
static simple_mtx_t kept_lock = SIMPLE_MTX_INITIALIZER;

static void
sgx_screen_destroy(struct pipe_screen *pscreen)
{
   struct sgx_screen *screen = sgx_screen(pscreen);

   /* Keep the services connection and a hardware context for the process's
    * next screen instead of tearing them down: when srvinit started the
    * microkernel, a render context's cleanup never completes (the bridge
    * returns -EINVAL after its wait), and the next connection's TA then runs
    * against a parameter buffer the microkernel doesn't use: no pages
    * allocated, nothing drawn, renders waiting ~270 ms, then lockups (an
    * application that initialises EGL, terminates it and initialises it
    * again).  The process's
    * exit releases them. */
   simple_mtx_lock(&kept_lock);
   if (kept.fd < 0) {
      kept.fd = screen->dev_fd;
      kept.dev = screen->dev;
      kept.hw = screen->hw;
      screen->hw = NULL;
   } else {
      if (screen->hw)
         sgxws_ctx_destroy(screen->hw);
      sgxws_dev_destroy(screen->dev);
      close(screen->dev_fd);
   }
   simple_mtx_unlock(&kept_lock);
   if (screen->fd != screen->dev_fd)
      close(screen->fd);
   if (screen->ro)
      screen->ro->destroy(screen->ro);
   slab_destroy_parent(&screen->transfer_pool);
   simple_mtx_destroy(&screen->lock);
   FREE(screen);
}

struct pipe_screen *
sgx_screen_create(int fd, const struct pipe_screen_config *config,
                  struct renderonly *ro)
{
   struct sgx_screen *screen = CALLOC_STRUCT(sgx_screen);

   if (!screen)
      return NULL;
   sgx_drv_debug = debug_get_option_sgx_drv_debug();
   /* The GL linker's varying optimisation (nir_opt_varyings) packs varyings
    * into shared slots whatever the driver asks: a fog varying packed in
    * front of a texture coordinate keeps the PDS from doing the texture read
    * (a projected read needs a whole vec4 varying at component 0), and it
    * becomes a dependent sample in the shader.  Varyings are kept as
    * declared, for this process, unless the user set it. */
   setenv("MESA_GLSL_DISABLE_IO_OPT", "1", 0);
   screen->fd = screen->dev_fd = fd;
   simple_mtx_lock(&kept_lock);
   if (kept.fd >= 0) {
      /* the previous screen's connection */
      screen->dev_fd = kept.fd;
      screen->dev = kept.dev;
      screen->hw = kept.hw;
      kept.fd = -1;
      kept.hw = NULL;
   }
   simple_mtx_unlock(&kept_lock);
   const struct sgxws_dev_options wsopts = {
      .verbose = (sgx_drv_debug & SGX_DEBUG_FRAME) != 0,
      .flush_rt = (sgx_drv_debug & SGX_DEBUG_FLUSH_RT) != 0,
      .trace = debug_get_option_sgx_trace(),
      .trace_kicks = debug_get_option_sgx_trace_kicks(),
      .dry_run = debug_get_option_sgx_dry_run(),
      .frame_gap_us = debug_get_option_sgx_frame_gap_us(),
      .slots = debug_get_option_sgx_slots(),
      .ta_split = debug_get_option_sgx_ta_split(),
   };

   if (screen->dev_fd == fd && !(screen->dev = sgxws_dev_create(fd, &wsopts))) {
      FREE(screen);
      return NULL;
   }
   screen->info = sgxws_dev_info(screen->dev);
   screen->features = sgx_device_features(screen->info);
   if (!screen->features) {
      mesa_loge("sgx: no support for core id %#x revision %#x",
                screen->info->core_id, screen->info->core_revision);
      sgxws_dev_destroy(screen->dev);
      FREE(screen);
      return NULL;
   }
   snprintf(screen->name, sizeof(screen->name), "PowerVR %s",
            screen->features->name);
   screen->ro = ro;
   screen->zero_copy =
      !(sgx_drv_debug & SGX_DEBUG_NO_ZERO_COPY) &&
      (screen->info->zero_copy || (sgx_drv_debug & SGX_DEBUG_ZERO_COPY));
   simple_mtx_init(&screen->lock, mtx_plain);
   slab_create_parent(&screen->transfer_pool, sizeof(struct sgx_transfer), 16);

   screen->base.destroy = sgx_screen_destroy;
   screen->base.get_name = sgx_get_name;
   screen->base.get_vendor = sgx_get_vendor;
   screen->base.get_device_vendor = sgx_get_device_vendor;
   screen->base.get_screen_fd = sgx_get_screen_fd;
   screen->base.is_format_supported = sgx_is_format_supported;
   screen->base.context_create = sgx_context_create;
   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++)
      screen->base.nir_options[i] = sgx_get_nir_options();

   sgx_resource_screen_init(screen);
   sgx_fence_screen_init(screen);
   sgx_init_shader_caps(&screen->base);
   sgx_init_screen_caps(&screen->base);
   return &screen->base;
}
