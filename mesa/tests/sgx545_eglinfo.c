/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

/*
 * sgx545_eglinfo: what eglinfo prints for the default display, plus the GL
 * strings of a GLES2 context made current on a pbuffer.
 */

#include <stdio.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

int
main(void)
{
   EGLDisplay dpy;

   setvbuf(stdout, NULL, _IONBF, 0);
   dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   EGLint maj, min, n = 0;
   EGLConfig cfgs[256], cfg;
   static const EGLint cattr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                   EGL_OPENGL_ES2_BIT, EGL_NONE };
   static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   static const EGLint pb_attr[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
   EGLSurface surf;
   EGLContext ctx;

   if (!eglInitialize(dpy, &maj, &min)) {
      printf("eglInitialize failed: %#x\n", eglGetError());
      return 1;
   }
   printf("EGL API version: %d.%d\n", maj, min);
   printf("EGL vendor string: %s\n", eglQueryString(dpy, EGL_VENDOR));
   printf("EGL version string: %s\n", eglQueryString(dpy, EGL_VERSION));
   printf("EGL client APIs: %s\n", eglQueryString(dpy, EGL_CLIENT_APIS));
   printf("EGL extensions string:\n    %s\n", eglQueryString(dpy, EGL_EXTENSIONS));
   eglGetConfigs(dpy, cfgs, 256, &n);
   printf("Configurations: %d\n     bf lv d st colorbuffer dp st  ms    vis  supported\n", n);
   for (int i = 0; i < n; i++) {
      EGLint id, size, r, g, b, a, d, s, type, rt;

      eglGetConfigAttrib(dpy, cfgs[i], EGL_CONFIG_ID, &id);
      eglGetConfigAttrib(dpy, cfgs[i], EGL_BUFFER_SIZE, &size);
      eglGetConfigAttrib(dpy, cfgs[i], EGL_RED_SIZE, &r);
      eglGetConfigAttrib(dpy, cfgs[i], EGL_GREEN_SIZE, &g);
      eglGetConfigAttrib(dpy, cfgs[i], EGL_BLUE_SIZE, &b);
      eglGetConfigAttrib(dpy, cfgs[i], EGL_ALPHA_SIZE, &a);
      eglGetConfigAttrib(dpy, cfgs[i], EGL_DEPTH_SIZE, &d);
      eglGetConfigAttrib(dpy, cfgs[i], EGL_STENCIL_SIZE, &s);
      eglGetConfigAttrib(dpy, cfgs[i], EGL_SURFACE_TYPE, &type);
      eglGetConfigAttrib(dpy, cfgs[i], EGL_RENDERABLE_TYPE, &rt);
      printf("0x%02x %2d  0  y  . %2d %2d %2d %2d %2d %2d  0  0x%02x %s%s %s\n", id, size, r, g, b,
             a, d, s, type, type & EGL_WINDOW_BIT ? "win " : "", type & EGL_PBUFFER_BIT ? "pb" : "",
             rt & EGL_OPENGL_ES2_BIT ? "es2" : "");
   }

   if (!eglChooseConfig(dpy, cattr, &cfg, 1, &n) || n < 1) {
      printf("no GLES2 pbuffer config\n");
      return 1;
   }
   surf = eglCreatePbufferSurface(dpy, cfg, pb_attr);
   ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
   if (!surf || !ctx || !eglMakeCurrent(dpy, surf, surf, ctx)) {
      printf("context failed: %#x\n", eglGetError());
      return 1;
   }
   printf("GL_VENDOR: %s\n", glGetString(GL_VENDOR));
   printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
   printf("GL_VERSION: %s\n", glGetString(GL_VERSION));
   printf("GL_SHADING_LANGUAGE_VERSION: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
   printf("GL_EXTENSIONS: %s\n", glGetString(GL_EXTENSIONS));
   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglTerminate(dpy);
   return 0;
}
