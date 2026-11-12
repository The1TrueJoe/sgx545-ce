/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 *
 * Desktop GL fixed-function multitexturing as GL1 game renderers draw
 * lightmapped surfaces (ioquake3's and Yamagi Quake II's GL1 paths), on a
 * pbuffer, each case read back:
 *
 *   multitex(+lock)       unit 0 the texture, unit 1 the lightmap, GL_MODULATE,
 *                         texcoords from client arrays per unit, plain and
 *                         inside glLockArraysEXT; then a unit-0 quad (the HUD)
 *   distinct(+subimage)   distinct texels per unit, unit 1's coordinates
 *                         mirrored, then one lightmap texel replaced with
 *                         glTexSubImage2D
 *   combine-scale2        unit 1 GL_COMBINE MODULATE with GL_RGB_SCALE 2
 *   luminance-lightmap    a GL_LUMINANCE lightmap
 *   npot-diffuse          a 3x2 texture under the lightmap
 *   pass1/pass2           the multipass form: the lightmap, then the texture
 *                         blended GL_DST_COLOR, GL_ZERO at depth GL_EQUAL
 *   flat-shademodel       glShadeModel(GL_FLAT): the last vertex's colour
 *   flat/smooth-alpha     a colour's alpha through a blend
 *   flat/smooth-tex-*     a texture times the colour (GL_MODULATE), and blended
 *                         by the texel's alpha (QuakeSpasm's particles, drawn
 *                         under GL_FLAT)
 *
 *   sgx_multitex          prints PASS/FAIL per check, exit 0 when all pass
 *
 * GL and EGL come through libEGL.so.1 (glvnd) and eglGetProcAddress, so it
 * builds without GL libraries: zig cc -target x86_64-linux-gnu.2.36 -I<mesa
 * include> -o sgx_multitex sgx_multitex.c -ldl
 */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EGL_EGL_PROTOTYPES 0
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <GL/gl.h>
#include <GL/glext.h>

#define W 64
#define H 64

static PFNEGLGETPROCADDRESSPROC gpa;
#define F(t, n) static t n;
F(PFNEGLGETDISPLAYPROC, eglGetDisplay_)
F(PFNEGLINITIALIZEPROC, eglInitialize_)
F(PFNEGLBINDAPIPROC, eglBindAPI_)
F(PFNEGLCHOOSECONFIGPROC, eglChooseConfig_)
F(PFNEGLCREATECONTEXTPROC, eglCreateContext_)
F(PFNEGLCREATEPBUFFERSURFACEPROC, eglCreatePbufferSurface_)
F(PFNEGLMAKECURRENTPROC, eglMakeCurrent_)

typedef void (*fn)(void);
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

int main(void)
{
   void *egl = dlopen("libEGL.so.1", RTLD_NOW);
   if (!egl) {
      fprintf(stderr, "%s\n", dlerror());
      return 2;
   }
   gpa = (PFNEGLGETPROCADDRESSPROC)dlsym(egl, "eglGetProcAddress");
   eglGetDisplay_ = (void *)dlsym(egl, "eglGetDisplay");
   eglInitialize_ = (void *)dlsym(egl, "eglInitialize");
   eglBindAPI_ = (void *)dlsym(egl, "eglBindAPI");
   eglChooseConfig_ = (void *)dlsym(egl, "eglChooseConfig");
   eglCreateContext_ = (void *)dlsym(egl, "eglCreateContext");
   eglCreatePbufferSurface_ = (void *)dlsym(egl, "eglCreatePbufferSurface");
   eglMakeCurrent_ = (void *)dlsym(egl, "eglMakeCurrent");

   EGLDisplay d = eglGetDisplay_(EGL_DEFAULT_DISPLAY);
   if (!eglInitialize_(d, NULL, NULL)) {
      fprintf(stderr, "eglInitialize\n");
      return 2;
   }
   eglBindAPI_(EGL_OPENGL_API);
   EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8,
                  EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 16,
                  EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
   EGLConfig cfg;
   EGLint n = 0;
   if (!eglChooseConfig_(d, ca, &cfg, 1, &n) || !n) {
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
   G(PFNGLTEXPARAMETERIPROC, glTexParameteri);
   G(PFNGLTEXENVIPROC, glTexEnvi);
   G(PFNGLENABLEPROC, glEnable);
   G(PFNGLDISABLEPROC, glDisable);
   G(PFNGLACTIVETEXTUREPROC, glActiveTexture);
   G(PFNGLCLIENTACTIVETEXTUREPROC, glClientActiveTexture);
   G(PFNGLENABLECLIENTSTATEPROC, glEnableClientState);
   G(PFNGLDISABLECLIENTSTATEPROC, glDisableClientState);
   G(PFNGLVERTEXPOINTERPROC, glVertexPointer);
   G(PFNGLTEXCOORDPOINTERPROC, glTexCoordPointer);
   G(PFNGLCOLORPOINTERPROC, glColorPointer);
   G(PFNGLDRAWELEMENTSPROC, glDrawElements);
   G(PFNGLREADPIXELSPROC, glReadPixels);
   G(PFNGLFINISHPROC, glFinish);
   G(PFNGLBLENDFUNCPROC, glBlendFunc);
   G(PFNGLDEPTHFUNCPROC, glDepthFunc);
   G(PFNGLGETSTRINGPROC, glGetString);
   G(PFNGLMATRIXMODEPROC, glMatrixMode);
   G(PFNGLLOADIDENTITYPROC, glLoadIdentity);
   G(PFNGLORTHOPROC, glOrtho);
   PFNGLLOCKARRAYSEXTPROC glLockArraysEXT = (void *)gpa("glLockArraysEXT");
   PFNGLUNLOCKARRAYSEXTPROC glUnlockArraysEXT = (void *)gpa("glUnlockArraysEXT");

   printf("renderer: %s\n", glGetString(GL_RENDERER));
   glViewport(0, 0, W, H);
   glMatrixMode(GL_PROJECTION);
   glLoadIdentity();
   glOrtho(0, W, 0, H, -1, 1);
   glMatrixMode(GL_MODELVIEW);
   glLoadIdentity();

   /* unit 0: 2x2 texels, left column red, right column green (all a=255);
    * unit 1: a 2x2 lightmap of 50% grey */
   unsigned char t0[16] = {255, 0, 0, 255, 0, 255, 0, 255,
                           255, 0, 0, 255, 0, 255, 0, 255};
   /* (rows padded to 4 bytes: the default unpack alignment) */
   unsigned char t1[16] = {128, 128, 128, 128, 128, 128, 0, 0,
                           128, 128, 128, 128, 128, 128, 0, 0};
   GLuint tex[2];
   glGenTextures(2, tex);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, tex[0]);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, t0);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glActiveTexture(GL_TEXTURE1);
   glBindTexture(GL_TEXTURE_2D, tex[1]);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, t1);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

   /* a quad over the whole target, as ioq3's tess arrays: xyz as 4 floats,
    * colours as bytes, a texcoord array per unit */
   float xyz[16] = {0, 0, 0, 1, W, 0, 0, 1, W, H, 0, 1, 0, H, 0, 1};
   float st0[8] = {0, 0, 1, 0, 1, 1, 0, 1};
   float st1[8] = {0, 0, 1, 0, 1, 1, 0, 1};
   unsigned char rgba[16];
   memset(rgba, 255, sizeof(rgba));
   unsigned short idx[6] = {0, 1, 2, 0, 2, 3};
   unsigned char px[W * H * 4];
   int fails = 0;

#define CHECK(name, x, y, r, g, b)                                              \
   do {                                                                        \
      glFinish();                                                              \
      glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);                 \
      unsigned char *p = px + ((y) * W + (x)) * 4;                             \
      int ok = abs(p[0] - (r)) <= 3 && abs(p[1] - (g)) <= 3 &&                 \
               abs(p[2] - (b)) <= 3;                                           \
      printf("%s %s: (%d,%d) = %d %d %d, want %d %d %d\n", ok ? "PASS" : "FAIL", \
             name, x, y, p[0], p[1], p[2], r, g, b);                           \
      fails += !ok;                                                            \
   } while (0)

   glEnableClientState(GL_VERTEX_ARRAY);
   glVertexPointer(3, GL_FLOAT, 16, xyz);
   glEnableClientState(GL_COLOR_ARRAY);
   glColorPointer(4, GL_UNSIGNED_BYTE, 0, rgba);

   for (int lock = 0; lock < 2; lock++) {
      const char *nm = lock ? "multitex+lock" : "multitex";
      glClearColor(0, 0, 1, 1);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      glActiveTexture(GL_TEXTURE0);
      glEnable(GL_TEXTURE_2D);
      glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
      glClientActiveTexture(GL_TEXTURE0);
      glEnableClientState(GL_TEXTURE_COORD_ARRAY);
      glTexCoordPointer(2, GL_FLOAT, 0, st0);
      glActiveTexture(GL_TEXTURE1);
      glEnable(GL_TEXTURE_2D);
      glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
      glClientActiveTexture(GL_TEXTURE1);
      glEnableClientState(GL_TEXTURE_COORD_ARRAY);
      glTexCoordPointer(2, GL_FLOAT, 0, st1);
      if (lock && glLockArraysEXT)
         glLockArraysEXT(0, 4);
      glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
      if (lock && glUnlockArraysEXT)
         glUnlockArraysEXT();
      CHECK(nm, 8, 32, 128, 0, 0);
      CHECK(nm, 56, 32, 0, 128, 0);

      /* the HUD: unit 1 off as ioq3 leaves it, a unit-0 quad over the left
       * half with texcoords 0..0.5 (red only) */
      glActiveTexture(GL_TEXTURE1);
      glDisable(GL_TEXTURE_2D);
      glClientActiveTexture(GL_TEXTURE1);
      glDisableClientState(GL_TEXTURE_COORD_ARRAY);
      glActiveTexture(GL_TEXTURE0);
      glClientActiveTexture(GL_TEXTURE0);
      float hx[16] = {0, 0, 0, 1, W / 2, 0, 0, 1, W / 2, H, 0, 1, 0, H, 0, 1};
      float hs[8] = {0, 0, 0.49f, 0, 0.49f, 1, 0, 1};
      glVertexPointer(3, GL_FLOAT, 16, hx);
      glTexCoordPointer(2, GL_FLOAT, 0, hs);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
      CHECK(lock ? "hud-after-lock" : "hud-after", 16, 32, 255, 0, 0);
      CHECK(lock ? "hud-after-lock" : "hud-after", 48, 32, 0, 0, 255);
      glVertexPointer(3, GL_FLOAT, 16, xyz);
   }

   /* distinct texels per unit, unit 1's coordinates mirrored in s and its
    * lightmap NEAREST (a set mixup or a stale texel shows), then one
    * lightmap texel replaced with glTexSubImage2D as Quake II's dynamic
    * lights do */
   {
      G(PFNGLPIXELSTOREIPROC, glPixelStorei);
      G(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D);
      unsigned char a[16] = {255, 0, 0, 255, 0, 255, 0, 255,
                             0, 0, 255, 255, 255, 255, 255, 255};
      unsigned char l[12] = {255, 255, 255, 128, 128, 128,
                             64, 64, 64, 255, 0, 255};
      float sm[8] = {1, 0, 0, 0, 0, 1, 1, 1};
      GLuint u[2];
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glGenTextures(2, u);
      glActiveTexture(GL_TEXTURE0);
      glEnable(GL_TEXTURE_2D);
      glBindTexture(GL_TEXTURE_2D, u[0]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, a);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
      glClientActiveTexture(GL_TEXTURE0);
      glEnableClientState(GL_TEXTURE_COORD_ARRAY);
      glTexCoordPointer(2, GL_FLOAT, 0, st0);
      glActiveTexture(GL_TEXTURE1);
      glEnable(GL_TEXTURE_2D);
      glBindTexture(GL_TEXTURE_2D, u[1]);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, l);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
      glClientActiveTexture(GL_TEXTURE1);
      glEnableClientState(GL_TEXTURE_COORD_ARRAY);
      glTexCoordPointer(2, GL_FLOAT, 0, sm);
      glVertexPointer(3, GL_FLOAT, 16, xyz);
      for (int pass = 0; pass < 2; pass++) {
         if (pass) {
            unsigned char g[3] = {0, 255, 0};
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, g);
            l[0] = 0, l[1] = 255, l[2] = 0;
         }
         glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
         glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
         for (int qy = 0; qy < 2; qy++) {
            for (int qx = 0; qx < 2; qx++) {
               const unsigned char *ta = a + (qy * 2 + qx) * 4,
                                   *tl = l + (qy * 2 + (1 - qx)) * 3;
               CHECK(pass ? "distinct+subimage" : "distinct", 16 + 32 * qx,
                     16 + 32 * qy, ta[0] * tl[0] / 255, ta[1] * tl[1] / 255,
                     ta[2] * tl[2] / 255);
            }
         }
      }
      /* Yamagi's overbright lightmap: unit 1 GL_COMBINE, MODULATE of the
       * texture and the previous unit, RGB_SCALE 2 */
      glActiveTexture(GL_TEXTURE1);
      glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
      glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
      glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
      glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PREVIOUS);
      glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
      glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
      G(PFNGLTEXENVFPROC, glTexEnvf);
      glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 2.0f);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
      for (int qy = 0; qy < 2; qy++) {
         for (int qx = 0; qx < 2; qx++) {
            const unsigned char *ta = a + (qy * 2 + qx) * 4,
                                *tl = l + (qy * 2 + (1 - qx)) * 3;
            int e[3];
            for (int k = 0; k < 3; k++) {
               e[k] = 2 * ta[k] * tl[k] / 255;
               if (e[k] > 255)
                  e[k] = 255;
            }
            CHECK("combine-scale2", 16 + 32 * qx, 16 + 32 * qy, e[0], e[1], e[2]);
         }
      }
      glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0f);
      glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

      /* a GL_LUMINANCE lightmap */
      unsigned char lum[4] = {255, 128, 64, 32};
      glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 2, 2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, lum);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
      for (int qy = 0; qy < 2; qy++) {
         for (int qx = 0; qx < 2; qx++) {
            const unsigned char *ta = a + (qy * 2 + qx) * 4;
            int lv = lum[qy * 2 + (1 - qx)];
            CHECK("luminance-lightmap", 16 + 32 * qx, 16 + 32 * qy,
                  ta[0] * lv / 255, ta[1] * lv / 255, ta[2] * lv / 255);
         }
      }

      /* an NPOT diffuse texture (3x2: R G B / W K Y) under the lightmap */
      unsigned char np[24] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255,
                              255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 0, 255};
      glActiveTexture(GL_TEXTURE0);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 3, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, np);
      glActiveTexture(GL_TEXTURE1);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 2, 0, GL_RGB, GL_UNSIGNED_BYTE, l);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
      {
         /* pixel x 5 + 21.3 i (texel column i), y 16 + 32 j */
         int xs[3] = {8, 32, 56};
         for (int j = 0; j < 2; j++) {
            for (int i = 0; i < 3; i++) {
               const unsigned char *t = np + (j * 3 + i) * 4;
               int lx = xs[i] < 32 ? 1 : 0; /* mirrored lightmap column */
               const unsigned char *tl = l + (j * 2 + lx) * 3;
               if (xs[i] == 32)
                  continue; /* on the lightmap's edge */
               CHECK("npot-diffuse", xs[i], 16 + 32 * j, t[0] * tl[0] / 255,
                     t[1] * tl[1] / 255, t[2] * tl[2] / 255);
            }
         }
      }
      glActiveTexture(GL_TEXTURE1);
      glDisable(GL_TEXTURE_2D);
      glClientActiveTexture(GL_TEXTURE1);
      glDisableClientState(GL_TEXTURE_COORD_ARRAY);
      glActiveTexture(GL_TEXTURE0);
      glClientActiveTexture(GL_TEXTURE0);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
   }

   /* multipass: the lightmap alone, then the texture blended DST_COLOR, ZERO
    * at depth EQUAL */
   glEnable(GL_DEPTH_TEST);
   glDepthFunc(GL_LEQUAL);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
   glActiveTexture(GL_TEXTURE0);
   glBindTexture(GL_TEXTURE_2D, tex[1]);
   glTexCoordPointer(2, GL_FLOAT, 0, st1);
   glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
   CHECK("pass1-lightmap", 8, 32, 128, 128, 128);
   glBindTexture(GL_TEXTURE_2D, tex[0]);
   glTexCoordPointer(2, GL_FLOAT, 0, st0);
   glEnable(GL_BLEND);
   glBlendFunc(GL_DST_COLOR, GL_ZERO);
   glDepthFunc(GL_EQUAL);
   glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx);
   CHECK("pass2-dstcolor-equal", 8, 32, 128, 0, 0);
   CHECK("pass2-dstcolor-equal", 56, 32, 0, 128, 0);

   /* glShadeModel(GL_FLAT): a triangle with a colour per vertex takes the
    * last vertex's (green) everywhere */
   {
      G(PFNGLSHADEMODELPROC, glShadeModel);
      unsigned char vc[12] = {255, 0, 0, 255, 0, 0, 255, 255, 0, 255, 0, 255};
      float tri[12] = {0, 0, 0, 1, W, 0, 0, 1, 0, H, 0, 1};
      unsigned short ti[3] = {0, 1, 2};
      glDisable(GL_BLEND);
      glDisable(GL_DEPTH_TEST);
      glActiveTexture(GL_TEXTURE0);
      glDisable(GL_TEXTURE_2D);
      glClientActiveTexture(GL_TEXTURE0);
      glDisableClientState(GL_TEXTURE_COORD_ARRAY);
      glVertexPointer(3, GL_FLOAT, 16, tri);
      glColorPointer(4, GL_UNSIGNED_BYTE, 0, vc);
      glShadeModel(GL_FLAT);
      glClear(GL_COLOR_BUFFER_BIT);
      glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, ti);
      CHECK("flat-shademodel", 4, 4, 0, 255, 0);
      CHECK("flat-shademodel", 20, 4, 0, 255, 0);
      glShadeModel(GL_SMOOTH);
   }

   /* a flat colour with a texture (a particle: glShadeModel(GL_FLAT)
    * left on, a textured MODULATE quad with a colour per vertex; and the flat
    * colour's alpha through a blend */
   {
      G(PFNGLSHADEMODELPROC, glShadeModel);
      float q[16] = {0, 0, 0, 1, W, 0, 0, 1, W, H, 0, 1, 0, H, 0, 1};
      float qst[8] = {0, 0, 1, 0, 1, 1, 0, 1};
      unsigned short qi[6] = {0, 1, 2, 0, 2, 3};
      unsigned char white[16], halfg[16], cyanish[16];
      memset(white, 255, sizeof(white));
      for (int v = 0; v < 4; v++) {
         unsigned char hg[4] = {0, 255, 0, 128}, cy[4] = {128, 255, 255, 255};
         memcpy(halfg + 4 * v, hg, 4);
         memcpy(cyanish + 4 * v, cy, 4);
      }
      glDisable(GL_DEPTH_TEST);
      glDisable(GL_BLEND);
      glActiveTexture(GL_TEXTURE1);
      glDisable(GL_TEXTURE_2D);
      glClientActiveTexture(GL_TEXTURE1);
      glDisableClientState(GL_TEXTURE_COORD_ARRAY);
      glActiveTexture(GL_TEXTURE0);
      glClientActiveTexture(GL_TEXTURE0);
      glVertexPointer(3, GL_FLOAT, 16, q);
      for (int flat = 0; flat < 2; flat++) {
         char nm[64];
         glShadeModel(flat ? GL_FLAT : GL_SMOOTH);
         /* untextured green, alpha 128, over red */
         glDisable(GL_TEXTURE_2D);
         glDisableClientState(GL_TEXTURE_COORD_ARRAY);
         glClearColor(1, 0, 0, 1);
         glClear(GL_COLOR_BUFFER_BIT);
         glEnable(GL_BLEND);
         glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
         glColorPointer(4, GL_UNSIGNED_BYTE, 0, halfg);
         glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, qi);
         snprintf(nm, sizeof(nm), "%s-alpha", flat ? "flat" : "smooth");
         CHECK(nm, 32, 32, 127, 128, 0);
         glDisable(GL_BLEND);
         /* unit 0's texture (left red, right green), MODULATE */
         glEnable(GL_TEXTURE_2D);
         glBindTexture(GL_TEXTURE_2D, tex[0]);
         glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
         glEnableClientState(GL_TEXTURE_COORD_ARRAY);
         glTexCoordPointer(2, GL_FLOAT, 0, qst);
         glClearColor(0, 0, 1, 1);
         for (int c = 0; c < 2; c++) {
            glClear(GL_COLOR_BUFFER_BIT);
            glColorPointer(4, GL_UNSIGNED_BYTE, 0, c ? cyanish : white);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, qi);
            snprintf(nm, sizeof(nm), "%s-tex-%s", flat ? "flat" : "smooth",
                     c ? "cyanish" : "white");
            CHECK(nm, 8, 32, c ? 128 : 255, 0, 0);
            CHECK(nm, 56, 32, 0, 255, 0);
         }
         /* blended by the texel's alpha (a particle): the red texel's alpha
          * 255, so the left half is the texel times the colour */
         glEnable(GL_BLEND);
         glClear(GL_COLOR_BUFFER_BIT);
         glColorPointer(4, GL_UNSIGNED_BYTE, 0, white);
         glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, qi);
         snprintf(nm, sizeof(nm), "%s-tex-blend", flat ? "flat" : "smooth");
         CHECK(nm, 8, 32, 255, 0, 0);
         glDisable(GL_BLEND);
         glDisableClientState(GL_TEXTURE_COORD_ARRAY);
         glDisable(GL_TEXTURE_2D);
      }
      glShadeModel(GL_SMOOTH);
   }

   printf("%s: %d failed\n", fails ? "FAIL" : "PASS", fails);
   return fails != 0;
}
