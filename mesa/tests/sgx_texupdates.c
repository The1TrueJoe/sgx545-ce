/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 *
 * One texture updated many times inside one frame, a draw after each update,
 * as Yamagi Quake II's GL1 renderer updates its dynamic lightmap per lit
 * surface: every draw must sample its own version, and the updates should
 * not wait for the GPU. On a pbuffer, the last frame read back.
 *
 *   sgx_texupdates [updates [frames [overdraw]]]   (default 32, 100, 0)
 *
 * Draw i fills the 8-pixel column i of a 256x128 target from a 128x128 RGB
 * texture whose top-left 16x16 box was just replaced with colour i; overdraw
 * first blends that many full-target quads of another texture, so a wait
 * for the GPU costs what it does under a game's frame. Prints
 * the time per frame and PASS/FAIL per column; exit 0 when all pass. Run with
 * SGX_DEBUG=stats for the driver's texture-write flushes.
 *
 * GL and EGL come through libEGL.so.1 (glvnd) and eglGetProcAddress, so it
 * builds without GL libraries: zig cc -target x86_64-linux-gnu.2.36 -I<mesa
 * include> -o sgx_texupdates sgx_texupdates.c -ldl
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define EGL_EGL_PROTOTYPES 0
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <GL/gl.h>
#include <GL/glext.h>

#define W 256
#define H 128
#define T 128 /* texture size */
#define B 16  /* the updated box */

static PFNEGLGETPROCADDRESSPROC gpa;

static void *L(const char *n)
{
   void *p = (void *)gpa(n);
   if (!p) {
      fprintf(stderr, "no %s\n", n);
      exit(2);
   }
   return p;
}
#define G(t, n) __typeof__(&n) n = L(#n)

static double now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

static void colour(int i, unsigned char *c)
{
   c[0] = (unsigned char)(i * 8);
   c[1] = (unsigned char)(255 - i * 8);
   c[2] = (unsigned char)(i & 1 ? 200 : 40);
}

int main(int argc, char **argv)
{
   int n = argc > 1 ? atoi(argv[1]) : 32, frames = argc > 2 ? atoi(argv[2]) : 100;
   int over = argc > 3 ? atoi(argv[3]) : 0;
   if (n < 1 || n > W / 8)
      n = 32;
   if (frames < 1)
      frames = 1;

   void *egl = dlopen("libEGL.so.1", RTLD_NOW);
   if (!egl) {
      fprintf(stderr, "%s\n", dlerror());
      return 2;
   }
   gpa = (PFNEGLGETPROCADDRESSPROC)dlsym(egl, "eglGetProcAddress");
   PFNEGLGETDISPLAYPROC eglGetDisplay_ = (void *)dlsym(egl, "eglGetDisplay");
   PFNEGLINITIALIZEPROC eglInitialize_ = (void *)dlsym(egl, "eglInitialize");
   PFNEGLBINDAPIPROC eglBindAPI_ = (void *)dlsym(egl, "eglBindAPI");
   PFNEGLCHOOSECONFIGPROC eglChooseConfig_ = (void *)dlsym(egl, "eglChooseConfig");
   PFNEGLCREATECONTEXTPROC eglCreateContext_ = (void *)dlsym(egl, "eglCreateContext");
   PFNEGLCREATEPBUFFERSURFACEPROC eglCreatePbufferSurface_ =
      (void *)dlsym(egl, "eglCreatePbufferSurface");
   PFNEGLMAKECURRENTPROC eglMakeCurrent_ = (void *)dlsym(egl, "eglMakeCurrent");

   EGLDisplay d = eglGetDisplay_(EGL_DEFAULT_DISPLAY);
   if (!eglInitialize_(d, NULL, NULL)) {
      fprintf(stderr, "eglInitialize\n");
      return 2;
   }
   eglBindAPI_(EGL_OPENGL_API);
   EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8,
                  EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                  EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
   EGLConfig cfg;
   EGLint nc = 0;
   if (!eglChooseConfig_(d, ca, &cfg, 1, &nc) || !nc) {
      fprintf(stderr, "no config\n");
      return 2;
   }
   EGLint sa[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
   EGLSurface s = eglCreatePbufferSurface_(d, cfg, sa);
   EGLContext c = eglCreateContext_(d, cfg, EGL_NO_CONTEXT, NULL);
   if (!s || !c || !eglMakeCurrent_(d, s, s, c)) {
      fprintf(stderr, "context\n");
      return 2;
   }

   G(PFNGLCLEARCOLORPROC, glClearColor);
   G(PFNGLCLEARPROC, glClear);
   G(PFNGLVIEWPORTPROC, glViewport);
   G(PFNGLGENTEXTURESPROC, glGenTextures);
   G(PFNGLBINDTEXTUREPROC, glBindTexture);
   G(PFNGLTEXIMAGE2DPROC, glTexImage2D);
   G(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D);
   G(PFNGLTEXPARAMETERIPROC, glTexParameteri);
   G(PFNGLTEXENVIPROC, glTexEnvi);
   G(PFNGLPIXELSTOREIPROC, glPixelStorei);
   G(PFNGLENABLEPROC, glEnable);
   G(PFNGLENABLECLIENTSTATEPROC, glEnableClientState);
   G(PFNGLVERTEXPOINTERPROC, glVertexPointer);
   G(PFNGLTEXCOORDPOINTERPROC, glTexCoordPointer);
   G(PFNGLDRAWARRAYSPROC, glDrawArrays);
   G(PFNGLREADPIXELSPROC, glReadPixels);
   G(PFNGLFLUSHPROC, glFlush);
   G(PFNGLFINISHPROC, glFinish);
   G(PFNGLGETSTRINGPROC, glGetString);
   G(PFNGLMATRIXMODEPROC, glMatrixMode);
   G(PFNGLLOADIDENTITYPROC, glLoadIdentity);
   G(PFNGLORTHOPROC, glOrtho);
   G(PFNGLDISABLEPROC, glDisable);
   G(PFNGLBLENDFUNCPROC, glBlendFunc);

   printf("renderer: %s; %d updates a frame, %d frames, overdraw %d\n",
          glGetString(GL_RENDERER), n, frames, over);
   glViewport(0, 0, W, H);
   glMatrixMode(GL_PROJECTION);
   glLoadIdentity();
   glOrtho(0, W, 0, H, -1, 1);
   glMatrixMode(GL_MODELVIEW);
   glLoadIdentity();

   /* a lightmap page: 128x128 RGB, grey, NEAREST */
   static unsigned char page[T * T * 3], box[B * B * 3];
   memset(page, 128, sizeof(page));
   GLuint tex, bg;
   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
   glGenTextures(1, &bg);
   glBindTexture(GL_TEXTURE_2D, bg);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, T, T, 0, GL_RGB, GL_UNSIGNED_BYTE, page);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, T, T, 0, GL_RGB, GL_UNSIGNED_BYTE, page);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
   glEnable(GL_TEXTURE_2D);
   glEnableClientState(GL_VERTEX_ARRAY);
   glEnableClientState(GL_TEXTURE_COORD_ARRAY);

   /* column i's quad, every texcoord inside the updated box */
   float xy[8], st[8];
   const float u = 0.5f * B / T;
   for (int k = 0; k < 4; k++)
      st[2 * k] = st[2 * k + 1] = u;
   glVertexPointer(2, GL_FLOAT, 0, xy);
   glTexCoordPointer(2, GL_FLOAT, 0, st);

   double t0 = now_ms(), worst = 0;
   for (int f = 0; f < frames; f++) {
      double tf = now_ms();
      glClearColor(0, 0, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      if (over) {
         float fq[8] = {0, 0, W, 0, W, H, 0, H}, fs[8] = {0, 0, 1, 0, 1, 1, 0, 1};
         memcpy(xy, fq, sizeof(fq));
         memcpy(st, fs, sizeof(fs));
         glBindTexture(GL_TEXTURE_2D, bg);
         glEnable(GL_BLEND);
         glBlendFunc(GL_ONE, GL_ONE);
         for (int k = 0; k < over; k++)
            glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
         glDisable(GL_BLEND);
         glBindTexture(GL_TEXTURE_2D, tex);
         for (int k = 0; k < 4; k++)
            st[2 * k] = st[2 * k + 1] = u;
      }
      for (int i = 0; i < n; i++) {
         unsigned char ci[3];
         colour(i, ci);
         for (int p = 0; p < B * B; p++)
            memcpy(box + p * 3, ci, 3);
         glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, B, B, GL_RGB, GL_UNSIGNED_BYTE, box);
         float x0 = i * 8.0f, x1 = x0 + 8;
         float q[8] = {x0, 0, x1, 0, x1, H, x0, H};
         memcpy(xy, q, sizeof(q));
         glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
      }
      if (f == frames - 1)
         glFinish();
      else
         glFlush();
      double dt = now_ms() - tf;
      if (dt > worst)
         worst = dt;
   }
   double total = now_ms() - t0;
   printf("frames: %.2f ms average, %.2f ms worst (%.1f FPS)\n", total / frames,
          worst, frames * 1e3 / total);

   static unsigned char px[W * H * 4];
   int fails = 0;
   glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
   for (int i = 0; i < n; i++) {
      unsigned char want[3], *p = px + ((H / 2) * W + i * 8 + 4) * 4;
      colour(i, want);
      int ok = abs(p[0] - want[0]) <= 3 && abs(p[1] - want[1]) <= 3 &&
               abs(p[2] - want[2]) <= 3;
      if (!ok || i == 0 || i == n - 1)
         printf("%s column %d: %d %d %d, want %d %d %d\n", ok ? "PASS" : "FAIL", i,
                p[0], p[1], p[2], want[0], want[1], want[2]);
      fails += !ok;
   }
   printf("%s: %d of %d columns failed\n", fails ? "FAIL" : "PASS", fails, n);
   return fails != 0;
}
