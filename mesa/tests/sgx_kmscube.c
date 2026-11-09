/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * sgx_kmscube: GLES2 frames through GBM on a KMS-only display device,
 * rendered by the SGX545 under powervr_sgx (Mesa's kmsro pairs the two) and
 * flipped onto the display.  Each frame clears to a colour of its own and
 * draws a green triangle; the last one is checked in the memory the display
 * scans out, read through /dev/mem (simpledrm copies a committed frame there).
 *
 *   sgx_kmscube [-d /dev/dri/card0] [-n frames] [-a scanout-phys-addr]
 *
 * -a: the framebuffer the firmware left running (the EA's CEFDK one is
 * 0x7fc00000); 0 skips the check.  Exit 0 when the last frame is right.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static const char *vs_src =
   "attribute vec2 pos;\n"
   "void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *fs_src =
   "precision mediump float;\n"
   "uniform vec4 color;\n"
   "void main() { gl_FragColor = color; }\n";

static GLuint
shader(GLenum type, const char *src)
{
   GLuint s = glCreateShader(type);
   GLint ok = 0;

   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[512];

      glGetShaderInfoLog(s, sizeof(log), NULL, log);
      fprintf(stderr, "shader: %s\n", log);
      exit(1);
   }
   return s;
}

struct fb {
   struct gbm_bo *bo;
   uint32_t id;
};

static void
fb_destroy(struct gbm_bo *bo, void *data)
{
   struct fb *fb = data;

   drmModeRmFB(gbm_device_get_fd(gbm_bo_get_device(bo)), fb->id);
   free(fb);
}

static struct fb *
fb_get(int fd, struct gbm_bo *bo)
{
   struct fb *fb = gbm_bo_get_user_data(bo);
   uint32_t handles[4] = { gbm_bo_get_handle(bo).u32 }, pitches[4] = { gbm_bo_get_stride(bo) };
   uint32_t offsets[4] = { 0 };

   if (fb)
      return fb;
   fb = calloc(1, sizeof(*fb));
   fb->bo = bo;
   if (drmModeAddFB2(fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo), GBM_FORMAT_XRGB8888, handles,
                     pitches, offsets, &fb->id, 0)) {
      fprintf(stderr, "AddFB2: %s\n", strerror(errno));
      exit(1);
   }
   gbm_bo_set_user_data(bo, fb, fb_destroy);
   return fb;
}

static void
flip_done(int fd, unsigned frame, unsigned sec, unsigned usec, void *data)
{
   *(int *)data = 0;
}

/* the frame's clear colour, as XRGB8888 */
static uint32_t
frame_rgb(unsigned f)
{
   return ((f * 37) & 0xff) << 16 | ((f * 11) & 0x7f) << 8 | 0x80;
}

int
main(int argc, char **argv)
{
   const char *node = "/dev/dri/card0";
   unsigned frames = 120, w, h;
   unsigned long scanout_pa = 0x7fc00000;
   drmModeRes *res;
   drmModeConnector *conn = NULL;
   drmModeEncoder *enc;
   drmModeModeInfo mode;
   uint32_t crtc, conn_id;
   struct gbm_device *gbm;
   struct gbm_surface *gs;
   struct gbm_bo *bo, *prev = NULL;
   EGLDisplay dpy;
   EGLConfig cfgs[64], cfg = NULL;
   EGLint n = 0;
   EGLSurface surf;
   EGLContext ctx;
   GLuint prog;
   GLint color;
   struct timespec t0, t1;
   int fd, opt, bad = 0;

   while ((opt = getopt(argc, argv, "d:n:a:")) != -1) {
      switch (opt) {
      case 'd': node = optarg; break;
      case 'n': frames = (unsigned)atoi(optarg); break;
      case 'a': scanout_pa = strtoul(optarg, NULL, 0); break;
      default: return 2;
      }
   }
   setvbuf(stdout, NULL, _IONBF, 0);

   fd = open(node, O_RDWR | O_CLOEXEC);
   if (fd < 0 || !(res = drmModeGetResources(fd))) {
      fprintf(stderr, "%s: no KMS device\n", node);
      return 1;
   }
   for (int i = 0; i < res->count_connectors && !conn; i++) {
      conn = drmModeGetConnector(fd, res->connectors[i]);
      if (conn && (conn->connection != DRM_MODE_CONNECTED || !conn->count_modes)) {
         drmModeFreeConnector(conn);
         conn = NULL;
      }
   }
   if (!conn) {
      fprintf(stderr, "no connected connector\n");
      return 1;
   }
   mode = conn->modes[0];
   conn_id = conn->connector_id;
   enc = drmModeGetEncoder(fd, conn->encoder_id ? conn->encoder_id : conn->encoders[0]);
   crtc = enc && enc->crtc_id ? enc->crtc_id : res->crtcs[0];
   w = mode.hdisplay;
   h = mode.vdisplay;
   printf("display %s: %ux%u, connector %u, crtc %u\n", node, w, h, conn_id, crtc);

   gbm = gbm_create_device(fd);
   gs = gbm ? gbm_surface_create(gbm, w, h, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING)
            : NULL;
   if (!gs) {
      fprintf(stderr, "GBM: no scanout surface\n");
      return 1;
   }
   printf("GBM backend: %s\n", gbm_device_get_backend_name(gbm));

   dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
   if (!eglInitialize(dpy, NULL, NULL)) {
      fprintf(stderr, "eglInitialize: %#x\n", eglGetError());
      return 1;
   }
   {
      static const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                     EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };

      eglChooseConfig(dpy, attr, cfgs, 64, &n);
      for (int i = 0; i < n; i++) {
         EGLint id;

         eglGetConfigAttrib(dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &id);
         if (id == GBM_FORMAT_XRGB8888) {
            cfg = cfgs[i];
            break;
         }
      }
   }
   if (!cfg) {
      fprintf(stderr, "no XRGB8888 window config\n");
      return 1;
   }
   {
      static const EGLint cattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

      ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cattr);
      surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)gs, NULL);
   }
   if (!ctx || !surf || !eglMakeCurrent(dpy, surf, surf, ctx)) {
      fprintf(stderr, "EGL context/surface: %#x\n", eglGetError());
      return 1;
   }
   printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));

   prog = glCreateProgram();
   glAttachShader(prog, shader(GL_VERTEX_SHADER, vs_src));
   glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs_src));
   glBindAttribLocation(prog, 0, "pos");
   glLinkProgram(prog);
   glUseProgram(prog);
   color = glGetUniformLocation(prog, "color");
   {
      static const GLfloat tri[] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f };

      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
      glEnableVertexAttribArray(0);
   }
   glViewport(0, 0, w, h);

   clock_gettime(CLOCK_MONOTONIC, &t0);
   for (unsigned f = 0; f < frames; f++) {
      uint32_t rgb = frame_rgb(f);
      struct fb *fb;
      int pending = 1;

      glClearColor(((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      glUniform4f(color, 0, 1, 0, 1);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      eglSwapBuffers(dpy, surf);
      bo = gbm_surface_lock_front_buffer(gs);
      fb = fb_get(fd, bo);
      if (!f) {
         if (drmModeSetCrtc(fd, crtc, fb->id, 0, 0, &conn_id, 1, &mode)) {
            fprintf(stderr, "SetCrtc: %s\n", strerror(errno));
            return 1;
         }
      } else if (!drmModePageFlip(fd, crtc, fb->id, DRM_MODE_PAGE_FLIP_EVENT, &pending)) {
         drmEventContext ev = { .version = 2, .page_flip_handler = flip_done };

         while (pending)
            drmHandleEvent(fd, &ev);
      } else if (drmModeSetCrtc(fd, crtc, fb->id, 0, 0, &conn_id, 1, &mode)) {
         fprintf(stderr, "flip and SetCrtc: %s\n", strerror(errno));
         return 1;
      }
      if (prev)
         gbm_surface_release_buffer(gs, prev);
      prev = bo;
   }
   clock_gettime(CLOCK_MONOTONIC, &t1);
   printf("%u frames, %.1f FPS\n", frames,
          frames / ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9));

   /* the last frame, where the display reads it */
   if (scanout_pa) {
      int mem = open("/dev/mem", O_RDONLY | O_SYNC);
      unsigned stride = (w + 31) & ~31u;
      size_t len = (size_t)stride * 4 * h;
      const uint32_t *p = mem < 0 ? MAP_FAILED : mmap(NULL, len, PROT_READ, MAP_SHARED, mem, scanout_pa);
      uint32_t bg = frame_rgb(frames - 1), tri = 0x00ff00;

      usleep(100000);
      if (p == MAP_FAILED) {
         printf("scanout: /dev/mem: %s, not checked\n", strerror(errno));
      } else {
         for (unsigned y = 0; y < h; y++) {
            for (unsigned x = 0; x < w; x++) {
               uint32_t v = p[(size_t)y * stride + x] & 0xffffff;

               if (v != bg && v != tri)
                  bad++;
            }
         }
         printf("scanout: centre %06x (want %06x), corner %06x (want %06x), %d pixels neither\n",
                p[(size_t)(h / 2) * stride + w / 2] & 0xffffff, tri, p[2 * stride + 2] & 0xffffff, bg, bad);
         if ((p[(size_t)(h / 2) * stride + w / 2] & 0xffffff) != tri || (p[2 * stride + 2] & 0xffffff) != bg)
            bad++;
      }
   }
   return bad ? 1 : 0;
}
