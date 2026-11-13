/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * Uniform blocks (SGX_DEBUG=ubo,glsl130, docs/GL3.md): four quads into a
 * 64x64 RGBA8 FBO, one a quadrant, in one frame, every one placed by a
 * vertex-program block (V, rewritten with glBufferSubData before each draw)
 * and coloured from a fragment-program block:
 *   0 (bottom left)  A.color, red;
 *   1 (bottom right) B.far, a member at byte 1280 (dword 320: past the
 *                    load's immediate 0-127), blue;
 *   2 (top left)     D.arr[i], i a uniform (the offset in a register), green;
 *   3 (top right)    A.color again, after A is rewritten to yellow in the
 *                    same frame (quad 0 must keep red: a copy a draw).
 * Then every pixel of each quadrant is checked against its colour.
 *
 *   SGX_DEBUG=ubo,glsl130 sgx_ubo
 *
 * zig cc -target x86_64-linux-gnu.2.36 -I<mesa include> -o sgx_ubo sgx_ubo.c
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
G(glViewport);
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
G(glGetShaderiv);
G(glGetShaderInfoLog);
G(glCreateProgram);
G(glAttachShader);
G(glLinkProgram);
G(glGetProgramiv);
G(glGetProgramInfoLog);
G(glUseProgram);
G(glGetString);
G(glGetIntegerv);
G(glVertexPointer);
G(glEnableClientState);
G(glDrawArrays);
G(glGenBuffers);
G(glBindBuffer);
G(glBufferData);
G(glBufferSubData);
G(glBindBufferBase);
G(glGetUniformBlockIndex);
G(glUniformBlockBinding);
G(glGetUniformLocation);
G(glUniform1i);
G(glGetError);

#define S 64

static const char *vs_src =
   "#version 120\n"
   "#extension GL_ARB_uniform_buffer_object : require\n"
   "layout(std140) uniform V { vec4 offs; };\n"
   "void main() {\n"
   "   gl_Position = vec4(gl_Vertex.xy * 0.5 + offs.xy, 0.0, 1.0);\n"
   "}\n";

static const char *fs_src[3] = {
   "#version 120\n"
   "#extension GL_ARB_uniform_buffer_object : require\n"
   "layout(std140) uniform A { vec4 color; };\n"
   "void main() { gl_FragColor = color; }\n",

   "#version 120\n"
   "#extension GL_ARB_uniform_buffer_object : require\n"
   "layout(std140) uniform B { vec4 pad[80]; vec4 far; };\n"
   "void main() { gl_FragColor = far; }\n",

   "#version 120\n"
   "#extension GL_ARB_uniform_buffer_object : require\n"
   "layout(std140) uniform D { vec4 arr[4]; };\n"
   "uniform int i;\n"
   "void main() { gl_FragColor = arr[i]; }\n",
};

static GLuint
shader(GLenum type, const char *src)
{
   GLuint sh = glCreateShader_(type);
   GLint ok = 0;
   char log[2048];

   glShaderSource_(sh, 1, &src, NULL);
   glCompileShader_(sh);
   glGetShaderiv_(sh, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      glGetShaderInfoLog_(sh, sizeof(log), NULL, log);
      printf("compile failed:\n%s\n%s\n", src, log);
      exit(1);
   }
   return sh;
}

static GLuint
program(GLuint vs, const char *fs, const char *block, GLuint binding)
{
   GLuint p = glCreateProgram_();
   GLint ok = 0;
   char log[2048];

   glAttachShader_(p, vs);
   glAttachShader_(p, shader(GL_FRAGMENT_SHADER, fs));
   glLinkProgram_(p);
   glGetProgramiv_(p, GL_LINK_STATUS, &ok);
   if (!ok) {
      glGetProgramInfoLog_(p, sizeof(log), NULL, log);
      printf("link failed: %s\n", log);
      exit(1);
   }
   glUniformBlockBinding_(p, glGetUniformBlockIndex_(p, "V"), 0);
   glUniformBlockBinding_(p, glGetUniformBlockIndex_(p, block), binding);
   return p;
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
   GET(glViewport);
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
   GET(glGetShaderiv);
   GET(glGetShaderInfoLog);
   GET(glCreateProgram);
   GET(glAttachShader);
   GET(glLinkProgram);
   GET(glGetProgramiv);
   GET(glGetProgramInfoLog);
   GET(glUseProgram);
   GET(glGetString);
   GET(glGetIntegerv);
   GET(glVertexPointer);
   GET(glEnableClientState);
   GET(glDrawArrays);
   GET(glGenBuffers);
   GET(glBindBuffer);
   GET(glBufferData);
   GET(glBufferSubData);
   GET(glBindBufferBase);
   GET(glGetUniformBlockIndex);
   GET(glUniformBlockBinding);
   GET(glGetUniformLocation);
   GET(glUniform1i);
   GET(glGetError);

   const char *ext = (const char *)glGetString_(GL_EXTENSIONS);
   GLint blocks = 0, size = 0;
   glGetIntegerv_(GL_MAX_FRAGMENT_UNIFORM_BLOCKS, &blocks);
   glGetIntegerv_(GL_MAX_UNIFORM_BLOCK_SIZE, &size);
   printf("renderer: %s, %d fragment blocks of %d bytes\n",
          glGetString_(GL_RENDERER), blocks, size);
   if (!ext || !strstr(ext, "GL_ARB_uniform_buffer_object")) {
      printf("FAIL: no ARB_uniform_buffer_object (SGX_DEBUG=ubo,glsl130?)\n");
      return 1;
   }

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

   GLuint vs = shader(GL_VERTEX_SHADER, vs_src);
   GLuint pa = program(vs, fs_src[0], "A", 1);
   GLuint pb = program(vs, fs_src[1], "B", 2);
   GLuint pd = program(vs, fs_src[2], "D", 3);
   glUseProgram_(pd);
   glUniform1i_(glGetUniformLocation_(pd, "i"), 2);

   /* V: the quad's offset; A: a colour; B: 80 vec4s of grey, then blue;
    * D: four colours, green at 2 */
   static const float red[4] = {1, 0, 0, 1}, yellow[4] = {1, 1, 0, 1};
   static float bdata[81][4], ddata[4][4] = {
      {1, 1, 1, 1}, {1, 0, 1, 1}, {0, 1, 0, 1}, {0, 0, 0, 1}};
   for (unsigned k = 0; k < 80; k++)
      for (unsigned j = 0; j < 4; j++)
         bdata[k][j] = 0.5f;
   bdata[80][0] = bdata[80][1] = 0;
   bdata[80][2] = bdata[80][3] = 1;
   GLuint buf[4];
   glGenBuffers_(4, buf);
   glBindBuffer_(GL_UNIFORM_BUFFER, buf[0]);
   glBufferData_(GL_UNIFORM_BUFFER, 16, NULL, GL_DYNAMIC_DRAW);
   glBindBuffer_(GL_UNIFORM_BUFFER, buf[1]);
   glBufferData_(GL_UNIFORM_BUFFER, 16, red, GL_DYNAMIC_DRAW);
   glBindBuffer_(GL_UNIFORM_BUFFER, buf[2]);
   glBufferData_(GL_UNIFORM_BUFFER, sizeof(bdata), bdata, GL_STATIC_DRAW);
   glBindBuffer_(GL_UNIFORM_BUFFER, buf[3]);
   glBufferData_(GL_UNIFORM_BUFFER, sizeof(ddata), ddata, GL_STATIC_DRAW);
   for (unsigned k = 0; k < 4; k++)
      glBindBufferBase_(GL_UNIFORM_BUFFER, k, buf[k]);

   glViewport_(0, 0, S, S);
   glClearColor_(0, 0, 0, 0);
   glClear_(GL_COLOR_BUFFER_BIT);
   static const GLfloat v[] = {-1, -1, 1, -1, -1, 1, 1, 1};
   glVertexPointer_(2, GL_FLOAT, 0, v);
   glEnableClientState_(GL_VERTEX_ARRAY);

   static const float offs[4][4] = {
      {-0.5f, -0.5f, 0, 0}, {0.5f, -0.5f, 0, 0},
      {-0.5f, 0.5f, 0, 0}, {0.5f, 0.5f, 0, 0}};
   const GLuint progs[4] = {pa, pb, pd, pa};
   for (unsigned q = 0; q < 4; q++) {
      glBindBuffer_(GL_UNIFORM_BUFFER, buf[0]);
      glBufferSubData_(GL_UNIFORM_BUFFER, 0, 16, offs[q]);
      if (q == 3) {
         glBindBuffer_(GL_UNIFORM_BUFFER, buf[1]);
         glBufferSubData_(GL_UNIFORM_BUFFER, 0, 16, yellow);
      }
      glUseProgram_(progs[q]);
      glDrawArrays_(GL_TRIANGLE_STRIP, 0, 4);
   }
   if (glGetError_())
      printf("GL error\n");

   static unsigned char px[S * S * 4];
   glReadPixels_(0, 0, S, S, GL_RGBA, GL_UNSIGNED_BYTE, px);
   static const char *name[4] = {"A.color (red)", "B.far (blue)",
                                 "D.arr[i] (green)",
                                 "A.color rewritten (yellow)"};
   static const unsigned want[4] = {0xff0000ff, 0xffff0000, 0xff00ff00,
                                    0xff00ffff}; /* ABGR as read */
   int bad = 0;
   for (unsigned q = 0; q < 4; q++) {
      unsigned x0 = (q & 1) * S / 2, y0 = (q >> 1) * S / 2, wrong = 0, got = 0;

      for (unsigned y = y0; y < y0 + S / 2; y++)
         for (unsigned x = x0; x < x0 + S / 2; x++) {
            unsigned p;

            memcpy(&p, px + 4 * (y * S + x), 4);
            if (p != want[q]) {
               if (!wrong)
                  got = p;
               wrong++;
            }
         }
      printf("%s quad %u %s: %u/%u wrong%s", wrong ? "FAIL" : "ok  ", q,
             name[q], wrong, S * S / 4, wrong ? "" : "\n");
      if (wrong)
         printf(", first %08x\n", got);
      bad += !!wrong;
   }
   printf("%s\n", bad ? "FAIL" : "PASS");
   return bad ? 1 : 0;
}
