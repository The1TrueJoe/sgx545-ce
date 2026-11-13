/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * The layout of a ZLS buffer (docs/TEX.md section 7): renders whose every
 * stored depth and stencil names the pixel it belongs to, for
 * SGX_DEBUG=zlsdump to write out and tools/zls_layout.py to read.
 *
 * Each case is an FBO with a depth (or depth/stencil) renderbuffer:
 *
 *   depth    a plane, window depth 0.05 + 0.9 * (Y - 0.5 + X / w) / h at
 *            window position (X, Y), written with GL_ALWAYS: at a pixel's
 *            centre the row is the integer part of the depth's scaled value
 *            and (X / w) the fraction, so every
 *            stored float gives back the position it was computed at (a
 *            pixel's centre at 1x, a sample's position at 4x)
 *   stencil  8x8 blocks, block (bx, by) set to bx << 4 | by (GL_REPLACE)
 *
 * at 96x64 and 120x72 (padded to 128x80: neither a whole number of 32x16
 * tiles), with GL_DEPTH_COMPONENT24 and GL_DEPTH24_STENCIL8, at 1 and 4
 * samples, then GL_DEPTH_COMPONENT16 once, and once with no colour buffer (a
 * depth-only framebuffer).  docs/TEX.md section 7 has the depth order from
 * window captures; this checks it for FBOs, 16-bit depth and the stencil.  One
 * glFinish a case, so one ZLS dump a case, in the order printed.
 *
 *   SGX_DEBUG=zlsdump sgx_zls_layout
 *
 * zig cc -target x86_64-linux-gnu.2.36 -I<mesa include> -o sgx_zls_layout
 * sgx_zls_layout.c -ldl
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#define EGL_EGL_PROTOTYPES 0
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <GL/gl.h>
#include <GL/glext.h>

static PFNEGLGETPROCADDRESSPROC gpa;

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
G(glClearStencil);
G(glViewport);
G(glEnable);
G(glDisable);
G(glDepthFunc);
G(glDepthMask);
G(glStencilFunc);
G(glStencilOp);
G(glEnableClientState);
G(glVertexPointer);
G(glColor4f);
G(glDrawArrays);
G(glFinish);
G(glGetString);
G(glGetError);
G(glMatrixMode);
G(glLoadIdentity);
G(glOrtho);
G(glGenFramebuffers);
G(glBindFramebuffer);
G(glGenRenderbuffers);
G(glBindRenderbuffer);
G(glRenderbufferStorageMultisample);
G(glFramebufferRenderbuffer);
G(glCheckFramebufferStatus);
G(glDeleteFramebuffers);
G(glDeleteRenderbuffers);
G(glDrawBuffer);
G(glReadBuffer);

static int failures;

static void
run(int w, int h, GLenum fmt, int samples, int colour)
{
   GLuint f, rb[2];
   int stencil = fmt == GL_DEPTH24_STENCIL8;
   /* glOrtho(.., -1, 1): window depth d is vertex z 1 - 2d */
   float d00 = 0.05f - 0.45f / h, d10 = 0.05f + 0.45f / h,
         d01 = 0.95f - 0.45f / h, d11 = 0.95f + 0.45f / h;
   const GLfloat plane[] = {
      0, 0, 1 - 2 * d00, (GLfloat)w, 0, 1 - 2 * d10, 0, (GLfloat)h, 1 - 2 * d01,
      0, (GLfloat)h, 1 - 2 * d01, (GLfloat)w, 0, 1 - 2 * d10, (GLfloat)w,
      (GLfloat)h, 1 - 2 * d11};

   glGenFramebuffers_(1, &f);
   glBindFramebuffer_(GL_FRAMEBUFFER, f);
   glGenRenderbuffers_(2, rb);
   if (colour) {
      glBindRenderbuffer_(GL_RENDERBUFFER, rb[0]);
      glRenderbufferStorageMultisample_(GL_RENDERBUFFER, samples, GL_RGBA8, w,
                                        h);
      glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                 GL_RENDERBUFFER, rb[0]);
   } else {
      glDrawBuffer_(GL_NONE);
      glReadBuffer_(GL_NONE);
   }
   glBindRenderbuffer_(GL_RENDERBUFFER, rb[1]);
   glRenderbufferStorageMultisample_(GL_RENDERBUFFER, samples, fmt, w, h);
   glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, rb[1]);
   if (stencil)
      glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                 GL_RENDERBUFFER, rb[1]);
   GLenum st = glCheckFramebufferStatus_(GL_FRAMEBUFFER);

   printf("case %dx%d %s s%d %s: %s\n", w, h,
          stencil ? "Z24_UNORM_S8_UINT"
          : fmt == GL_DEPTH_COMPONENT16 ? "Z16_UNORM" : "Z24X8_UNORM",
          samples ? samples : 1,
          colour ? "colour" : "depth-only",
          st == GL_FRAMEBUFFER_COMPLETE ? "complete" : "INCOMPLETE");
   if (st != GL_FRAMEBUFFER_COMPLETE) {
      failures++;
      goto out;
   }
   glViewport_(0, 0, w, h);
   glMatrixMode_(GL_PROJECTION);
   glLoadIdentity_();
   glOrtho_(0, w, 0, h, -1, 1);
   glMatrixMode_(GL_MODELVIEW);
   glLoadIdentity_();
   glClearColor_(0, 0, 0, 1);
   glClearDepth_(1.0);
   glClearStencil_(0);
   glClear_(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
            (stencil ? GL_STENCIL_BUFFER_BIT : 0));
   glEnableClientState_(GL_VERTEX_ARRAY);
   glEnable_(GL_DEPTH_TEST);
   glDepthFunc_(GL_ALWAYS);
   glColor4f_(0, 1, 0, 1);
   glVertexPointer_(3, GL_FLOAT, 0, plane);
   glDrawArrays_(GL_TRIANGLES, 0, 6);
   glDepthMask_(GL_FALSE);
   if (stencil) {
      glEnable_(GL_STENCIL_TEST);
      glStencilOp_(GL_KEEP, GL_KEEP, GL_REPLACE);
      for (int by = 0; by * 8 < h; by++)
         for (int bx = 0; bx * 8 < w; bx++) {
            GLfloat x0 = bx * 8.0f, y0 = by * 8.0f, x1 = x0 + 8, y1 = y0 + 8;
            const GLfloat q[] = {x0, y0, 0, x1, y0, 0, x0, y1, 0,
                                 x0, y1, 0, x1, y0, 0, x1, y1, 0};

            glStencilFunc_(GL_ALWAYS, bx << 4 | by, 0xff);
            glVertexPointer_(3, GL_FLOAT, 0, q);
            glDrawArrays_(GL_TRIANGLES, 0, 6);
         }
      glDisable_(GL_STENCIL_TEST);
   }
   glDepthMask_(GL_TRUE);
   glDisable_(GL_DEPTH_TEST);
   glFinish_();
   GLenum e = glGetError_();
   if (e) {
      printf("  glerror %#x\n", e);
      failures++;
   }
out:
   glBindFramebuffer_(GL_FRAMEBUFFER, 0);
   glDeleteFramebuffers_(1, &f);
   glDeleteRenderbuffers_(2, rb);
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
   GET(glClearStencil);
   GET(glViewport);
   GET(glEnable);
   GET(glDisable);
   GET(glDepthFunc);
   GET(glDepthMask);
   GET(glStencilFunc);
   GET(glStencilOp);
   GET(glEnableClientState);
   GET(glVertexPointer);
   GET(glColor4f);
   GET(glDrawArrays);
   GET(glFinish);
   GET(glGetString);
   GET(glGetError);
   GET(glMatrixMode);
   GET(glLoadIdentity);
   GET(glOrtho);
   GET(glGenFramebuffers);
   GET(glBindFramebuffer);
   GET(glGenRenderbuffers);
   GET(glBindRenderbuffer);
   GET(glRenderbufferStorageMultisample);
   GET(glFramebufferRenderbuffer);
   GET(glCheckFramebufferStatus);
   GET(glDeleteFramebuffers);
   GET(glDeleteRenderbuffers);
   GET(glDrawBuffer);
   GET(glReadBuffer);

   printf("renderer: %s\n", glGetString_(GL_RENDERER));
   static const int sizes[][2] = {{96, 64}, {120, 72}};
   for (unsigned i = 0; i < 2; i++)
      for (int samples = 0; samples <= 4; samples += 4) {
         run(sizes[i][0], sizes[i][1], GL_DEPTH_COMPONENT24, samples, 1);
         run(sizes[i][0], sizes[i][1], GL_DEPTH24_STENCIL8, samples, 1);
      }
   run(96, 64, GL_DEPTH_COMPONENT16, 0, 1);
   run(96, 64, GL_DEPTH24_STENCIL8, 0, 0);
   return failures ? 1 : 0;
}
