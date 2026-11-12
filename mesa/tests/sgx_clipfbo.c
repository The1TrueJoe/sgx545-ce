/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * piglit's fixed-clip-enables in an FBO, with every pixel checked: six
 * fixed-function clip planes, four combinations of enables, each a clear
 * and a full-screen quad drawn white into a 250x250 RGBA8 FBO, read back
 * whole after each and compared with the half-planes it should keep.  The
 * piglit test fails now and then once ARB_framebuffer_object puts it in an
 * FBO (docs/GL3.md); this prints where: the wrong pixels' count and bounding
 * box per combination, over 20 rounds.
 *
 *   sgx_clipfbo
 *
 * zig cc -target x86_64-linux-gnu.2.36 -I<mesa include> -o sgx_clipfbo
 * sgx_clipfbo.c -ldl
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
G(glViewport);
G(glEnable);
G(glDisable);
G(glClipPlane);
G(glMatrixMode);
G(glLoadIdentity);
G(glReadPixels);
G(glGenTextures);
G(glBindTexture);
G(glTexImage2D);
G(glGenFramebuffers);
G(glBindFramebuffer);
G(glFramebufferTexture2D);
G(glCheckFramebufferStatus);
G(glCreateShader);
G(glShaderSource);
G(glCompileShader);
G(glCreateProgram);
G(glAttachShader);
G(glLinkProgram);
G(glUseProgram);
G(glBegin);
G(glEnd);
G(glVertex2f);
G(glGetString);

#define S 250

/* the planes, as the piglit test sets them: a x + b y + d >= 0 keeps */
static const double planes[6][4] = {
   {0, 1, 0, 0.5}, {-1, 1, 0, 0.8}, {-1, -1, 0, 0.8},
   {0, -1, 0, 0.5}, {1, -1, 0, 0.8}, {1, 1, 0, 0.8},
};
static const unsigned combos[4] = {0x15, 0x33, 0x0f, 0x30};

int
main(void)
{
   void *egl = dlopen("libEGL.so.1", RTLD_NOW);
   if (!egl)
      return 2;
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
   if (!initialize(d, NULL, NULL))
      return 2;
   bindapi(EGL_OPENGL_API);
   EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8,
                  EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
   EGLConfig cfg;
   EGLint n = 0;
   if (!chooseconfig(d, ca, &cfg, 1, &n) || !n)
      return 2;
   EGLint sa[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
   EGLSurface s = createpbuffer(d, cfg, sa);
   EGLContext c = createcontext(d, cfg, EGL_NO_CONTEXT, NULL);
   if (!s || !c || !makecurrent(d, s, s, c))
      return 2;
   GET(glClearColor);
   GET(glClear);
   GET(glViewport);
   GET(glEnable);
   GET(glDisable);
   GET(glClipPlane);
   GET(glMatrixMode);
   GET(glLoadIdentity);
   GET(glReadPixels);
   GET(glGenTextures);
   GET(glBindTexture);
   GET(glTexImage2D);
   GET(glGenFramebuffers);
   GET(glBindFramebuffer);
   GET(glFramebufferTexture2D);
   GET(glCheckFramebufferStatus);
   GET(glCreateShader);
   GET(glShaderSource);
   GET(glCompileShader);
   GET(glCreateProgram);
   GET(glAttachShader);
   GET(glLinkProgram);
   GET(glUseProgram);
   GET(glBegin);
   GET(glEnd);
   GET(glVertex2f);
   GET(glGetString);
   printf("renderer: %s\n", glGetString_(GL_RENDERER));

   GLuint tex, fb;
   glGenTextures_(1, &tex);
   glBindTexture_(GL_TEXTURE_2D, tex);
   glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, S, S, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
   glGenFramebuffers_(1, &fb);
   glBindFramebuffer_(GL_FRAMEBUFFER, fb);
   glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex, 0);
   if (glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      return 2;
   const char *src = "#version 120\nvoid main() { gl_FragColor = vec4(1.0); }\n";
   GLuint sh = glCreateShader_(GL_FRAGMENT_SHADER), p = glCreateProgram_();
   glShaderSource_(sh, 1, &src, NULL);
   glCompileShader_(sh);
   glAttachShader_(p, sh);
   glLinkProgram_(p);
   glUseProgram_(p);
   glViewport_(0, 0, S, S);
   glMatrixMode_(GL_PROJECTION);
   glLoadIdentity_();
   glMatrixMode_(GL_MODELVIEW);
   glLoadIdentity_();
   for (unsigned i = 0; i < 6; i++)
      glClipPlane_(GL_CLIP_PLANE0 + i, planes[i]);

   static unsigned char px[S * S * 4];
   int bad_rounds = 0;
   for (int round = 0; round < 20; round++) {
      int round_bad = 0;
      for (unsigned k = 0; k < 4; k++) {
         for (unsigned i = 0; i < 6; i++)
            (combos[k] >> i & 1 ? glEnable_ : glDisable_)(GL_CLIP_PLANE0 + i);
         glClearColor_(0, 0, 0, 0);
         glClear_(GL_COLOR_BUFFER_BIT);
         glBegin_(GL_QUADS);
         glVertex2f_(-1, -1);
         glVertex2f_(1, -1);
         glVertex2f_(1, 1);
         glVertex2f_(-1, 1);
         glEnd_();
         glReadPixels_(0, 0, S, S, GL_RGBA, GL_UNSIGNED_BYTE, px);
         unsigned wrong = 0, x0 = S, y0 = S, x1 = 0, y1 = 0, white_wrong = 0;
         for (unsigned y = 0; y < S; y++)
            for (unsigned x = 0; x < S; x++) {
               double fx = (x + 0.5) / S * 2 - 1, fy = (y + 0.5) / S * 2 - 1;
               int keep = 1, margin = 0;
               for (unsigned i = 0; i < 6; i++) {
                  if (!(combos[k] >> i & 1))
                     continue;
                  double e = planes[i][0] * fx + planes[i][1] * fy + planes[i][3];
                  if (e < 0)
                     keep = 0;
                  if (e > -0.02 && e < 0.02)
                     margin = 1; /* an edge pixel: either is fine */
               }
               int white = px[4 * (y * S + x)] > 128;
               if (white != keep && !margin) {
                  wrong++;
                  white_wrong += white;
                  if (x < x0) x0 = x;
                  if (y < y0) y0 = y;
                  if (x > x1) x1 = x;
                  if (y > y1) y1 = y;
               }
            }
         if (wrong) {
            printf("round %d combo %u (enables %#x): %u wrong (%u white where "
                   "clipped), box x %u-%u y %u-%u\n",
                   round, k, combos[k], wrong, white_wrong, x0, x1, y0, y1);
            round_bad = 1;
         }
      }
      bad_rounds += round_bad;
   }
   printf("%s: %d of 20 rounds had wrong pixels\n",
          bad_rounds ? "FAIL" : "PASS", bad_rounds);
   return bad_rounds ? 1 : 0;
}
