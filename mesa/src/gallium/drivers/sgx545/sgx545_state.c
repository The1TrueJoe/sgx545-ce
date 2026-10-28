/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

#include "util/u_framebuffer.h"
#include "util/u_helpers.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_surface.h"
#include "util/format/u_format.h"

#include "sgx545_context.h"
#include "sgx545_resource.h"

static void *
sgx545_create_blend_state(struct pipe_context *pctx, const struct pipe_blend_state *cso)
{
   struct sgx545_blend *so = CALLOC_STRUCT(sgx545_blend);

   if (so)
      so->base = *cso;
   return so;
}

static void
sgx545_bind_blend_state(struct pipe_context *pctx, void *hwcso)
{
   sgx545_context(pctx)->blend = hwcso;
}

static void *
sgx545_create_rasterizer_state(struct pipe_context *pctx, const struct pipe_rasterizer_state *cso)
{
   struct sgx545_rasterizer *so = CALLOC_STRUCT(sgx545_rasterizer);

   if (so)
      so->base = *cso;
   return so;
}

static void
sgx545_bind_rasterizer_state(struct pipe_context *pctx, void *hwcso)
{
   sgx545_context(pctx)->rasterizer = hwcso;
}

static void *
sgx545_create_dsa_state(struct pipe_context *pctx, const struct pipe_depth_stencil_alpha_state *cso)
{
   struct sgx545_dsa *so = CALLOC_STRUCT(sgx545_dsa);

   if (so)
      so->base = *cso;
   return so;
}

static void
sgx545_bind_dsa_state(struct pipe_context *pctx, void *hwcso)
{
   sgx545_context(pctx)->dsa = hwcso;
}

static void
sgx545_delete_state(struct pipe_context *pctx, void *hwcso)
{
   FREE(hwcso);
}

static void *
sgx545_create_vertex_elements(struct pipe_context *pctx, unsigned num_elements,
                              const struct pipe_vertex_element *elements)
{
   struct sgx545_vertex_elements *so = CALLOC_STRUCT(sgx545_vertex_elements);

   if (!so)
      return NULL;
   so->num_elements = num_elements;
   memcpy(so->elements, elements, num_elements * sizeof(*elements));
   return so;
}

static void
sgx545_bind_vertex_elements(struct pipe_context *pctx, void *hwcso)
{
   sgx545_context(pctx)->velems = hwcso;
}

static void
sgx545_set_vertex_buffers(struct pipe_context *pctx, unsigned count,
                          const struct pipe_vertex_buffer *vb)
{
   struct sgx545_context *ctx = sgx545_context(pctx);

   util_set_vertex_buffers_count(ctx->vertex_buffers, &ctx->num_vertex_buffers, vb, count);
}

static void
sgx545_set_constant_buffer(struct pipe_context *pctx, mesa_shader_stage shader, uint index,
                           const struct pipe_constant_buffer *cb)
{
   struct sgx545_context *ctx = sgx545_context(pctx);
   struct pipe_constant_buffer *so = &ctx->constants[shader];

   if (index != 0)
      return;
   if (!cb) {
      pipe_resource_reference(&so->buffer, NULL);
      memset(so, 0, sizeof(*so));
      return;
   }
   pipe_resource_reference(&so->buffer, cb->buffer);
   so->buffer_offset = cb->buffer_offset;
   so->buffer_size = cb->buffer_size;
   so->user_buffer = cb->user_buffer;
}

static void
sgx545_set_framebuffer_state(struct pipe_context *pctx, const struct pipe_framebuffer_state *fb)
{
   struct sgx545_context *ctx = sgx545_context(pctx);

   util_copy_framebuffer_state(&ctx->framebuffer, fb);
}

static void
sgx545_set_viewport_states(struct pipe_context *pctx, unsigned start_slot, unsigned num_viewports,
                           const struct pipe_viewport_state *viewport)
{
   if (start_slot == 0 && num_viewports)
      sgx545_context(pctx)->viewport = viewport[0];
}

static void
sgx545_set_scissor_states(struct pipe_context *pctx, unsigned start_slot, unsigned num_scissors,
                          const struct pipe_scissor_state *scissor)
{
   /* pass-type-3 objects and region clips (STATE.md section 3): not yet */
}

static void
sgx545_set_blend_color(struct pipe_context *pctx, const struct pipe_blend_color *color)
{
}

static void
sgx545_set_stencil_ref(struct pipe_context *pctx, const struct pipe_stencil_ref ref)
{
}

static void
sgx545_set_sample_mask(struct pipe_context *pctx, unsigned sample_mask)
{
}

static void
sgx545_set_polygon_stipple(struct pipe_context *pctx, const struct pipe_poly_stipple *stipple)
{
}

static void
sgx545_set_clip_state(struct pipe_context *pctx, const struct pipe_clip_state *clip)
{
}

static void *
sgx545_create_sampler_state(struct pipe_context *pctx, const struct pipe_sampler_state *cso)
{
   struct pipe_sampler_state *so = CALLOC_STRUCT(pipe_sampler_state);

   if (so)
      *so = *cso;
   return so;
}

static void
sgx545_bind_sampler_states(struct pipe_context *pctx, mesa_shader_stage shader, unsigned start,
                           unsigned nr, void **hwcso)
{
   struct sgx545_context *ctx = sgx545_context(pctx);

   if (shader != MESA_SHADER_FRAGMENT)
      return;
   for (unsigned i = 0; i < nr && start + i < PIPE_MAX_SAMPLERS; i++)
      ctx->samplers[start + i] = hwcso ? hwcso[i] : NULL;
}

static struct pipe_sampler_view *
sgx545_create_sampler_view(struct pipe_context *pctx, struct pipe_resource *prsc,
                           const struct pipe_sampler_view *cso)
{
   struct pipe_sampler_view *so = CALLOC_STRUCT(pipe_sampler_view);

   if (!so)
      return NULL;
   *so = *cso;
   so->texture = NULL;
   pipe_resource_reference(&so->texture, prsc);
   so->reference.count = 1;
   so->context = pctx;
   return so;
}

static void
sgx545_sampler_view_destroy(struct pipe_context *pctx, struct pipe_sampler_view *view)
{
   pipe_resource_reference(&view->texture, NULL);
   FREE(view);
}

static void
sgx545_set_sampler_views(struct pipe_context *pctx, mesa_shader_stage shader, unsigned start,
                         unsigned nr, unsigned unbind_num_trailing_slots,
                         struct pipe_sampler_view **views)
{
   struct sgx545_context *ctx = sgx545_context(pctx);

   if (shader != MESA_SHADER_FRAGMENT)
      return;
   for (unsigned i = 0; i < nr + unbind_num_trailing_slots; i++) {
      unsigned s = start + i;

      if (s < PIPE_MAX_SHADER_SAMPLER_VIEWS)
         pipe_sampler_view_reference(&ctx->views[s], i < nr && views ? views[i] : NULL);
   }
}

static void
sgx545_texture_barrier(struct pipe_context *pctx, unsigned flags)
{
}

/* queries: none of GLES2's need the GPU yet */
struct sgx545_query {
   unsigned type;
};

static struct pipe_query *
sgx545_create_query(struct pipe_context *pctx, unsigned query_type, unsigned index)
{
   struct sgx545_query *q = CALLOC_STRUCT(sgx545_query);

   if (q)
      q->type = query_type;
   return (struct pipe_query *)q;
}

static void
sgx545_destroy_query(struct pipe_context *pctx, struct pipe_query *q)
{
   FREE(q);
}

static bool
sgx545_begin_end_query(struct pipe_context *pctx, struct pipe_query *q)
{
   return true;
}

static bool
sgx545_get_query_result(struct pipe_context *pctx, struct pipe_query *q, bool wait,
                        union pipe_query_result *result)
{
   memset(result, 0, sizeof(*result));
   return true;
}

static void
sgx545_set_active_query_state(struct pipe_context *pctx, bool enable)
{
}

/* copies on the CPU: textures and buffers are CPU-visible, render targets are
 * waited for by the map */
static void
sgx545_resource_copy_region(struct pipe_context *pctx, struct pipe_resource *dst, unsigned dst_level,
                            unsigned dstx, unsigned dsty, unsigned dstz, struct pipe_resource *src,
                            unsigned src_level, const struct pipe_box *src_box)
{
   util_resource_copy_region(pctx, dst, dst_level, dstx, dsty, dstz, src, src_level, src_box);
}

static void
sgx545_blit(struct pipe_context *pctx, const struct pipe_blit_info *info)
{
   if (util_try_blit_via_copy_region(pctx, info, false))
      return;
   fprintf(stderr, "sgx545: blit %s -> %s not supported yet\n",
           util_format_short_name(info->src.format), util_format_short_name(info->dst.format));
}

void
sgx545_state_init(struct sgx545_context *ctx)
{
   struct pipe_context *p = &ctx->base;

   p->create_blend_state = sgx545_create_blend_state;
   p->bind_blend_state = sgx545_bind_blend_state;
   p->delete_blend_state = sgx545_delete_state;
   p->create_rasterizer_state = sgx545_create_rasterizer_state;
   p->bind_rasterizer_state = sgx545_bind_rasterizer_state;
   p->delete_rasterizer_state = sgx545_delete_state;
   p->create_depth_stencil_alpha_state = sgx545_create_dsa_state;
   p->bind_depth_stencil_alpha_state = sgx545_bind_dsa_state;
   p->delete_depth_stencil_alpha_state = sgx545_delete_state;
   p->create_vertex_elements_state = sgx545_create_vertex_elements;
   p->bind_vertex_elements_state = sgx545_bind_vertex_elements;
   p->delete_vertex_elements_state = sgx545_delete_state;
   p->set_vertex_buffers = sgx545_set_vertex_buffers;
   p->set_constant_buffer = sgx545_set_constant_buffer;
   p->set_framebuffer_state = sgx545_set_framebuffer_state;
   p->set_viewport_states = sgx545_set_viewport_states;
   p->set_scissor_states = sgx545_set_scissor_states;
   p->set_blend_color = sgx545_set_blend_color;
   p->set_stencil_ref = sgx545_set_stencil_ref;
   p->set_sample_mask = sgx545_set_sample_mask;
   p->set_polygon_stipple = sgx545_set_polygon_stipple;
   p->set_clip_state = sgx545_set_clip_state;
   p->create_sampler_state = sgx545_create_sampler_state;
   p->bind_sampler_states = sgx545_bind_sampler_states;
   p->delete_sampler_state = sgx545_delete_state;
   p->create_sampler_view = sgx545_create_sampler_view;
   p->sampler_view_destroy = sgx545_sampler_view_destroy;
   p->sampler_view_release = u_default_sampler_view_release;
   p->set_sampler_views = sgx545_set_sampler_views;
   p->texture_barrier = sgx545_texture_barrier;
   p->resource_release = u_default_resource_release;
   p->create_query = sgx545_create_query;
   p->destroy_query = sgx545_destroy_query;
   p->begin_query = sgx545_begin_end_query;
   p->end_query = sgx545_begin_end_query;
   p->get_query_result = sgx545_get_query_result;
   p->set_active_query_state = sgx545_set_active_query_state;
   p->resource_copy_region = sgx545_resource_copy_region;
   p->blit = sgx545_blit;
}
