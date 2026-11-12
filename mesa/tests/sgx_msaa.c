/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 *
 * 4x multisampled framebuffers (docs/GL3.md, STATE.md "Multisampling (4x)"):
 * a 4-sample colour renderbuffer, a triangle with slanted edges, resolved
 * with glBlitFramebuffer into a one-sample buffer and read back.  The SGX545
 * resolves 2x2 samples a pixel with a box filter, so an edge pixel covered by
 * 1, 2 or 3 of its samples reads 1/4, 2/4 or 3/4 of the triangle's colour
 * (the DDK's ms4 capture: green 3f, 7f, bf over black).  Cases:
 *
 *   colour WxH        green triangle over black: every pixel 0, ff or one of
 *                     the quarter steps, and the three steps all present
 *   depth WxH         the same with a 4-sample depth buffer, then a red quad
 *                     behind the triangle (GL_LESS): per-sample depth keeps
 *                     red out of the samples the triangle covers, so every
 *                     pixel's red + green is ff (within the rounding) and
 *                     both pure colours are present
 *   scaled WxH        the colour case resolved into a buffer half the size
 *                     with EXT_framebuffer_multisample_blit_scaled's
 *                     SCALED_RESOLVE_FASTEST filter (a scaled resolve with
 *                     GL_LINEAR is GL_INVALID_OPERATION by the spec): runs
 *
 * at 720x480 (the capture's size) and 256x256 (the small-target tiling mode,
 * which no capture covers).
 *
 *   sgx_msaa          prints PASS/FAIL per case, exit 0 when all pass
 *
 * GL and EGL through libEGL.so.1 (glvnd) and eglGetProcAddress:
 * zig cc -target x86_64-linux-gnu.2.36 -I<mesa include> -o sgx_msaa
 * sgx_msaa.c -ldl
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EGL_EGL_PROTOTYPES 0
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <GL/gl.h>
#include <GL/glext.h>

static PFNEGLGETPROCADDRESSPROC gpa;

typedef void (*fn)(void);
static void *
L(const char *n)
{
   void *p = (void *)gpa(n);
   if (!p) {
      fprintf(stderr, "no %s\n", n);
      exit(2);
   }
   return p;
}
#define G(n) static __typeof__(&n) n##_
#define GET(n) n##_ = L(#n)

G(glClearColor);
G(glClear);
G(glClearDepth);
G(glViewport);
G(glEnable);
G(glDisable);
G(glDepthFunc);
G(glEnableClientState);
G(glVertexPointer);
G(glColor4f);
G(glDrawArrays);
G(glReadPixels);
G(glGetIntegerv);
G(glGetString);
G(glGetError);
G(glMatrixMode);
G(glLoadIdentity);
G(glOrtho);
G(glGenFramebuffers);
G(glBindFramebuffer);
G(glGenRenderbuffers);
G(glBindRenderbuffer);
G(glRenderbufferStorage);
G(glRenderbufferStorageMultisample);
G(glFramebufferRenderbuffer);
G(glCheckFramebufferStatus);
G(glBlitFramebuffer);
G(glDeleteFramebuffers);
G(glDeleteRenderbuffers);

static int failures;

static void
check(const char *name, int ok, const char *detail)
{
   printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name, detail ? ": " : "",
          detail ? detail : "");
   failures += !ok;
}

/* a framebuffer: colour (and depth) renderbuffers of samples samples */
static GLuint
fbo(int w, int h, int samples, int depth, GLuint rb[2])
{
   GLuint f;

   glGenFramebuffers_(1, &f);
   glBindFramebuffer_(GL_FRAMEBUFFER, f);
   glGenRenderbuffers_(2, rb);
   glBindRenderbuffer_(GL_RENDERBUFFER, rb[0]);
   if (samples)
      glRenderbufferStorageMultisample_(GL_RENDERBUFFER, samples, GL_RGBA8, w,
                                        h);
   else
      glRenderbufferStorage_(GL_RENDERBUFFER, GL_RGBA8, w, h);
   glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, rb[0]);
   if (depth) {
      glBindRenderbuffer_(GL_RENDERBUFFER, rb[1]);
      glRenderbufferStorageMultisample_(GL_RENDERBUFFER, samples,
                                        GL_DEPTH_COMPONENT24, w, h);
      glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                 GL_RENDERBUFFER, rb[1]);
   }
   return f;
}

static void
draw(int w, int h, int depth)
{
   /* slanted edges at every angle class: a shallow, a steep and a mixed one */
   const GLfloat tri[] = {0.1f * w, 0.1f * h, 0.0f,  0.9f * w, 0.23f * h,
                          0.0f,     0.47f * w, 0.9f * h, 0.0f};
   const GLfloat quad[] = {0, 0, -0.5f, (GLfloat)w, 0, -0.5f,
                           0, (GLfloat)h, -0.5f, 0, (GLfloat)h, -0.5f,
                           (GLfloat)w, 0, -0.5f, (GLfloat)w, (GLfloat)h, -0.5f};

   glViewport_(0, 0, w, h);
   glMatrixMode_(GL_PROJECTION);
   glLoadIdentity_();
   glOrtho_(0, w, 0, h, -1, 1);
   glMatrixMode_(GL_MODELVIEW);
   glLoadIdentity_();
   glClearColor_(0, 0, 0, 1);
   glClearDepth_(1.0);
   glClear_(GL_COLOR_BUFFER_BIT | (depth ? GL_DEPTH_BUFFER_BIT : 0));
   glEnableClientState_(GL_VERTEX_ARRAY);
   if (depth) {
      glEnable_(GL_DEPTH_TEST);
      glDepthFunc_(GL_LESS);
   }
   glColor4f_(0, 1, 0, 1);
   glVertexPointer_(3, GL_FLOAT, 0, tri);
   glDrawArrays_(GL_TRIANGLES, 0, 3);
   if (depth) {
      /* behind the triangle: glOrtho's z -0.5 is farther than 0 */
      glColor4f_(1, 0, 0, 1);
      glVertexPointer_(3, GL_FLOAT, 0, quad);
      glDrawArrays_(GL_TRIANGLES, 0, 6);
      glDisable_(GL_DEPTH_TEST);
   }
}

static int
near(int v, int want)
{
   return v >= want - 1 && v <= want + 1;
}

static void
run(int w, int h, int depth, int scaled)
{
   char name[64], detail[160];
   GLuint rbm[2], rbs[2];
   int dw = scaled ? w / 2 : w, dh = scaled ? h / 2 : h;
   unsigned char *px = malloc((size_t)dw * dh * 4);
   long pure0 = 0, pure1 = 0, q[3] = {0}, bad = 0;

   snprintf(name, sizeof(name), "%s %dx%d",
            scaled ? "scaled" : depth ? "depth" : "colour", w, h);
   GLuint fm = fbo(w, h, 4, depth, rbm);
   GLint samples = 0;

   glGetIntegerv_(GL_SAMPLES, &samples);
   if (glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
       samples != 4) {
      snprintf(detail, sizeof(detail), "4x framebuffer incomplete (GL_SAMPLES %d)",
               samples);
      check(name, 0, detail);
      free(px);
      return;
   }
   draw(w, h, depth);
   GLuint fs = fbo(dw, dh, 0, 0, rbs);
   glBindFramebuffer_(GL_READ_FRAMEBUFFER, fm);
   glBindFramebuffer_(GL_DRAW_FRAMEBUFFER, fs);
   glBlitFramebuffer_(0, 0, w, h, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT,
                      scaled ? GL_SCALED_RESOLVE_FASTEST_EXT : GL_NEAREST);
   glBindFramebuffer_(GL_READ_FRAMEBUFFER, fs);
   glReadPixels_(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, px);
   GLenum err = glGetError_();

   for (long i = 0; i < (long)dw * dh; i++) {
      int r = px[4 * i], g = px[4 * i + 1], b = px[4 * i + 2];

      if (depth) {
         /* red behind green: per sample one or the other */
         if (b || !near(r + g, 255))
            bad++;
         else if (g >= 254)
            pure1++;
         else if (r >= 254)
            pure0++;
         else if (near(g, 64) || near(g, 63))
            q[0]++;
         else if (near(g, 128) || near(g, 127))
            q[1]++;
         else if (near(g, 191) || near(g, 192))
            q[2]++;
         else
            bad++;
      } else {
         if (r || b)
            bad++;
         else if (g == 0)
            pure0++;
         else if (g >= 254)
            pure1++;
         else if (near(g, 64) || near(g, 63))
            q[0]++;
         else if (near(g, 128) || near(g, 127))
            q[1]++;
         else if (near(g, 191) || near(g, 192))
            q[2]++;
         else if (!scaled)
            bad++;
      }
   }
   snprintf(detail, sizeof(detail),
            "outside %ld inside %ld quarters %ld/%ld/%ld other %ld glerror %#x",
            pure0, pure1, q[0], q[1], q[2], bad, err);
   check(name,
         !err && pure0 && pure1 && !bad &&
            (scaled || (q[0] && q[1] && q[2])),
         detail);
   glBindFramebuffer_(GL_FRAMEBUFFER, 0);
   glDeleteFramebuffers_(1, &fm);
   glDeleteFramebuffers_(1, &fs);
   glDeleteRenderbuffers_(2, rbm);
   glDeleteRenderbuffers_(2, rbs);
   free(px);
}

int
main(void)
{
   void *egl = dlopen("libEGL.so.1", RTLD_NOW);
   if (!egl) {
      fprintf(stderr, "%s\n", dlerror());
      return 2;
   }
   gpa = (PFNEGLGETPROCADDRESSPROC)dlsym(egl, "eglGetProcAddress");
   PFNEGLGETDISPLAYPROC getdisplay = (void *)dlsym(egl, "eglGetDisplay");
   PFNEGLINITIALIZEPROC initialize = (void *)dlsym(egl, "eglInitialize");
   PFNEGLBINDAPIPROC bindapi = (void *)dlsym(egl, "eglBindAPI");
   PFNEGLCHOOSECONFIGPROC chooseconfig = (void *)dlsym(egl, "eglChooseConfig");
   PFNEGLCREATECONTEXTPROC createcontext =
      (void *)dlsym(egl, "eglCreateContext");
   PFNEGLCREATEPBUFFERSURFACEPROC createpbuffer =
      (void *)dlsym(egl, "eglCreatePbufferSurface");
   PFNEGLMAKECURRENTPROC makecurrent = (void *)dlsym(egl, "eglMakeCurrent");

   EGLDisplay d = getdisplay(EGL_DEFAULT_DISPLAY);
   if (!initialize(d, NULL, NULL)) {
      fprintf(stderr, "eglInitialize\n");
      return 2;
   }
   bindapi(EGL_OPENGL_API);
   EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8,
                  EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_RENDERABLE_TYPE,
                  EGL_OPENGL_BIT, EGL_NONE};
   EGLConfig cfg;
   EGLint n = 0;
   if (!chooseconfig(d, ca, &cfg, 1, &n) || !n) {
      fprintf(stderr, "no config\n");
      return 2;
   }
   EGLint sa[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
   EGLSurface s = createpbuffer(d, cfg, sa);
   EGLContext c = createcontext(d, cfg, EGL_NO_CONTEXT, NULL);
   if (!s || !c || !makecurrent(d, s, s, c)) {
      fprintf(stderr, "context\n");
      return 2;
   }
   GET(glClearColor);
   GET(glClear);
   GET(glClearDepth);
   GET(glViewport);
   GET(glEnable);
   GET(glDisable);
   GET(glDepthFunc);
   GET(glEnableClientState);
   GET(glVertexPointer);
   GET(glColor4f);
   GET(glDrawArrays);
   GET(glReadPixels);
   GET(glGetIntegerv);
   GET(glGetString);
   GET(glGetError);
   GET(glMatrixMode);
   GET(glLoadIdentity);
   GET(glOrtho);
   GET(glGenFramebuffers);
   GET(glBindFramebuffer);
   GET(glGenRenderbuffers);
   GET(glBindRenderbuffer);
   GET(glRenderbufferStorage);
   GET(glRenderbufferStorageMultisample);
   GET(glFramebufferRenderbuffer);
   GET(glCheckFramebufferStatus);
   GET(glBlitFramebuffer);
   GET(glDeleteFramebuffers);
   GET(glDeleteRenderbuffers);

   GLint max = 0;
   glGetIntegerv_(GL_MAX_SAMPLES, &max);
   printf("renderer: %s, GL_MAX_SAMPLES %d\n", glGetString_(GL_RENDERER), max);
   if (max < 4) {
      check("max samples", 0, "GL_MAX_SAMPLES < 4");
      return 1;
   }
   static const int sizes[][2] = {{720, 480}, {256, 256}};
   for (unsigned i = 0; i < 2; i++) {
      run(sizes[i][0], sizes[i][1], 0, 0);
      run(sizes[i][0], sizes[i][1], 1, 0);
      run(sizes[i][0], sizes[i][1], 0, 1);
   }
   return failures ? 1 : 0;
}
