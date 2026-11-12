/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * Test H10 (docs/GL3.md): does depthf at the feedback point write
 * gl_FragDepth?  Run with SGX_DEBUG=fragdepth, which builds the program of
 * docs/ISA.md "Phases, visibility feedback and discard" (depthf pa<depth>,
 * sa3, sa4 [end], pass type 5; its form there is a guess).
 *
 * Each case is a 128x64 FBO with a colour and a 24-bit depth buffer, depth
 * cleared to 1:
 *
 *   1. a full quad at z = 0 whose shader writes gl_FragDepth = x / w (a
 *      ramp across the width), depth test ALWAYS, colour green;
 *   2. a full quad at depth 0.5, depth test LESS, colour red: where the ramp
 *      is above 0.5 (the right half) it is drawn, where below kept green.
 *
 * Read back: the colour (green left of the middle, red right of it) and
 * the depth (through the ZLS buffer: the ramp, against x / w).  Without
 * depthf the first quad stores 0.5 everywhere, and the second covers
 * nothing (LESS fails at 0.5).  Cases: no discard; a discard of the top
 * quarter rows (they keep the clear: depth 1, black, then red); the ramp
 * written through a varying instead of gl_FragCoord.
 *
 *   SGX_DEBUG=fragdepth sgx_fragdepth
 *
 * zig cc -target x86_64-linux-gnu.2.36 -I<mesa include> -o sgx_fragdepth
 * sgx_fragdepth.c -ldl
 */
#include <dlfcn.h>
#include <math.h>
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
G(glViewport);
G(glEnable);
G(glDisable);
G(glDepthFunc);
G(glFinish);
G(glGetString);
G(glGetError);
G(glReadPixels);
G(glGenFramebuffers);
G(glBindFramebuffer);
G(glGenRenderbuffers);
G(glBindRenderbuffer);
G(glRenderbufferStorage);
G(glFramebufferRenderbuffer);
G(glCheckFramebufferStatus);
G(glDeleteFramebuffers);
G(glDeleteRenderbuffers);
G(glCreateShader);
G(glShaderSource);
G(glCompileShader);
G(glGetShaderiv);
G(glGetShaderInfoLog);
G(glCreateProgram);
G(glAttachShader);
G(glBindAttribLocation);
G(glLinkProgram);
G(glGetProgramiv);
G(glUseProgram);
G(glGetUniformLocation);
G(glUniform1f);
G(glUniform4f);
G(glVertexAttribPointer);
G(glEnableVertexAttribArray);
G(glDrawArrays);

#define W 128
#define H 64

static const char *vs_src = "attribute vec2 p;\n"
                            "uniform float z;\n"
                            "varying float r;\n"
                            "void main() {\n"
                            "   r = p.x * 0.5 + 0.5;\n"
                            "   gl_Position = vec4(p, z, 1.0);\n"
                            "}\n";

/* the ramp from gl_FragCoord, or the varying; discard the top quarter */
static const char *fs_ramp[] = {
   "uniform vec4 c;\n"
   "void main() {\n"
   "   gl_FragColor = c;\n"
   "   gl_FragDepth = gl_FragCoord.x / 128.0;\n"
   "}\n",
   "uniform vec4 c;\n"
   "void main() {\n"
   "   if (gl_FragCoord.y > 48.0) discard;\n"
   "   gl_FragColor = c;\n"
   "   gl_FragDepth = gl_FragCoord.x / 128.0;\n"
   "}\n",
   "uniform vec4 c;\n"
   "varying float r;\n"
   "void main() {\n"
   "   gl_FragColor = c;\n"
   "   gl_FragDepth = r;\n"
   "}\n",
};
static const char *case_name[] = {"fragcoord ramp", "ramp + discard top",
                                  "varying ramp"};

static const char *fs_flat = "uniform vec4 c;\n"
                             "void main() { gl_FragColor = c; }\n";

static GLuint
shader(GLenum type, const char *src)
{
   GLuint s = glCreateShader_(type);
   GLint ok = 0;
   char log[1024];

   glShaderSource_(s, 1, &src, NULL);
   glCompileShader_(s);
   glGetShaderiv_(s, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      glGetShaderInfoLog_(s, sizeof(log), NULL, log);
      fprintf(stderr, "compile: %s\n", log);
      exit(2);
   }
   return s;
}

static GLuint
program(const char *fs)
{
   GLuint p = glCreateProgram_();
   GLint ok = 0;

   glAttachShader_(p, shader(GL_VERTEX_SHADER, vs_src));
   glAttachShader_(p, shader(GL_FRAGMENT_SHADER, fs));
   glBindAttribLocation_(p, 0, "p");
   glLinkProgram_(p);
   glGetProgramiv_(p, GL_LINK_STATUS, &ok);
   if (!ok) {
      fprintf(stderr, "link failed\n");
      exit(2);
   }
   return p;
}

static void
quad(GLuint prog, float z, float r, float g, float b)
{
   static const GLfloat v[] = {-1, -1, 1, -1, -1, 1, 1, 1};

   glUseProgram_(prog);
   glUniform1f_(glGetUniformLocation_(prog, "z"), z);
   glUniform4f_(glGetUniformLocation_(prog, "c"), r, g, b, 1);
   glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, 0, v);
   glEnableVertexAttribArray_(0);
   glDrawArrays_(GL_TRIANGLE_STRIP, 0, 4);
}

static int failures;

static void
run(unsigned k, GLuint ramp, GLuint flat)
{
   static unsigned char px[W * H * 4];
   static float depth[W * H];
   GLuint f, rb[2];
   int discard = k == 1;

   glGenFramebuffers_(1, &f);
   glBindFramebuffer_(GL_FRAMEBUFFER, f);
   glGenRenderbuffers_(2, rb);
   glBindRenderbuffer_(GL_RENDERBUFFER, rb[0]);
   glRenderbufferStorage_(GL_RENDERBUFFER, GL_RGBA8, W, H);
   glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, rb[0]);
   glBindRenderbuffer_(GL_RENDERBUFFER, rb[1]);
   glRenderbufferStorage_(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
   glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, rb[1]);
   if (glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      printf("FAIL %s: framebuffer incomplete\n", case_name[k]);
      failures++;
      goto out;
   }
   glViewport_(0, 0, W, H);
   glClearColor_(0, 0, 0, 1);
   glClearDepth_(1.0);
   glClear_(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
   glEnable_(GL_DEPTH_TEST);
   glDepthFunc_(GL_ALWAYS);
   quad(ramp, 0.0f, 0, 1, 0); /* depth 0.5 from z, the ramp from depthf */
   glDepthFunc_(GL_LESS);
   quad(flat, 0.0f, 1, 0, 0); /* depth 0.5 */
   glDisable_(GL_DEPTH_TEST);
   glFinish_();
   glReadPixels_(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
   glReadPixels_(0, 0, W, H, GL_DEPTH_COMPONENT, GL_FLOAT, depth);

   /* colour: green where the ramp is below 0.5, red above; the discarded
    * rows red (depth 1 from the clear); a column at the middle either */
   unsigned cok = 0, ctot = 0, dok = 0, dtot = 0, red = 0, green = 0,
            flat05 = 0;
   float worst = 0;

   for (unsigned y = 0; y < H; y++)
      for (unsigned x = 0; x < W; x++) {
         const unsigned char *p = px + 4 * (y * W + x);
         float d = depth[y * W + x];
         float want = (x + 0.5f) / W;
         int is_red = p[0] > 200 && p[1] < 50, is_green = p[1] > 200 && p[0] < 50;
         int dropped = discard && y + 0.5f > 48.0f;

         red += is_red;
         green += is_green;
         flat05 += fabsf(d - 0.5f) < 1e-3f;
         if (x == W / 2 || x == W / 2 - 1)
            continue;
         ctot++;
         cok += dropped ? is_red : (want < 0.5f ? is_green : is_red);
         /* the depth left: the ramp where it is green, 0.5 under the red;
          * discarded rows: 0.5 (red drawn over the clear) */
         dtot++;
         float dw = dropped || want > 0.5f ? 0.5f : want;
         float e = fabsf(d - dw);
         worst = fmaxf(worst, e);
         dok += e < 2.0f / (1 << 16);
      }
   int pass = cok == ctot && dok == dtot;
   failures += !pass;
   printf("%s %s: colour %u/%u, depth %u/%u (worst %.6f); red %u green %u, "
          "depth 0.5 at %u pixels; depth row 8: %.4f %.4f %.4f %.4f\n",
          pass ? "PASS" : "FAIL", case_name[k], cok, ctot, dok, dtot, worst,
          red, green, flat05, depth[8 * W + 0], depth[8 * W + 32],
          depth[8 * W + 63], depth[8 * W + 96]);
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
   GET(glViewport);
   GET(glEnable);
   GET(glDisable);
   GET(glDepthFunc);
   GET(glFinish);
   GET(glGetString);
   GET(glGetError);
   GET(glReadPixels);
   GET(glGenFramebuffers);
   GET(glBindFramebuffer);
   GET(glGenRenderbuffers);
   GET(glBindRenderbuffer);
   GET(glRenderbufferStorage);
   GET(glFramebufferRenderbuffer);
   GET(glCheckFramebufferStatus);
   GET(glDeleteFramebuffers);
   GET(glDeleteRenderbuffers);
   GET(glCreateShader);
   GET(glShaderSource);
   GET(glCompileShader);
   GET(glGetShaderiv);
   GET(glGetShaderInfoLog);
   GET(glCreateProgram);
   GET(glAttachShader);
   GET(glBindAttribLocation);
   GET(glLinkProgram);
   GET(glGetProgramiv);
   GET(glUseProgram);
   GET(glGetUniformLocation);
   GET(glUniform1f);
   GET(glUniform4f);
   GET(glVertexAttribPointer);
   GET(glEnableVertexAttribArray);
   GET(glDrawArrays);

   printf("renderer: %s\n", glGetString_(GL_RENDERER));
   GLuint flat = program(fs_flat);
   for (unsigned k = 0; k < 3; k++)
      run(k, program(fs_ramp[k]), flat);
   return failures ? 1 : 0;
}
