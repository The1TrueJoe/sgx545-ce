/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 *
 * A depth texture as a framebuffer's depth attachment, as Minecraft 1.15+
 * builds its main target: desktop GL 2.1 through EXT_framebuffer_object, an
 * RGBA8 color texture and a GL_DEPTH_COMPONENT texture made with GL_FLOAT
 * and no data.  At W x H (default 854 x 480):
 *
 *   status    the framebuffer is complete
 *   depth     left half at depth 0.25, then everything at 0.75, then (after a
 *             one-pixel readback, which splits the render) everything at 0.5,
 *             all with GL_LESS: green on the left, blue on the right
 *   readback  glReadPixels of GL_DEPTH_COMPONENT gives 0.25 and 0.5
 *   sample    the depth texture drawn into the window: 0.25 and 0.5 as grey
 *   upload    the texture set to 0.6 by glTexSubImage2D, then drawn into
 *             without a clear: a quad at 0.75 fails, one at 0.5 passes
 *
 *   sgx_depthtex [-v] [W H]   exit 0 when every check passes
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

/* EXT_framebuffer_object, as Minecraft 1.16 calls it on GL 2.1 */
static PFNGLGENFRAMEBUFFERSEXTPROC GenFramebuffersEXT;
static PFNGLBINDFRAMEBUFFEREXTPROC BindFramebufferEXT;
static PFNGLFRAMEBUFFERTEXTURE2DEXTPROC FramebufferTexture2DEXT;
static PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC CheckFramebufferStatusEXT;

static int verbose, fails;
static unsigned W = 854, H = 480;
static GLuint prog_color, prog_tex;
static GLint u_color;

static void check(const char *what, int ok) {
  printf("%-10s %s\n", what, ok ? "pass" : "FAIL");
  fails += !ok;
}

static GLuint program(const char *vs, const char *fs) {
  GLuint p = glCreateProgram();
  const char *src[2] = {vs, fs};
  GLenum type[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
  GLint ok;

  for (int i = 0; i < 2; i++) {
    GLuint s = glCreateShader(type[i]);

    glShaderSource(s, 1, &src[i], NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      fprintf(stderr, "shader didn't compile\n");
      exit(2);
    }
    glAttachShader(p, s);
  }
  glBindAttribLocation(p, 0, "p");
  glLinkProgram(p);
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    fprintf(stderr, "program didn't link\n");
    exit(2);
  }
  return p;
}

/* a quad over x0..x1 (clip space) at window depth z (0..1) */
static void quad(float x0, float x1, float z) {
  float d = 2.0f * z - 1.0f;
  float v[] = {x0, -1, d, x1, -1, d, x0, 1, d, x1, 1, d};

  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, v);
  glEnableVertexAttribArray(0);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void color_quad(float x0, float x1, float z, float r, float g, float b) {
  glUseProgram(prog_color);
  glUniform4f(u_color, r, g, b, 1);
  quad(x0, x1, z);
}

static int near(int a, int b, int tol) { return abs(a - b) <= tol; }

/* the pixel at (x, y) of the bound framebuffer is (r, g, b), within tol */
static int pixel_is(unsigned x, unsigned y, int r, int g, int b, int tol) {
  GLubyte p[4];

  glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
  if (verbose ||
      !(near(p[0], r, tol) && near(p[1], g, tol) && near(p[2], b, tol)))
    printf("  (%u, %u): %u %u %u, expected %d %d %d\n", x, y, p[0], p[1], p[2],
           r, g, b);
  return near(p[0], r, tol) && near(p[1], g, tol) && near(p[2], b, tol);
}

/* the left and right columns of the middle row */
static int halves_are(int lr, int lg, int lb, int rr, int rg, int rb, int tol) {
  int ok = 1;

  ok &= pixel_is(W / 8, H / 2, lr, lg, lb, tol);
  ok &= pixel_is(W / 2 - 2, H / 3, lr, lg, lb, tol);
  ok &= pixel_is(W / 2 + 2, H / 2, rr, rg, rb, tol);
  ok &= pixel_is(W - 1, H - 1, rr, rg, rb, tol);
  return ok;
}

int main(int argc, char **argv) {
  EGLDisplay dpy;
  EGLConfig cfg;
  EGLContext ctx;
  EGLSurface surf;
  EGLint n;
  GLuint tex_color, tex_depth, fb;
  GLenum status;
  float *depths;
  int ok;

  if (argc > 1 && !strcmp(argv[1], "-v"))
    verbose = 1, argc--, argv++;
  if (argc > 2)
    W = atoi(argv[1]), H = atoi(argv[2]);

  dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_API)) {
    fprintf(stderr, "no EGL with desktop GL\n");
    return 2;
  }
  {
    const EGLint ca[] = {EGL_SURFACE_TYPE,
                         EGL_PBUFFER_BIT,
                         EGL_RENDERABLE_TYPE,
                         EGL_OPENGL_BIT,
                         EGL_RED_SIZE,
                         8,
                         EGL_GREEN_SIZE,
                         8,
                         EGL_BLUE_SIZE,
                         8,
                         EGL_NONE};
    const EGLint sa[] = {EGL_WIDTH, (EGLint)W, EGL_HEIGHT, (EGLint)H, EGL_NONE};

    if (!eglChooseConfig(dpy, ca, &cfg, 1, &n) || !n) {
      fprintf(stderr, "no config\n");
      return 2;
    }
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, NULL);
    surf = eglCreatePbufferSurface(dpy, cfg, sa);
    if (!ctx || !surf || !eglMakeCurrent(dpy, surf, surf, ctx)) {
      fprintf(stderr, "no context\n");
      return 2;
    }
  }
  printf("%s, %s, %ux%u\n", glGetString(GL_RENDERER), glGetString(GL_VERSION),
         W, H);
  GenFramebuffersEXT =
      (PFNGLGENFRAMEBUFFERSEXTPROC)eglGetProcAddress("glGenFramebuffersEXT");
  BindFramebufferEXT =
      (PFNGLBINDFRAMEBUFFEREXTPROC)eglGetProcAddress("glBindFramebufferEXT");
  FramebufferTexture2DEXT = (PFNGLFRAMEBUFFERTEXTURE2DEXTPROC)eglGetProcAddress(
      "glFramebufferTexture2DEXT");
  CheckFramebufferStatusEXT =
      (PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC)eglGetProcAddress(
          "glCheckFramebufferStatusEXT");
  if (!strstr((const char *)glGetString(GL_EXTENSIONS),
              "GL_EXT_framebuffer_object") ||
      !GenFramebuffersEXT || !BindFramebufferEXT || !FramebufferTexture2DEXT ||
      !CheckFramebufferStatusEXT) {
    fprintf(stderr, "no EXT_framebuffer_object\n");
    return 2;
  }

  prog_color = program("#version 120\nattribute vec3 p;\n"
                       "void main() { gl_Position = vec4(p, 1.0); }\n",
                       "#version 120\nuniform vec4 color;\n"
                       "void main() { gl_FragColor = color; }\n");
  u_color = glGetUniformLocation(prog_color, "color");
  prog_tex = program(
      "#version 120\nattribute vec3 p;\nvarying vec2 uv;\n"
      "void main() { uv = p.xy * 0.5 + 0.5; gl_Position = vec4(p, 1.0); }\n",
      "#version 120\nuniform sampler2D t;\nvarying vec2 uv;\n"
      "void main() { float d = texture2D(t, uv).r;"
      " gl_FragColor = vec4(d, d, d, 1.0); }\n");

  /* Minecraft's RenderTarget: color, then depth, NEAREST and CLAMP */
  glGenTextures(1, &tex_color);
  glBindTexture(GL_TEXTURE_2D, tex_color);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               NULL);
  glGenTextures(1, &tex_depth);
  glBindTexture(GL_TEXTURE_2D, tex_depth);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, W, H, 0,
               GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
  GenFramebuffersEXT(1, &fb);
  BindFramebufferEXT(GL_FRAMEBUFFER_EXT, fb);
  FramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                          GL_TEXTURE_2D, tex_color, 0);
  FramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT,
                          GL_TEXTURE_2D, tex_depth, 0);
  status = CheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
  if (status != GL_FRAMEBUFFER_COMPLETE_EXT)
    printf("  status %#x\n", status);
  check("status", status == GL_FRAMEBUFFER_COMPLETE_EXT);
  if (status != GL_FRAMEBUFFER_COMPLETE_EXT)
    return 1;

  /* depth: kept within a render and across a split */
  glViewport(0, 0, W, H);
  glClearColor(0, 0, 0, 1);
  glClearDepth(1.0);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  color_quad(-1, 0, 0.25f, 0, 1, 0);
  color_quad(-1, 1, 0.75f, 1, 0, 0);
  pixel_is(0, 0, 0, 255, 0, 0); /* ends the render */
  color_quad(-1, 1, 0.5f, 0, 0, 1);
  check("depth", halves_are(0, 255, 0, 0, 0, 255, 0));

  /* readback: the depth the renders stored */
  depths = malloc(sizeof(float) * W * H);
  glReadPixels(0, 0, W, H, GL_DEPTH_COMPONENT, GL_FLOAT, depths);
  ok = 1;
  for (unsigned y = 0; y < H; y += 7)
    for (unsigned x = 0; x < W; x += 3) {
      float want = x < W / 2 ? 0.25f : 0.5f;

      if (fabsf(depths[y * W + x] - want) > 1e-5f) {
        if (ok || verbose)
          printf("  depth (%u, %u) %f, expected %f\n", x, y, depths[y * W + x],
                 want);
        ok = 0;
      }
    }
  check("readback", ok);

  /* sample: the depth texture drawn into the window */
  BindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
  glDisable(GL_DEPTH_TEST);
  glClearColor(1, 0, 1, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(prog_tex);
  glUniform1i(glGetUniformLocation(prog_tex, "t"), 0);
  glBindTexture(GL_TEXTURE_2D, tex_depth);
  quad(-1, 1, 0.5f);
  check("sample", halves_are(64, 64, 64, 128, 128, 128, 1));

  /* upload: a render that doesn't clear starts from the texture's depth */
  for (unsigned i = 0; i < W * H; i++)
    depths[i] = 0.6f;
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_DEPTH_COMPONENT, GL_FLOAT,
                  depths);
  BindFramebufferEXT(GL_FRAMEBUFFER_EXT, fb);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);
  color_quad(-1, 1, 0.75f, 1, 0, 0);
  color_quad(-1, 0, 0.5f, 0, 1, 0);
  check("upload", halves_are(0, 255, 0, 0, 0, 0, 0));

  free(depths);
  eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglTerminate(dpy);
  printf("%s\n", fails ? "FAIL" : "PASS");
  return fails ? 1 : 0;
}
