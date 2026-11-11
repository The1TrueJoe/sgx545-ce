/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <time.h>

#include "util/os_misc.h"
#include "util/os_time.h"
#include "util/u_debug.h"
#include "util/u_debug_cb.h"
#include "util/u_framebuffer.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_upload_mgr.h"

#include "sgx_context.h"
#include "sgx_resource.h"
#include "sgx_screen.h"

/* A fence: the renders submitted before it, up to its kick number (renders
 * finish in order).  Waiting for idle here made the swap's throttle (DRI
 * waits on the previous frame's fence) wait for the frame just submitted, so
 * CPU and GPU took turns every frame. */
struct sgx_fence {
   struct pipe_reference reference;
   uint32_t seq;
};

static void
sgx_fence_reference(struct pipe_screen *pscreen, struct pipe_fence_handle **ptr,
                    struct pipe_fence_handle *fence)
{
   if (pipe_reference((struct pipe_reference *)*ptr,
                      (struct pipe_reference *)fence))
      FREE(*ptr);
   *ptr = fence;
}

static bool
sgx_fence_finish(struct pipe_screen *pscreen, struct pipe_context *pctx,
                 struct pipe_fence_handle *fence, uint64_t timeout)
{
   struct sgx_screen *screen = sgx_screen(pscreen);
   bool done;

   simple_mtx_lock(&screen->lock);
   done = !screen->hw ||
          !sgxws_wait_seq(screen->hw, ((struct sgx_fence *)fence)->seq,
                          timeout ? 2000 : 0);
   simple_mtx_unlock(&screen->lock);
   return done;
}

struct sgx_resource *
sgx_recording(struct sgx_context *ctx)
{
   if (ctx->recording &&
       (!ctx->hw || sgxws_target_bo(ctx->hw) != &ctx->recording->bo))
      ctx->recording = NULL;
   return ctx->recording;
}

void
sgx_flush_frame(struct sgx_context *ctx)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);
   int e;

   uint64_t t;

   if (!sgx_recording(ctx))
      return;
   t = ctx->stats.on ? os_time_get_nano() : 0;
   simple_mtx_lock(&screen->lock);
   /* SGX_DEBUG=serial: wait for every frame's render (as before frames
    * overlapped) */
   e = sgxws_flush(ctx->hw, (sgx_drv_debug & SGX_DEBUG_SERIAL) ? 2000 : 0);
   sgx_check_reset(ctx);
   simple_mtx_unlock(&screen->lock);
   if (ctx->stats.on) {
      ctx->stats.flush_ns += os_time_get_nano() - t;
      ctx->stats.flushes++;
      sgx_stats_report(ctx);
   }
   if (e) {
      fprintf(stderr, "sgx: frame did not finish: %d\n", e);
      /* SGX_DEBUG=hangabort: stop the client at the first hang, before
       * the recovery loop wedges the GPU (debugging on a shared box) */
      if (sgx_drv_debug & SGX_DEBUG_HANG_ABORT)
         abort();
   }
   ctx->recording = NULL;
}

void
sgx_finish(struct sgx_context *ctx)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);

   sgx_flush_frame(ctx);
   simple_mtx_lock(&screen->lock);
   if (ctx->hw && sgxws_wait_idle(ctx->hw, 2000))
      fprintf(stderr, "sgx: a render did not finish\n");
   simple_mtx_unlock(&screen->lock);
}

void
sgx_stats_report(struct sgx_context *ctx)
{
   uint64_t now = os_time_get_nano(), dt = now - ctx->stats.t0, cpu;
   double ms = 1e-6;
   struct sgxws_stats ws = {0};
   struct timespec ts;

   if (dt < 2000000000ull)
      return;
   /* the thread's own CPU time: the rest of the window it waited */
   clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
   cpu = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
   if (ctx->hw)
      sgxws_get_stats(ctx->hw, &ws, true);
   fprintf(
      stderr,
      "sgx stats: thread CPU %.0f ms of %.0f; flush %.0f: TA command %.0f, kick %.0f, target wait %.0f, "
      "slot wait %.0f, sync wait %.0f ms\n",
      (cpu - ctx->stats.cpu0) * ms, dt * ms, ws.ns_flush * ms, ws.ns_cmd * ms,
      ws.ns_kick * ms, ws.ns_target_wait * ms, ws.ns_slot_wait * ms,
      ws.ns_sync_wait * ms);
   fprintf(
      stderr,
      "sgx stats: renders by cause: end of frame %u, other flush %u, target switch %u "
      "(%u of clears only), readback %u, deferred %u; zero-copy draws %u; vertex cache %u hits, %u misses, %u not cacheable, %u KB; fence waits %.0f ms; render latency (kick to seen done) %.1f ms over %u, into scanout %.1f ms over %u\n",
      ctx->stats.fl_eof, ctx->stats.fl_other, ctx->stats.fl_switch,
      ctx->stats.fl_switch_clear, ctx->stats.fl_read, ctx->stats.fl_deferred,
      ctx->stats.zc_draws, ctx->stats.vc_hits, ctx->stats.vc_misses,
      ctx->stats.vc_uncacheable, (unsigned)(ctx->vcache_bytes >> 10),
      ws.ns_fence_wait * ms,
      ws.n_latency ? ws.ns_latency * ms / ws.n_latency : 0, ws.n_latency,
      ws.n_latency_scanout ? ws.ns_latency_scanout * ms / ws.n_latency_scanout
                           : 0,
      ws.n_latency_scanout);
   fprintf(
      stderr,
      "sgx stats: blits: GPU %u, copy %u, CPU %u (%llu px, %.0f ms); textures read back %.0f ms (%llu KB)\n",
      ctx->stats.blit_gpu, ctx->stats.blit_copy, ctx->stats.blit_cpu,
      (unsigned long long)ctx->stats.blit_cpu_px, ctx->stats.blit_cpu_ns * ms,
      ctx->stats.rb_ns * ms, (unsigned long long)(ctx->stats.rb_bytes >> 10));
   fprintf(
      stderr,
      "sgx stats: GPU draws were given %llu indices into %llu vertices (%.0f / %.0f a frame); TA state blocks %u sent, %u skipped\n",
      (unsigned long long)ws.gpu_indices, (unsigned long long)ws.gpu_verts,
      ctx->stats.frames ? (double)ws.gpu_indices / ctx->stats.frames : 0.0,
      ctx->stats.frames ? (double)ws.gpu_verts / ctx->stats.frames : 0.0,
      ws.state_sent, ws.state_skipped);
   fprintf(
      stderr,
      "sgx stats %.1f s: %.1f FPS, %u flushes (%u for texture writes, %u rings full), %u draws, "
      "%u vertices, %u texture writes; ms: draw %.0f, flush+wait %.0f, texture %.0f, compile %.0f\n",
      dt * 1e-9, ctx->stats.frames / (dt * 1e-9), ctx->stats.flushes,
      ctx->stats.tex_flushes, ws.room_flushes, ctx->stats.draws,
      ctx->stats.verts, ctx->stats.tex_writes, ctx->stats.draw_ns * ms,
      ctx->stats.flush_ns * ms, ctx->stats.tex_ns * ms,
      ctx->stats.compile_ns * ms);
   memset(&ctx->stats, 0, sizeof(ctx->stats));
   ctx->stats.on = true;
   ctx->stats.t0 = now;
   ctx->stats.cpu0 = cpu;
}

void
sgx_flush_resource_writes(struct sgx_context *ctx, struct sgx_resource *res)
{
   if (sgx_recording(ctx) == res)
      sgx_flush_frame(ctx);
}

static void
sgx_flush(struct pipe_context *pctx, struct pipe_fence_handle **fence,
          unsigned flags)
{
   struct sgx_context *ctx = sgx_context(pctx);

   if (flags & PIPE_FLUSH_END_OF_FRAME)
      ctx->stats.frames++;
   /* A plain flush (glFlush) of an off-screen frame is deferred, as GL
    * allows: submitting it split the frame, and the draws after it into the
    * same target (an application that flushes, then draws an overlay) waited
    * for the whole render.  The frame still goes at a fence, a
    * readback, another target or the end of the frame; renders into the
    * scanout buffer (the swap's copy) or a shared one (a dma-buf another
    * process or device reads: Xorg's glamor flushes its screen pixmap so)
    * go now. */
   if (sgx_recording(ctx) && !fence && !(flags & PIPE_FLUSH_END_OF_FRAME) &&
       !ctx->recording->wrapped && !ctx->recording->shared &&
       !(sgx_drv_debug & SGX_DEBUG_FLUSH_ALL)) {
      ctx->stats.fl_deferred++;
      return;
   }
   if (ctx->recording) {
      if (flags & PIPE_FLUSH_END_OF_FRAME)
         ctx->stats.fl_eof++;
      else
         ctx->stats.fl_other++;
   }
   sgx_flush_frame(ctx);
   if (fence) {
      struct sgx_fence *f = CALLOC_STRUCT(sgx_fence);

      pipe_reference_init(&f->reference, 1);
      f->seq = ctx->hw ? sgxws_next_seq(ctx->hw) - 1 : 0;
      *fence = NULL;
      sgx_fence_reference(pctx->screen, fence, (struct pipe_fence_handle *)f);
      sgx_fence_reference(pctx->screen, (struct pipe_fence_handle **)&f, NULL);
   }
}

static void
sgx_flush_resource(struct pipe_context *pctx, struct pipe_resource *prsc)
{
   sgx_flush_resource_writes(sgx_context(pctx), sgx_resource(prsc));
}

static void
sgx_context_destroy(struct pipe_context *pctx)
{
   struct sgx_context *ctx = sgx_context(pctx);
   struct sgx_screen *screen = sgx_screen(pctx->screen);

   sgx_finish(ctx);
   sgx_draw_fini(ctx);
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
         sgxws_rt_destroy(ctx->hw,
                          ctx->rts[i]); /* flushes this context's frame */
      if (sgxws_bo_valid(&ctx->ms_zls)) {
         sgxws_wait_idle(ctx->hw, 2000);
         sgxws_bo_free(screen->dev, &ctx->ms_zls);
      }
   }
   simple_mtx_unlock(&screen->lock);

   slab_destroy_child(&ctx->transfer_pool);
   free(ctx->staging);
   free(ctx->idx_staging);
   free(ctx->mem_consts[0]);
   free(ctx->mem_consts[1]);
   FREE(ctx);
}

/* A GPU reset the winsys saw (GL_ARB_robustness).  It went on with a new
 * kernel context, so a context that didn't ask about resets keeps rendering
 * (the frames in flight were lost); one made to lose itself on a reset gets
 * st's callback, which makes it refuse every call until the application
 * makes another.  Called with the screen lock held. */
void
sgx_check_reset(struct sgx_context *ctx)
{
   enum pipe_reset_status status;

   switch (sgxws_reset_status(ctx->hw)) {
   case SGXWS_RESET_GUILTY:
      status = PIPE_GUILTY_CONTEXT_RESET;
      break;
   case SGXWS_RESET_INNOCENT:
      status = PIPE_INNOCENT_CONTEXT_RESET;
      break;
   case SGXWS_RESET_LOST:
      status = PIPE_UNKNOWN_CONTEXT_RESET;
      break;
   default:
      return;
   }
   if (ctx->reset == PIPE_NO_RESET || status == PIPE_GUILTY_CONTEXT_RESET)
      ctx->reset = status;
   if (ctx->reset_cb.reset)
      ctx->reset_cb.reset(ctx->reset_cb.data, status);
}

static enum pipe_reset_status
sgx_get_device_reset_status(struct pipe_context *pctx)
{
   struct sgx_context *ctx = sgx_context(pctx);
   struct sgx_screen *screen = sgx_screen(pctx->screen);
   enum pipe_reset_status status;

   simple_mtx_lock(&screen->lock);
   sgx_check_reset(ctx);
   status = ctx->reset;
   ctx->reset = PIPE_NO_RESET;
   simple_mtx_unlock(&screen->lock);
   return status;
}

static void
sgx_set_device_reset_callback(struct pipe_context *pctx,
                              const struct pipe_device_reset_callback *cb)
{
   struct sgx_context *ctx = sgx_context(pctx);

   if (cb)
      ctx->reset_cb = *cb;
   else
      memset(&ctx->reset_cb, 0, sizeof(ctx->reset_cb));
}

struct pipe_context *
sgx_context_create(struct pipe_screen *pscreen, void *priv, unsigned flags)
{
   struct sgx_screen *screen = sgx_screen(pscreen);
   struct sgx_context *ctx = CALLOC_STRUCT(sgx_context);

   if (!ctx)
      return NULL;

   simple_mtx_lock(&screen->lock);
   if (!screen->hw) {
      /* the screen's one hardware context takes its first context's priority;
       * SGX_CTX_PRIORITY=high|normal|low overrides it (an X server that
       * doesn't ask) */
      const char *env = os_get_option("SGX_CTX_PRIORITY");
      int prio = flags & PIPE_CONTEXT_HIGH_PRIORITY  ? 1
                 : flags & PIPE_CONTEXT_LOW_PRIORITY ? -1
                                                     : 0;

      if (env)
         prio = !strcmp(env, "high") ? 1 : !strcmp(env, "low") ? -1 : 0;
      screen->hw = sgxws_ctx_create(screen->dev, prio);
   }
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
   ctx->base.destroy = sgx_context_destroy;
   ctx->base.flush = sgx_flush;
   ctx->base.flush_resource = sgx_flush_resource;
   ctx->base.get_device_reset_status = sgx_get_device_reset_status;
   ctx->base.set_device_reset_callback = sgx_set_device_reset_callback;
   ctx->base.set_debug_callback = u_default_set_debug_callback;

   slab_create_child(&ctx->transfer_pool, &screen->transfer_pool);
   ctx->stats.on = (sgx_drv_debug & SGX_DEBUG_STATS) != 0;
   ctx->stats.t0 = os_time_get_nano();
   sgx_resource_context_init(ctx);
   sgx_state_init(ctx);
   sgx_draw_init(ctx);
   sgx_program_init(ctx);

   ctx->base.stream_uploader = u_upload_create_default(&ctx->base);
   if (!ctx->base.stream_uploader) {
      sgx_context_destroy(&ctx->base);
      return NULL;
   }
   ctx->base.const_uploader = ctx->base.stream_uploader;
   /* after every hook it creates state through */
   ctx->blitter = util_blitter_create(&ctx->base);
   return &ctx->base;
}

void
sgx_fence_screen_init(struct sgx_screen *screen)
{
   screen->base.fence_reference = sgx_fence_reference;
   screen->base.fence_finish = sgx_fence_finish;
}
