/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

#include <time.h>

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

/* A fence: the renders submitted before it, up to its kick number (renders
 * finish in order).  Waiting for idle here made the swap's throttle (DRI
 * waits on the previous frame's fence) wait for the frame just submitted, so
 * CPU and GPU took turns every frame. */
struct sgx545_fence {
   struct pipe_reference reference;
   uint32_t seq;
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

   simple_mtx_lock(&screen->lock);
   done = !screen->hw ||
          !sgxws_wait_seq(screen->hw, ((struct sgx545_fence *)fence)->seq, timeout ? 2000 : 0);
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
   uint64_t now = os_time_get_nano(), dt = now - ctx->stats.t0, cpu;
   double ms = 1e-6;
   struct timespec ts;

   if (dt < 2000000000ull)
      return;
   /* the thread's own CPU time: the rest of the window it waited */
   clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
   cpu = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
   fprintf(stderr, "sgx545 stats: thread CPU %.0f ms of %.0f; flush %.0f: TA command %.0f, kick %.0f, target wait %.0f, "
           "slot wait %.0f, sync wait %.0f ms\n",
           (cpu - ctx->stats.cpu0) * ms, dt * ms, ctx->hw ? ctx->hw->ns_flush * ms : 0,
           ctx->hw ? ctx->hw->ns_cmd * ms : 0, ctx->hw ? ctx->hw->ns_kick * ms : 0,
           ctx->hw ? ctx->hw->ns_target_wait * ms : 0, ctx->hw ? ctx->hw->ns_slot_wait * ms : 0,
           ctx->hw ? ctx->hw->ns_sync_wait * ms : 0);
   if (ctx->hw)
      ctx->hw->ns_flush = ctx->hw->ns_cmd = ctx->hw->ns_sync_wait = 0;
   fprintf(stderr, "sgx545 stats: renders by cause: end of frame %u, other flush %u, target switch %u, "
           "readback %u, deferred %u; vertex cache %u hits, %u misses, %u not cacheable, %u KB; fence waits %.0f ms; render latency (kick to seen done) %.1f ms over %u\n",
           ctx->stats.fl_eof, ctx->stats.fl_other, ctx->stats.fl_switch, ctx->stats.fl_read,
           ctx->stats.fl_deferred, ctx->stats.vc_hits, ctx->stats.vc_misses, ctx->stats.vc_uncacheable,
           (unsigned)(ctx->vcache_bytes >> 10), ctx->hw ? ctx->hw->ns_fence_wait * ms : 0,
           ctx->hw && ctx->hw->n_latency ? ctx->hw->ns_latency * ms / ctx->hw->n_latency : 0,
           ctx->hw ? ctx->hw->n_latency : 0);
   if (ctx->hw) {
      ctx->hw->ns_kick = ctx->hw->ns_target_wait = ctx->hw->ns_slot_wait = 0;
      ctx->hw->ns_fence_wait = ctx->hw->ns_latency = 0;
      ctx->hw->n_latency = 0;
   }
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
   ctx->stats.cpu0 = cpu;
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
   /* A plain flush (glFlush) of an off-screen frame is deferred, as GL
    * allows: submitting it split the frame, and the draws after it into the
    * same target (the openhc-minecraft overlay's, after the game's glFlush)
    * waited for the whole render.  The frame still goes at a fence, a
    * readback, another target or the end of the frame; renders into the
    * scanout buffer (the swap's copy) go now. */
   if (ctx->recording && !fence && !(flags & PIPE_FLUSH_END_OF_FRAME) && !ctx->recording->wrapped &&
       !debug_get_bool_option("SGX545_FLUSH_ALL", false)) {
      ctx->stats.fl_deferred++;
      return;
   }
   if (ctx->recording) {
      if (flags & PIPE_FLUSH_END_OF_FRAME)
         ctx->stats.fl_eof++;
      else
         ctx->stats.fl_other++;
   }
   sgx545_flush_frame(ctx);
   if (fence) {
      struct sgx545_fence *f = CALLOC_STRUCT(sgx545_fence);

      pipe_reference_init(&f->reference, 1);
      f->seq = ctx->hw ? ctx->hw->kicks : 0;
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
   sgx545_draw_fini(ctx);
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
