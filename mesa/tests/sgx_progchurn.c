/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 *
 * Programs made, drawn with and deleted, many times over, as a game does when
 * it restarts its renderer: each program is distinct (its own color and
 * offset as literals), so each takes new vertex and pixel code. The code of
 * deleted programs must be used again, and a program must never run another's
 * code: every draw is read back.
 *
 *   sgx_progchurn [-v] [programs]   exit 0 when every draw is right
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#define W 32
#define H 32

static GLuint
shader(GLenum type, const char *src)
{
   GLuint s = glCreateShader(type);
   GLint ok;

   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      fprintf(stderr, "shader didn't compile\n");
      exit(1);
   }
   return s;
}

int
main(int argc, char **argv)
{
   static const EGLint cattr[] = {EGL_RED_SIZE,        8,
                                  EGL_GREEN_SIZE,      8,
                                  EGL_BLUE_SIZE,       8,
                                  EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
                                  EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                  EGL_NONE};
   static const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
   static const EGLint pb[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
   static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
   int programs = 4000, verbose = 0, bad = 0;
   EGLDisplay dpy;
   EGLConfig cfg;
   EGLSurface surf;
   EGLContext ctx;
   EGLint n;

   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "-v"))
         verbose = 1;
      else
         programs = atoi(argv[i]);
   }
   setvbuf(stdout, NULL, _IONBF, 0);
   dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   if (!eglInitialize(dpy, NULL, NULL) ||
       !eglChooseConfig(dpy, cattr, &cfg, 1, &n) || !n)
      return printf("no EGL config (%#x)\n", eglGetError()), 1;
   surf = eglCreatePbufferSurface(dpy, cfg, pb);
   ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
   if (!surf || !ctx || !eglMakeCurrent(dpy, surf, surf, ctx))
      return printf("surface/context failed %#x\n", eglGetError()), 1;
   glViewport(0, 0, W, H);

   for (int i = 0; i < programs; i++) {
      /* the color's channels from i, so neighbors differ */
      int r = (i * 37) & 0xff, g = (i * 101 + 64) & 0xff, b = (i * 13) & 0xff;
      char vs[256], fs[256];
      unsigned char px[4];
      GLuint p = glCreateProgram(), sv, sf;

      snprintf(vs, sizeof(vs),
               "attribute vec2 a;\n"
               "void main() { gl_Position = vec4(a * %d.0 / %d.0, 0.0, 1.0); }\n",
               i + 1, i + 1);
      snprintf(fs, sizeof(fs),
               "precision mediump float;\n"
               "void main() { gl_FragColor = vec4(%d.0, %d.0, %d.0, 255.0) / "
               "255.0; }\n",
               r, g, b);
      sv = shader(GL_VERTEX_SHADER, vs);
      sf = shader(GL_FRAGMENT_SHADER, fs);
      glAttachShader(p, sv);
      glAttachShader(p, sf);
      glBindAttribLocation(p, 0, "a");
      glLinkProgram(p);
      glDeleteShader(sv);
      glDeleteShader(sf);
      glUseProgram(p);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
      glEnableVertexAttribArray(0);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
      if (abs(px[0] - r) > 1 || abs(px[1] - g) > 1 || abs(px[2] - b) > 1) {
         if (bad++ < 10)
            printf("  program %d: %u %u %u, expected %d %d %d\n", i, px[0],
                   px[1], px[2], r, g, b);
      } else if (verbose && i % 500 == 0) {
         printf("  program %d right\n", i);
      }
      glUseProgram(0);
      glDeleteProgram(p);
   }
   printf("%s: %d programs, %d wrong\n", bad ? "FAIL" : "pass", programs, bad);
   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglTerminate(dpy);
   return bad ? 1 : 0;
}
