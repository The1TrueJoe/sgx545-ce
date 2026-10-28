/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 *
 * EGL initialised, used, terminated and initialised again in one process, as
 * gl4es does (its hardware probe, then the real display). The EGL and GLES
 * libraries are dlopen'd, so one i386 build runs on the DDK and on ours:
 *
 *   sgx545_reinit LIBEGL LIBGLESV2 [rounds] [w h]
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#define EGL_EGL_PROTOTYPES 0
#define GL_GLES_PROTOTYPES 0
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#define F(lib, type, name) type name = (type)dlsym(lib, #name)

int
main(int argc, char **argv)
{
   void *egl, *gles;
   int rounds = argc > 3 ? atoi(argv[3]) : 2, w = argc > 5 ? atoi(argv[4]) : 300,
       h = argc > 5 ? atoi(argv[5]) : 300;

   if (argc < 3)
      return fprintf(stderr, "usage: %s LIBEGL LIBGLESV2 [rounds] [w h]\n", argv[0]), 2;
   setvbuf(stdout, NULL, _IONBF, 0);
   egl = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
   gles = dlopen(argv[2], RTLD_NOW | RTLD_GLOBAL);
   if (!egl || !gles)
      return fprintf(stderr, "dlopen: %s\n", dlerror()), 1;
   F(egl, PFNEGLGETDISPLAYPROC, eglGetDisplay);
   F(egl, PFNEGLINITIALIZEPROC, eglInitialize);
   F(egl, PFNEGLCHOOSECONFIGPROC, eglChooseConfig);
   F(egl, PFNEGLCREATEPBUFFERSURFACEPROC, eglCreatePbufferSurface);
   F(egl, PFNEGLCREATECONTEXTPROC, eglCreateContext);
   F(egl, PFNEGLMAKECURRENTPROC, eglMakeCurrent);
   F(egl, PFNEGLDESTROYSURFACEPROC, eglDestroySurface);
   F(egl, PFNEGLDESTROYCONTEXTPROC, eglDestroyContext);
   F(egl, PFNEGLTERMINATEPROC, eglTerminate);
   F(egl, PFNEGLGETERRORPROC, eglGetError);
   F(gles, PFNGLCLEARCOLORPROC, glClearColor);
   F(gles, PFNGLCLEARPROC, glClear);
   F(gles, PFNGLREADPIXELSPROC, glReadPixels);

   for (int r = 0; r < rounds; r++) {
      static const EGLint cattr[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                      EGL_DEPTH_SIZE, 16, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
      static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
      const EGLint pb[] = { EGL_WIDTH, r ? w : 32, EGL_HEIGHT, r ? h : 32, EGL_NONE };
      EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
      EGLConfig cfg;
      EGLSurface surf;
      EGLContext ctx;
      EGLint n;
      unsigned char px[4];

      if (!eglInitialize(dpy, NULL, NULL) || !eglChooseConfig(dpy, cattr, &cfg, 1, &n) || !n)
         return printf("round %d: EGL init failed %#x\n", r, eglGetError()), 1;
      printf("round %d: initialised, creating a %dx%d pbuffer\n", r, pb[1], pb[3]);
      surf = eglCreatePbufferSurface(dpy, cfg, pb);
      ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
      if (!surf || !ctx || !eglMakeCurrent(dpy, surf, surf, ctx))
         return printf("round %d: surface/context failed %#x\n", r, eglGetError()), 1;
      glClearColor(0, 1, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
      printf("round %d: pixel %u %u %u %u\n", r, px[0], px[1], px[2], px[3]);
      eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      eglDestroyContext(dpy, ctx);
      eglDestroySurface(dpy, surf);
      eglTerminate(dpy);
   }
   return 0;
}
