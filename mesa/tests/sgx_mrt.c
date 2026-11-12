/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * MRT a pass a colour buffer (SGX_DEBUG=mrt, docs/GL3.md): a 64x64 FBO with
 * four RGBA8 colour textures and a 24-bit depth buffer.  A shader writes a
 * different colour into each gl_FragData[i]; a quad near the eye over the
 * left two thirds, then one farther away over the right two thirds with
 * depth LESS.  Each buffer must have the near quad's colour on the left,
 * the far one's on the right and the near one's where they overlap (the
 * later passes' depth EQUAL keeping the first pass's result), the clear
 * colour nowhere.
 *
 *   SGX_DEBUG=mrt sgx_mrt
 *
 * zig cc -target x86_64-linux-gnu.2.36 -I<mesa include> -o sgx_mrt sgx_mrt.c
 * -ldl
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
G(glClearDepth);
G(glViewport);
G(glEnable);
G(glDepthFunc);
G(glReadPixels);
G(glReadBuffer);
G(glDrawBuffers);
G(glGetIntegerv);
G(glGetString);
G(glGetError);
G(glGenTextures);
G(glBindTexture);
G(glTexImage2D);
G(glTexParameteri);
G(glGenFramebuffers);
G(glBindFramebuffer);
G(glFramebufferTexture2D);
G(glGenRenderbuffers);
G(glBindRenderbuffer);
G(glRenderbufferStorage);
G(glFramebufferRenderbuffer);
G(glCheckFramebufferStatus);
G(glCreateShader);
G(glShaderSource);
G(glCompileShader);
G(glCreateProgram);
G(glAttachShader);
G(glBindAttribLocation);
G(glLinkProgram);
G(glGetProgramiv);
G(glUseProgram);
G(glGetUniformLocation);
G(glUniform1f);
G(glVertexAttribPointer);
G(glEnableVertexAttribArray);
G(glDrawArrays);

#define S 64
#define N 4

static const char *vs = "#version 110\n"
                        "attribute vec2 p;\n"
                        "uniform float z;\n"
                        "void main() { gl_Position = vec4(p, z, 1.0); }\n";
static const char *fs = "#version 110\n"
                        "uniform float far;\n"
                        "void main() {\n"
                        "   gl_FragData[0] = vec4(1.0, far, 0.0, 1.0);\n"
                        "   gl_FragData[1] = vec4(0.0, 1.0, far, 1.0);\n"
                        "   gl_FragData[2] = vec4(far, 0.0, 1.0, 1.0);\n"
                        "   gl_FragData[3] = vec4(1.0, 1.0, far, 1.0);\n"
                        "}\n";

/* buffer i's colour for the near (far = 0) or far (far = 1) quad */
static void
want(unsigned i, int far, unsigned char c[4])
{
   const float f = far ? 1.0f : 0.0f;
   const float v[N][4] = {{1, f, 0, 1}, {0, 1, f, 1}, {f, 0, 1, 1}, {1, 1, f, 1}};

   for (unsigned k = 0; k < 4; k++)
      c[k] = v[i][k] > 0.5f ? 255 : 0;
}

static void
quad(GLuint p, float x0, float x1, float z, float far)
{
   const GLfloat v[] = {x0, -1, x1, -1, x0, 1, x1, 1};

   glUniform1f_(glGetUniformLocation_(p, "z"), z);
   glUniform1f_(glGetUniformLocation_(p, "far"), far);
   glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, 0, v);
   glEnableVertexAttribArray_(0);
   glDrawArrays_(GL_TRIANGLE_STRIP, 0, 4);
}

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
   GET(glClearDepth);
   GET(glViewport);
   GET(glEnable);
   GET(glDepthFunc);
   GET(glReadPixels);
   GET(glReadBuffer);
   GET(glDrawBuffers);
   GET(glGetIntegerv);
   GET(glGetString);
   GET(glGetError);
   GET(glGenTextures);
   GET(glBindTexture);
   GET(glTexImage2D);
   GET(glTexParameteri);
   GET(glGenFramebuffers);
   GET(glBindFramebuffer);
   GET(glFramebufferTexture2D);
   GET(glGenRenderbuffers);
   GET(glBindRenderbuffer);
   GET(glRenderbufferStorage);
   GET(glFramebufferRenderbuffer);
   GET(glCheckFramebufferStatus);
   GET(glCreateShader);
   GET(glShaderSource);
   GET(glCompileShader);
   GET(glCreateProgram);
   GET(glAttachShader);
   GET(glBindAttribLocation);
   GET(glLinkProgram);
   GET(glGetProgramiv);
   GET(glUseProgram);
   GET(glGetUniformLocation);
   GET(glUniform1f);
   GET(glVertexAttribPointer);
   GET(glEnableVertexAttribArray);
   GET(glDrawArrays);

   GLint maxdb = 0;
   glGetIntegerv_(GL_MAX_DRAW_BUFFERS, &maxdb);
   printf("renderer: %s, GL_MAX_DRAW_BUFFERS %d\n", glGetString_(GL_RENDERER),
          maxdb);
   if (maxdb < N) {
      printf("FAIL: fewer than %d draw buffers (SGX_DEBUG=mrt?)\n", N);
      return 1;
   }

   GLuint tex[N], fb, ds;
   const GLenum bufs[N] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1,
                           GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
   glGenFramebuffers_(1, &fb);
   glBindFramebuffer_(GL_FRAMEBUFFER, fb);
   glGenTextures_(N, tex);
   for (unsigned i = 0; i < N; i++) {
      glBindTexture_(GL_TEXTURE_2D, tex[i]);
      glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, S, S, 0, GL_RGBA,
                    GL_UNSIGNED_BYTE, NULL);
      glFramebufferTexture2D_(GL_FRAMEBUFFER, bufs[i], GL_TEXTURE_2D, tex[i],
                              0);
   }
   glGenRenderbuffers_(1, &ds);
   glBindRenderbuffer_(GL_RENDERBUFFER, ds);
   glRenderbufferStorage_(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, S, S);
   glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, ds);
   glDrawBuffers_(N, bufs);
   if (glCheckFramebufferStatus_(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      printf("FAIL: framebuffer incomplete\n");
      return 1;
   }

   const char *src[2] = {vs, fs};
   GLenum type[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
   GLuint p = glCreateProgram_();
   for (int i = 0; i < 2; i++) {
      GLuint sh = glCreateShader_(type[i]);
      glShaderSource_(sh, 1, &src[i], NULL);
      glCompileShader_(sh);
      glAttachShader_(p, sh);
   }
   glBindAttribLocation_(p, 0, "p");
   glLinkProgram_(p);
   glUseProgram_(p);

   glViewport_(0, 0, S, S);
   glClearColor_(0.5f, 0.5f, 0.5f, 0.5f);
   glClearDepth_(1.0);
   glClear_(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
   glEnable_(GL_DEPTH_TEST);
   glDepthFunc_(GL_LESS);
   /* near: x -1 .. 1/3; far: -1/3 .. 1 */
   quad(p, -1.0f, 1.0f / 3, -0.5f, 0.0f);
   quad(p, -1.0f / 3, 1.0f, 0.5f, 1.0f);

   int failures = 0;
   static unsigned char px[S * S * 4];
   for (unsigned i = 0; i < N; i++) {
      glReadBuffer_(bufs[i]);
      glReadPixels_(0, 0, S, S, GL_RGBA, GL_UNSIGNED_BYTE, px);
      unsigned ok = 0, tot = 0;
      for (unsigned y = 0; y < S; y++)
         for (unsigned x = 0; x < S; x++) {
            /* pixel centre in clip space; skip the two seams */
            float cx = (x + 0.5f) / S * 2 - 1;
            if ((cx > -1.0f / 3 - 0.04f && cx < -1.0f / 3 + 0.04f) ||
                (cx > 1.0f / 3 - 0.04f && cx < 1.0f / 3 + 0.04f))
               continue;
            unsigned char w[4];
            want(i, cx > 1.0f / 3, w);
            tot++;
            ok += px[4 * (y * S + x)] == w[0] &&
                  px[4 * (y * S + x) + 1] == w[1] &&
                  px[4 * (y * S + x) + 2] == w[2];
         }
      const unsigned char *l = px + 4 * (32 * S + 4), *r = px + 4 * (32 * S + 60);
      printf("%s buffer %u: %u/%u pixels; left %02x%02x%02x, right %02x%02x%02x\n",
             ok == tot ? "PASS" : "FAIL", i, ok, tot, l[0], l[1], l[2], r[0],
             r[1], r[2]);
      failures += ok != tot;
   }
   GLenum e = glGetError_();
   if (e) {
      printf("FAIL: glerror %#x\n", e);
      failures++;
   }
   return failures ? 1 : 0;
}
