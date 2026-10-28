/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

#include "util/os_time.h"
#include "util/u_debug.h"
#include "util/u_debug_cb.h"
#include "util/u_framebuffer.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_upload_mgr.h"

#include "sgx545_context.h"
#include "sgx545_resource.h"
#include "sgx545_screen.h"

/* A fence is every render submitted before it (they run in order). */
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
   struct sgx545_screen *screen = sgx545_screen(pscreen);
   bool done;

   /* every render submitted before the fence: renders run in order */
   simple_mtx_lock(&screen->lock);
   done = !screen->hw || !sgxws_wait_idle(screen->hw, timeout ? 2000 : 0);
   simple_mtx_unlock(&screen->lock);
   return done;
}

void
sgx545_flush_frame(struct sgx545_context *ctx)
{
   struct sgx545_screen *screen = sgx545_screen(ctx->base.screen);
   int e;

   uint64_t t;

   if (!ctx->recording)
      return;
   t = ctx->stats.on ? os_time_get_nano() : 0;
   simple_mtx_lock(&screen->lock);
   /* SGX545_SYNC=1: wait for every frame's render (as before frames overlapped) */
   e = sgxws_flush(ctx->hw, debug_get_bool_option("SGX545_SYNC", false) ? 2000 : 0);
   simple_mtx_unlock(&screen->lock);
   if (ctx->stats.on) {
      ctx->stats.flush_ns += os_time_get_nano() - t;
      ctx->stats.flushes++;
      sgx545_stats_report(ctx);
   }
   if (e) {
      fprintf(stderr, "sgx545: frame did not finish: %d\n", e);
      /* SGX545_HANG_ABORT=1: stop the client at the first hang, before the
       * recovery loop wedges the GPU (debugging on a shared box) */
      if (debug_get_bool_option("SGX545_HANG_ABORT", false))
         abort();
   }
   ctx->recording = NULL;
}

void
sgx545_finish(struct sgx545_context *ctx)
{
   struct sgx545_screen *screen = sgx545_screen(ctx->base.screen);

   sgx545_flush_frame(ctx);
   simple_mtx_lock(&screen->lock);
   if (ctx->hw && sgxws_wait_idle(ctx->hw, 2000))
      fprintf(stderr, "sgx545: a render did not finish\n");
   simple_mtx_unlock(&screen->lock);
}

void
sgx545_stats_report(struct sgx545_context *ctx)
{
   uint64_t now = os_time_get_nano(), dt = now - ctx->stats.t0;
   double ms = 1e-6;

   if (dt < 2000000000ull)
      return;
   fprintf(stderr, "sgx545 stats %.1f s: %.1f FPS, %u flushes (%u for texture writes, %u rings full), %u draws, "
           "%u vertices, %u texture writes; ms: draw %.0f, flush+wait %.0f, texture %.0f, compile %.0f\n",
           dt * 1e-9, ctx->stats.frames / (dt * 1e-9), ctx->stats.flushes, ctx->stats.tex_flushes, ctx->hw ? ctx->hw->room_flushes : 0,
           ctx->stats.draws, ctx->stats.verts, ctx->stats.tex_writes,
           ctx->stats.draw_ns * ms, ctx->stats.flush_ns * ms, ctx->stats.tex_ns * ms, ctx->stats.compile_ns * ms);
   memset(&ctx->stats, 0, sizeof(ctx->stats));
   if (ctx->hw)
      ctx->hw->room_flushes = 0;
   ctx->stats.on = true;
   ctx->stats.t0 = now;
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

   if (flags & PIPE_FLUSH_END_OF_FRAME)
      ctx->stats.frames++;
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

   sgx545_finish(ctx);
   if (ctx->blitter)
      util_blitter_destroy(ctx->blitter);
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
   free(ctx->idx_staging);
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
   ctx->stats.on = debug_get_bool_option("SGX545_STATS", false);
   ctx->stats.t0 = os_time_get_nano();
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
   /* after every hook it creates state through */
   ctx->blitter = util_blitter_create(&ctx->base);
   return &ctx->base;
}

void
sgx545_fence_screen_init(struct sgx545_screen *screen)
{
   screen->base.fence_reference = sgx545_fence_reference;
   screen->base.fence_finish = sgx545_fence_finish;
}
