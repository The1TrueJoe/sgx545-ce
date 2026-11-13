/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 *
 * Depth and stencil across renders, on a depth/stencil pbuffer:
 *
 *   depth-only clear  a render ends with depth 0.5 and a stencil mark on the
 *                     left; the next starts with glClear(GL_DEPTH_BUFFER_BIT)
 *                     alone (stencil kept, so the render loads it), then a
 *                     quad at depth 0.75 with GL_LESS must pass everywhere
 *                     (ioquake3 starts every frame so), and a stencil-tested
 *                     one only on the left
 *   long frame        a quad at depth 0.25, then enough draws that the
 *                     frame is submitted in parts, then a quad at 0.5 with
 *                     GL_LESS must fail: each part keeps the depth
 *
 *   sgx_depthclear [-v] [draws]   exit 0 when both pass
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#define W 64
#define H 64

static int verbose;
static GLuint prog;
static GLint u_color;

static const char *vs = "attribute vec3 p;\n"
                        "void main() { gl_Position = vec4(p, 1.0); }\n";
static const char *fs = "precision mediump float;\n"
                        "uniform vec4 color;\n"
                        "void main() { gl_FragColor = color; }\n";

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

/* a quad over x0..x1 (clip space) at window depth z (0..1) */
static void
quad(float x0, float x1, float z, const float c[4])
{
   float d = 2.0f * z - 1.0f;
   float v[] = {x0, -1, d, x1, -1, d, x0, 1, d, x1, 1, d};

   glUniform4fv(u_color, 1, c);
   glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, v);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* the pixel at x, y is r, g, b (0 or 255) */
static int
expect(const char *what, int x, int y, int r, int g, int b)
{
   unsigned char px[4];

   glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
   if (verbose || px[0] != r || px[1] != g || px[2] != b)
      printf("  %s at %d,%d: %u %u %u, expected %d %d %d\n", what, x, y, px[0],
             px[1], px[2], r, g, b);
   return px[0] == r && px[1] == g && px[2] == b;
}

static const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1},
                   blue[4] = {0, 0, 1, 1};

static int
depth_only_clear(void)
{
   int ok = 1;

   /* render 1: depth 0.5 over everything, stencil 1 on the left */
   glClearColor(0, 0, 0, 1);
   glClearDepthf(1.0f);
   glClearStencil(0);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
   glEnable(GL_DEPTH_TEST);
   glDepthFunc(GL_ALWAYS);
   glEnable(GL_STENCIL_TEST);
   glStencilFunc(GL_ALWAYS, 1, 0xff);
   glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
   quad(-1, 0, 0.5f, red);
   glStencilFunc(GL_ALWAYS, 0, 0xff);
   quad(0, 1, 0.5f, red);
   glFinish();

   /* render 2: the depth-only clear, then depth 0.75 passes everywhere */
   glClear(GL_DEPTH_BUFFER_BIT);
   glDisable(GL_STENCIL_TEST);
   glDepthFunc(GL_LESS);
   quad(-1, 1, 0.75f, green);
   ok &= expect("depth-only clear, left", W / 4, H / 2, 0, 255, 0);
   ok &= expect("depth-only clear, right", 3 * W / 4, H / 2, 0, 255, 0);

   /* the stencil survived the clear: blue only where it is 1 */
   glEnable(GL_STENCIL_TEST);
   glStencilFunc(GL_EQUAL, 1, 0xff);
   glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
   glDepthFunc(GL_ALWAYS);
   quad(-1, 1, 0.5f, blue);
   glDisable(GL_STENCIL_TEST);
   ok &= expect("stencil kept, left", W / 4, H / 2, 0, 0, 255);
   ok &= expect("stencil kept, right", 3 * W / 4, H / 2, 0, 255, 0);
   printf("%s: depth-only clear\n", ok ? "pass" : "FAIL");
   return ok;
}

static int
long_frame(int draws)
{
   static const float tri[] = {-1, -1, 0.5f, -0.99f, -1, 0.5f, -1, -0.99f, 0.5f};
   int ok = 1;

   glClearColor(0, 0, 0, 1);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
   glEnable(GL_DEPTH_TEST);
   glDepthFunc(GL_LESS);
   quad(-1, 1, 0.25f, blue);
   /* tiny triangles in the corner that write nothing */
   glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
   glDepthMask(GL_FALSE);
   glUniform4fv(u_color, 1, red);
   glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, tri);
   for (int i = 0; i < draws; i++)
      glDrawArrays(GL_TRIANGLES, 0, 3);
   glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
   glDepthMask(GL_TRUE);
   /* behind the first quad: fails */
   quad(-1, 1, 0.5f, green);
   ok &= expect("long frame", W / 2, H / 2, 0, 0, 255);
   printf("%s: long frame (%d draws)\n", ok ? "pass" : "FAIL", draws);
   return ok;
}

int
main(int argc, char **argv)
{
   static const EGLint cattr[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                  EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24,
                                  EGL_STENCIL_SIZE, 8, EGL_SURFACE_TYPE,
                                  EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                                  EGL_OPENGL_ES2_BIT, EGL_NONE};
   static const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
   static const EGLint pb[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
   int draws = 20000, ok = 1;
   EGLDisplay dpy;
   EGLConfig cfg;
   EGLSurface surf;
   EGLContext ctx;
   EGLint n;

   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "-v"))
         verbose = 1;
      else
         draws = atoi(argv[i]);
   }
   setvbuf(stdout, NULL, _IONBF, 0);
   dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   if (!eglInitialize(dpy, NULL, NULL) ||
       !eglChooseConfig(dpy, cattr, &cfg, 1, &n) || !n)
      return printf("no EGL config with depth 24 and stencil 8 (%#x)\n",
                    eglGetError()),
             1;
   surf = eglCreatePbufferSurface(dpy, cfg, pb);
   ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
   if (!surf || !ctx || !eglMakeCurrent(dpy, surf, surf, ctx))
      return printf("surface/context failed %#x\n", eglGetError()), 1;

   prog = glCreateProgram();
   glAttachShader(prog, shader(GL_VERTEX_SHADER, vs));
   glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
   glBindAttribLocation(prog, 0, "p");
   glLinkProgram(prog);
   glUseProgram(prog);
   u_color = glGetUniformLocation(prog, "color");
   glEnableVertexAttribArray(0);
   glViewport(0, 0, W, H);

   ok &= depth_only_clear();
   ok &= long_frame(draws);

   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglTerminate(dpy);
   return ok ? 0 : 1;
}
