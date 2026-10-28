/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <unistd.h>

#include "util/format/u_format.h"
#include "util/u_debug.h"
#include "util/u_memory.h"
#include "util/u_screen.h"

#include "sgx545_context.h"
#include "sgx545_program.h"
#include "sgx545_resource.h"
#include "sgx545_screen.h"

uint32_t sgx545_debug;

static const struct debug_named_value sgx545_debug_options[] = {
   { "frame", SGX545_DEBUG_FRAME, "print every frame the winsys submits" },
   { "nir", SGX545_DEBUG_NIR, "print every shader's NIR" },
   { "draw", SGX545_DEBUG_DRAW, "print every draw" },
   { "sync", SGX545_DEBUG_SYNC, "print allocations and waits" },
   DEBUG_NAMED_VALUE_END
};

DEBUG_GET_ONCE_FLAGS_OPTION(sgx545_debug, "SGX545_DEBUG", sgx545_debug_options, 0)


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

      caps->max_instructions = caps->max_alu_instructions = caps->max_tex_instructions =
         caps->max_tex_indirections = 16384;
      caps->max_control_flow_depth = 32;
      /* ISA.md: 16 attributes, 10 texture coordinate sets */
      caps->max_inputs = s == MESA_SHADER_VERTEX ? 16 : 10;
      caps->max_outputs = s == MESA_SHADER_VERTEX ? 11 : 1;
      caps->max_const_buffer0_size = 128 * 4 * sizeof(float);
      caps->max_const_buffers = 1;
      caps->max_temps = 64;
      if (s == MESA_SHADER_FRAGMENT)
         caps->max_texture_samplers = caps->max_sampler_views = 8;
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
   caps->max_point_size = caps->max_point_size_aa = 64;
   caps->point_size_granularity = caps->line_width_granularity = 0.1f;
   caps->max_texture_lod_bias = 16;
   caps->alpha_test = true;
   caps->clip_planes = 0;
   caps->texture_transfer_modes = 0;
   caps->max_vertex_element_src_offset = 2047;
}

static bool
sgx545_is_format_supported(struct pipe_screen *pscreen, enum pipe_format format,
                           enum pipe_texture_target target, unsigned sample_count,
                           unsigned storage_sample_count, unsigned usage)
{
   if (sample_count > 1 || MAX2(1, sample_count) != MAX2(1, storage_sample_count))
      return false;

   if (usage & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SCANOUT |
                PIPE_BIND_SHARED)) {
      /* the PBE's BGRA8888 (STATE.md section 4) */
      if (format != PIPE_FORMAT_B8G8R8A8_UNORM && format != PIPE_FORMAT_B8G8R8X8_UNORM)
         return false;
   }
   if (usage & PIPE_BIND_DEPTH_STENCIL) {
      /* The ISP keeps depth per tile and no depth buffer is loaded or stored
       * (no ZLS yet), so depth is good within one render. */
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
   if (usage & PIPE_BIND_SAMPLER_VIEW) {
      switch (format) {
      case PIPE_FORMAT_R8G8B8A8_UNORM:
      case PIPE_FORMAT_R8G8B8X8_UNORM:
      case PIPE_FORMAT_B8G8R8A8_UNORM:
      case PIPE_FORMAT_B8G8R8X8_UNORM:
      case PIPE_FORMAT_B5G6R5_UNORM:
      case PIPE_FORMAT_B4G4R4A4_UNORM:
      case PIPE_FORMAT_B5G5R5A1_UNORM:
      case PIPE_FORMAT_A8_UNORM:
      case PIPE_FORMAT_L8_UNORM:
      case PIPE_FORMAT_L8A8_UNORM:
         break;
      default:
         return false;
      }
   }
   switch (target) {
   case PIPE_BUFFER:
   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_RECT:
   case PIPE_TEXTURE_CUBE:
      return true;
   default:
      return false;
   }
}

static void
sgx545_screen_destroy(struct pipe_screen *pscreen)
{
   struct sgx545_screen *screen = sgx545_screen(pscreen);

   sgxws_dev_fini(&screen->dev);
   slab_destroy_parent(&screen->transfer_pool);
   simple_mtx_destroy(&screen->lock);
   close(screen->fd);
   FREE(screen);
}

struct pipe_screen *
sgx545_screen_create(int fd, const struct pipe_screen_config *config)
{
   struct sgx545_screen *screen = CALLOC_STRUCT(sgx545_screen);
   int e;

   if (!screen)
      return NULL;
   sgx545_debug = debug_get_option_sgx545_debug();
   screen->fd = fd;
   if ((e = sgxws_dev_init(&screen->dev, fd))) {
      fprintf(stderr, "sgx545: no PVR services on fd %d: %d\n", fd, e);
      FREE(screen);
      return NULL;
   }
   if (sgx545_debug & SGX545_DEBUG_FRAME)
      screen->dev.verbose = 1;
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
