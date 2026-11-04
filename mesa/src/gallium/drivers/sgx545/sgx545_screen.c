/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <unistd.h>

#include "drm-uapi/drm.h"
#include "util/format/u_format.h"
#include "util/u_debug.h"
#include "util/u_memory.h"
#include "util/u_screen.h"
#include "renderonly/renderonly.h"

#include "sgx545_context.h"
#include "sgx545_program.h"
#include "sgx545_resource.h"
#include "sgx545_screen.h"
#include "sgx545_texture.h"

uint32_t sgx545_debug;

static const struct debug_named_value sgx545_debug_options[] = {
   { "frame", SGX545_DEBUG_FRAME, "print every frame the winsys submits" },
   { "nir", SGX545_DEBUG_NIR, "print every shader's NIR" },
   { "draw", SGX545_DEBUG_DRAW, "print every draw" },
   { "sync", SGX545_DEBUG_SYNC, "print allocations and waits" },
   { "stats", SGX545_DEBUG_STATS, "print where the time goes, every 2 s" },
   { "serial", SGX545_DEBUG_SERIAL, "wait for every render after submitting it (no overlap)" },
   { "hangabort", SGX545_DEBUG_HANG_ABORT, "abort at the first render that does not finish" },
   { "flushall", SGX545_DEBUG_FLUSH_ALL, "submit at every flush (none deferred)" },
   { "novcache", SGX545_DEBUG_NO_VCACHE, "no vertex cache" },
   { "nof16", SGX545_DEBUG_NO_F16, "no F16/C10 fragment arithmetic" },
   { "nodrvconsts", SGX545_DEBUG_NO_DRV_CONSTS, "no driver constant formats (lowp as F16, literals)" },
   { "fsconst", SGX545_DEBUG_FS_CONST, "every fragment shader a constant colour per program (measuring)" },
   { "attrs", SGX545_DEBUG_ATTRS, "describe the first draws with 5 or more vertex inputs, and clears" },
   { "blits", SGX545_DEBUG_BLITS, "describe every 255th blit and its caller" },
   { "novs", SGX545_DEBUG_NO_VS_COMPILER, "the fixed vertex programs instead of the compiler" },
   { "nofs", SGX545_DEBUG_NO_FS_COMPILER, "the fixed fragment programs instead of the compiler" },
   { "flushrt", SGX545_DEBUG_FLUSH_RT, "send SGX_FLUSH_HW_RENDER_TARGET at a target's destruction (hangs a srvinit microkernel)" },
   DEBUG_NAMED_VALUE_END
};

DEBUG_GET_ONCE_FLAGS_OPTION(sgx545_debug, "SGX545_DEBUG", sgx545_debug_options, 0)
DEBUG_GET_ONCE_OPTION(sgx545_dump_code, "SGX545_DUMP_CODE", NULL)
/* the winsys's debugging aids: a pvrtrace log of this client (and memory
 * copies for its first n kicks), frames built but never sent from the n-th
 * on, idle time after every render */
DEBUG_GET_ONCE_OPTION(sgx545_trace, "SGX545_TRACE", NULL)
DEBUG_GET_ONCE_NUM_OPTION(sgx545_trace_kicks, "SGX545_TRACE_KICKS", 0)
DEBUG_GET_ONCE_NUM_OPTION(sgx545_dry_run, "SGX545_DRY_RUN", -1)
DEBUG_GET_ONCE_NUM_OPTION(sgx545_frame_gap_us, "SGX545_FRAME_GAP_US", 0)

const char *
sgx545_dump_code_dir(void)
{
   return debug_get_option_sgx545_dump_code();
}


static const char *
sgx545_get_name(struct pipe_screen *pscreen)
{
   return "PowerVR SGX545 (sgx545-ce)";
}

static const char *
sgx545_get_vendor(struct pipe_screen *pscreen)
{
   return "Mesa";
}

static const char *
sgx545_get_device_vendor(struct pipe_screen *pscreen)
{
   return "Imagination Technologies";
}

static int
sgx545_get_screen_fd(struct pipe_screen *pscreen)
{
   return sgx545_screen(pscreen)->fd;
}

static void
sgx545_init_shader_caps(struct pipe_screen *pscreen)
{
   for (unsigned s = MESA_SHADER_VERTEX; s <= MESA_SHADER_FRAGMENT; s += MESA_SHADER_FRAGMENT) {
      struct pipe_shader_caps *caps = (struct pipe_shader_caps *)&pscreen->shader_caps[s];

      caps->max_instructions = caps->max_alu_instructions = caps->max_tex_instructions = 16384;
      /* 8: st then doesn't pack varyings (st_extensions.c), so every varying
       * keeps a texture coordinate set of its own, as the DDK's compiler
       * allocates them; a projected or plain read of a whole varying is then
       * sampled by the PDS before the shader runs, not by the shader
       * (Minecraft's terrain: two dependent samples per pixel otherwise) */
      caps->max_tex_indirections = 8;
      caps->max_control_flow_depth = 32;
      /* ISA.md: 16 attributes, 10 texture coordinate sets */
      caps->max_inputs = s == MESA_SHADER_VERTEX ? 16 : 10;
      caps->max_outputs = s == MESA_SHADER_VERTEX ? 11 : 1;
      /* Constants live in SA registers, sa9 (sa10 in a vertex program) to
       * sa127.  How many uniforms that holds depends on the program: only
       * the constants it uses go there, and mediump ones two to a register
       * (dEQP's nested struct arrays of mat4 fit in a fragment program, and
       * failed at a limit of 24 vec4).  The constants that don't fit are
       * loaded from a block in memory (ld.a.d, sgxws_const_mem), so the
       * limit is GLES2's vertex minimum, 128 vec4, in both stages */
      caps->max_const_buffer0_size = 128 * 4 * sizeof(float);
      caps->max_const_buffers = 1;
      /* u[i]: indexed from SA, or from the block when the array doesn't fit
       * (without it GLSL selects over the whole array, a load per element) */
      caps->indirect_const_addr = true;
      caps->max_temps = 64;
      if (s == MESA_SHADER_FRAGMENT) {
         caps->max_texture_samplers = caps->max_sampler_views = 8;
         /* mediump and lowp in 16 bits (F16 iteration, fmad16, F16/C10
          * uniforms), as the DDK computes them: the varyings, texture and
          * clipping A/B cases are byte-identical to the DDK with it
          * (sgx545_varyab.sh, _texproj, _texshape, _nearclip), and the bench
          * world runs 24-28 FPS against 20-27 in F32.  SGX545_F16=0 keeps F32 */
         caps->fp16 = caps->glsl_16bit_consts = !(sgx545_debug & SGX545_DEBUG_NO_F16);
      }
   }
}

static void
sgx545_init_screen_caps(struct pipe_screen *pscreen)
{
   struct pipe_caps *caps = (struct pipe_caps *)&pscreen->caps;

   u_init_pipe_screen_caps(pscreen, 1);
   caps->graphics = true;
   caps->npot_textures = true;
   /* blending is shader code (STATE.md section 3): any equation per channel */
   caps->blend_equation_separate = true;
   caps->uma = true;
   caps->max_texture_2d_size = 2048;
   caps->max_texture_cube_levels = 12;
   caps->max_texture_3d_levels = 0;
   caps->max_render_targets = 1;
   caps->glsl_feature_level = 120;
   caps->glsl_feature_level_compatibility = 120;
   caps->essl_feature_level = 100;
   caps->max_viewports = 1;
   caps->vendor_id = 0x8086;
   caps->device_id = 0x089b;
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
    * needn't be copied into upload buffers first (u_vbuf's copy was a
    * second memcpy of every gl4es draw) */
   caps->user_vertex_buffers = true;
   /* linear render targets, over powervr_sgx */
   if (sgxws_dev_has_dmabuf(sgx545_screen(pscreen)->dev))
      caps->dmabuf = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT;
}

static bool
sgx545_is_format_supported(struct pipe_screen *pscreen, enum pipe_format format,
                           enum pipe_texture_target target, unsigned sample_count,
                           unsigned storage_sample_count, unsigned usage)
{
   if (sample_count > 1 || MAX2(1, sample_count) != MAX2(1, storage_sample_count))
      return false;

   if (usage & (PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SCANOUT | PIPE_BIND_SHARED)) {
      /* windows and pbuffers: the PBE's BGRA8888 (STATE.md section 4) */
      if (format != PIPE_FORMAT_B8G8R8A8_UNORM && format != PIPE_FORMAT_B8G8R8X8_UNORM)
         return false;
   }
   if (usage & PIPE_BIND_RENDER_TARGET) {
      /* the PBE's formats (sgx545_resource_target).  Not 565: st would then
       * pick it for GL_RGB565 textures, and T1 0x05 hands the compiler's
       * unpack R and B swapped; RGBA8 stands in */
      if (format != PIPE_FORMAT_B8G8R8A8_UNORM && format != PIPE_FORMAT_B8G8R8X8_UNORM &&
          format != PIPE_FORMAT_R8G8B8A8_UNORM && format != PIPE_FORMAT_R8G8B8X8_UNORM)
         return false;
   }
   if (usage & PIPE_BIND_DEPTH_STENCIL) {
      /* The ISP keeps depth and stencil per tile and no buffer is loaded or
       * stored (no ZLS yet), so they are good within one render. */
      if (format != PIPE_FORMAT_Z16_UNORM && format != PIPE_FORMAT_Z24X8_UNORM &&
          format != PIPE_FORMAT_Z24_UNORM_S8_UINT)
         return false;
   }
   if (usage & PIPE_BIND_VERTEX_BUFFER) {
      /* gathered on the CPU, so anything util_format can unpack */
      const struct util_format_description *desc = util_format_description(format);

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
   if ((usage & PIPE_BIND_SAMPLER_VIEW) && !sgx545_tex_format(format) &&
       !(usage & PIPE_BIND_RENDER_TARGET) && format != PIPE_FORMAT_B8G8R8A8_UNORM &&
       format != PIPE_FORMAT_B8G8R8X8_UNORM && format != PIPE_FORMAT_R8G8B8A8_UNORM &&
       format != PIPE_FORMAT_R8G8B8X8_UNORM)
      return false;
   switch (target) {
   case PIPE_BUFFER:
   case PIPE_TEXTURE_1D:      /* a 2D texture a texel high, sampled on that row (sgx545_program.c) */
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
} kept = { .fd = -1 };
static simple_mtx_t kept_lock = SIMPLE_MTX_INITIALIZER;

static void
sgx545_screen_destroy(struct pipe_screen *pscreen)
{
   struct sgx545_screen *screen = sgx545_screen(pscreen);

   /* Keep the services connection and a hardware context for the process's
    * next screen instead of tearing them down: when srvinit started the
    * microkernel, a render context's cleanup never completes (the bridge
    * returns -EINVAL after its wait), and the next connection's TA then runs
    * against a parameter buffer the microkernel doesn't use: no pages
    * allocated, nothing drawn, renders waiting ~270 ms, then lockups (gl4es
    * initialises EGL, terminates it and initialises it again).  The process's
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
sgx545_screen_create(int fd, const struct pipe_screen_config *config, struct renderonly *ro)
{
   struct sgx545_screen *screen = CALLOC_STRUCT(sgx545_screen);

   if (!screen)
      return NULL;
   sgx545_debug = debug_get_option_sgx545_debug();
   /* The GL linker's varying optimisation (nir_opt_varyings) packs varyings
    * into shared slots whatever the driver asks: gl4es's fog went in front of
    * a texture coordinate, so neither texture read could be done by the PDS
    * (a projected read needs a whole vec4 varying at component 0) and both
    * became dependent samples, most of a Minecraft frame's GPU time.  The
    * DDK's compiler keeps varyings as declared; so do we, for this process,
    * unless the user set it. */
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
      .verbose = (sgx545_debug & SGX545_DEBUG_FRAME) != 0,
      .flush_rt = (sgx545_debug & SGX545_DEBUG_FLUSH_RT) != 0,
      .trace = debug_get_option_sgx545_trace(),
      .trace_kicks = debug_get_option_sgx545_trace_kicks(),
      .dry_run = debug_get_option_sgx545_dry_run(),
      .frame_gap_us = debug_get_option_sgx545_frame_gap_us(),
   };

   if (screen->dev_fd == fd && !(screen->dev = sgxws_dev_create(fd, &wsopts))) {
      FREE(screen);
      return NULL;
   }
   screen->ro = ro;
   simple_mtx_init(&screen->lock, mtx_plain);
   slab_create_parent(&screen->transfer_pool, sizeof(struct sgx545_transfer), 16);

   screen->base.destroy = sgx545_screen_destroy;
   screen->base.get_name = sgx545_get_name;
   screen->base.get_vendor = sgx545_get_vendor;
   screen->base.get_device_vendor = sgx545_get_device_vendor;
   screen->base.get_screen_fd = sgx545_get_screen_fd;
   screen->base.is_format_supported = sgx545_is_format_supported;
   screen->base.context_create = sgx545_context_create;
   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++)
      screen->base.nir_options[i] = sgx545_get_nir_options();

   sgx545_resource_screen_init(screen);
   sgx545_fence_screen_init(screen);
   sgx545_init_shader_caps(&screen->base);
   sgx545_init_screen_caps(&screen->base);
   return &screen->base;
}
