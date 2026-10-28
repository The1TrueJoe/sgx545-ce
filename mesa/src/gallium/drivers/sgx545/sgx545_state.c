/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

#include "util/u_framebuffer.h"
#include "util/u_helpers.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"

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
}

static void
sgx545_texture_barrier(struct pipe_context *pctx, unsigned flags)
{
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
}
