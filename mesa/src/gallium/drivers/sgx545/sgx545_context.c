/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

#include "util/u_debug.h"
#include "util/u_debug_cb.h"
#include "util/u_framebuffer.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_upload_mgr.h"

#include "sgx545_context.h"
#include "sgx545_resource.h"
#include "sgx545_screen.h"

/* Every frame is waited for: rendering is synchronous for now, so a fence
 * is signalled by the time it exists. */
struct sgx545_fence {
   struct pipe_reference reference;
};

static void
sgx545_fence_reference(struct pipe_screen *pscreen, struct pipe_fence_handle **ptr,
                       struct pipe_fence_handle *fence)
{
   if (pipe_reference((struct pipe_reference *)*ptr, (struct pipe_reference *)fence))
      FREE(*ptr);
   *ptr = fence;
}

static bool
sgx545_fence_finish(struct pipe_screen *pscreen, struct pipe_context *pctx,
                    struct pipe_fence_handle *fence, uint64_t timeout)
{
   return true;
}

void
sgx545_flush_frame(struct sgx545_context *ctx)
{
   struct sgx545_screen *screen = sgx545_screen(ctx->base.screen);
   int e;

   if (!ctx->recording)
      return;
   simple_mtx_lock(&screen->lock);
   e = sgxws_flush(ctx->hw, 2000);
   simple_mtx_unlock(&screen->lock);
   if (e)
      fprintf(stderr, "sgx545: frame did not finish: %d\n", e);
   ctx->recording = NULL;
}

void
sgx545_flush_resource_writes(struct sgx545_context *ctx, struct sgx545_resource *res)
{
   if (ctx->recording == res)
      sgx545_flush_frame(ctx);
}

static void
sgx545_flush(struct pipe_context *pctx, struct pipe_fence_handle **fence, unsigned flags)
{
   struct sgx545_context *ctx = sgx545_context(pctx);

   sgx545_flush_frame(ctx);
   if (fence) {
      struct sgx545_fence *f = CALLOC_STRUCT(sgx545_fence);

      pipe_reference_init(&f->reference, 1);
      *fence = NULL;
      sgx545_fence_reference(pctx->screen, fence, (struct pipe_fence_handle *)f);
      sgx545_fence_reference(pctx->screen, (struct pipe_fence_handle **)&f, NULL);
   }
}

static void
sgx545_flush_resource(struct pipe_context *pctx, struct pipe_resource *prsc)
{
   sgx545_flush_resource_writes(sgx545_context(pctx), sgx545_resource(prsc));
}

static void
sgx545_context_destroy(struct pipe_context *pctx)
{
   struct sgx545_context *ctx = sgx545_context(pctx);
   struct sgx545_screen *screen = sgx545_screen(pctx->screen);

   sgx545_flush_frame(ctx);
   util_unreference_framebuffer_state(&ctx->framebuffer);
   for (unsigned i = 0; i < ctx->num_vertex_buffers; i++)
      pipe_vertex_buffer_unreference(&ctx->vertex_buffers[i]);
   for (unsigned i = 0; i < MESA_SHADER_STAGES; i++)
      pipe_resource_reference(&ctx->constants[i].buffer, NULL);
   for (unsigned i = 0; i < PIPE_MAX_SHADER_SAMPLER_VIEWS; i++)
      pipe_sampler_view_reference(&ctx->views[i], NULL);
   if (pctx->stream_uploader)
      u_upload_destroy(pctx->stream_uploader);

   simple_mtx_lock(&screen->lock);
   if (ctx->hw) {
      for (unsigned i = 0; i < ctx->num_rts; i++)
         sgxws_rt_destroy(ctx->hw, ctx->rts[i]);   /* flushes this context's frame */
   }
   simple_mtx_unlock(&screen->lock);

   slab_destroy_child(&ctx->transfer_pool);
   free(ctx->staging);
   FREE(ctx);
}

struct pipe_context *
sgx545_context_create(struct pipe_screen *pscreen, void *priv, unsigned flags)
{
   struct sgx545_screen *screen = sgx545_screen(pscreen);
   struct sgx545_context *ctx = CALLOC_STRUCT(sgx545_context);

   if (!ctx)
      return NULL;

   simple_mtx_lock(&screen->lock);
   if (!screen->hw)
      screen->hw = sgxws_ctx_create(&screen->dev);
   ctx->hw = screen->hw;
   if (ctx->hw) {
      sgxws_prog_builtin(ctx->hw, SGXWS_VS_XY, &ctx->vs_xy);
      sgxws_prog_builtin(ctx->hw, SGXWS_VS_XYZ, &ctx->vs_xyz);
      sgxws_prog_builtin(ctx->hw, SGXWS_FS_UNIFORM, &ctx->fs_uniform);
   }
   simple_mtx_unlock(&screen->lock);
   if (!ctx->hw) {
      FREE(ctx);
      return NULL;
   }

   ctx->base.screen = pscreen;
   ctx->base.priv = priv;
   ctx->base.destroy = sgx545_context_destroy;
   ctx->base.flush = sgx545_flush;
   ctx->base.flush_resource = sgx545_flush_resource;
   ctx->base.set_debug_callback = u_default_set_debug_callback;

   slab_create_child(&ctx->transfer_pool, &screen->transfer_pool);
   sgx545_resource_context_init(ctx);
   sgx545_state_init(ctx);
   sgx545_draw_init(ctx);
   sgx545_program_init(ctx);

   ctx->base.stream_uploader = u_upload_create_default(&ctx->base);
   if (!ctx->base.stream_uploader) {
      sgx545_context_destroy(&ctx->base);
      return NULL;
   }
   ctx->base.const_uploader = ctx->base.stream_uploader;
   return &ctx->base;
}

void
sgx545_fence_screen_init(struct sgx545_screen *screen)
{
   screen->base.fence_reference = sgx545_fence_reference;
   screen->base.fence_finish = sgx545_fence_finish;
}
