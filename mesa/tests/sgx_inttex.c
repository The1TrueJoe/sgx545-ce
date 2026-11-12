/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * Integer textures and integer render targets (docs/GL3.md), under
 * SGX_DEBUG=glsl130 (GLSL 1.30, EXT_texture_integer):
 *
 *   sample  a 16x8 texture of each integer format (RGBA8UI, RGBA8I, RG8UI,
 *           R8UI, R32UI, R32I, RG32UI, RGBA32UI, RGBA32I) whose every
 *           texel differs, read with
 *           texelFetch into an RGBA8 framebuffer as bytes (an R32 value's
 *           four bytes; an 8-bit channel's byte): every texel against what
 *           was uploaded, the missing channels as (0, 0, 1)
 *   render  a 16x8 integer colour buffer (RGBA8UI, RG8UI, R8UI, R32UI,
 *           RG32UI, RGBA32UI: the last two a pass a channel)
 *           cleared with glClearColorIuiEXT, then a draw over its left half
 *           writing values made from the pixel's position; read back with
 *           GL_RGBA_INTEGER: the left half the draw's, the right the clear's
 *
 *   SGX_DEBUG=glsl130 sgx_inttex
 *
 * zig cc -target x86_64-linux-gnu.2.36 -I<mesa include> -o sgx_inttex
 * sgx_inttex.c -ldl
 */
#include <dlfcn.h>
#include <stdint.h>
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
G(glGetString);
G(glGetError);
G(glReadPixels);
G(glPixelStorei);
G(glGenTextures);
G(glBindTexture);
G(glTexImage2D);
G(glTexParameteri);
G(glDeleteTextures);
G(glGenFramebuffers);
G(glBindFramebuffer);
G(glFramebufferTexture2D);
G(glCheckFramebufferStatus);
G(glDeleteFramebuffers);
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
G(glGetProgramInfoLog);
G(glUseProgram);
G(glGetUniformLocation);
G(glUniform1i);
G(glVertexAttribPointer);
G(glEnableVertexAttribArray);
G(glDrawArrays);
G(glClearColorIuiEXT);

#define W 16
#define H 8

static int failures;

static GLuint
program(const char *fs)
{
   static const char *vs = "#version 130\n"
                           "in vec2 p;\n"
                           "void main() { gl_Position = vec4(p, 0.0, 1.0); }\n";
   const char *src[2] = {vs, fs};
   GLenum type[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
   GLuint p = glCreateProgram_();
   GLint ok = 0;
   char log[2048];

   for (int i = 0; i < 2; i++) {
      GLuint s = glCreateShader_(type[i]);

      glShaderSource_(s, 1, &src[i], NULL);
      glCompileShader_(s);
      glGetShaderiv_(s, GL_COMPILE_STATUS, &ok);
      if (!ok) {
         glGetShaderInfoLog_(s, sizeof(log), NULL, log);
         printf("  compile: %s\n", log);
         return 0;
      }
      glAttachShader_(p, s);
   }
   glBindAttribLocation_(p, 0, "p");
   glLinkProgram_(p);
   glGetProgramiv_(p, GL_LINK_STATUS, &ok);
   if (!ok) {
      glGetProgramInfoLog_(p, sizeof(log), NULL, log);
      printf("  link: %s\n", log);
      return 0;
   }
   return p;
}

static void
quad(float x1)
{
   const GLfloat v[] = {-1, -1, x1, -1, -1, 1, x1, 1};

   glVertexAttribPointer_(0, 2, GL_FLOAT, GL_FALSE, 0, v);
   glEnableVertexAttribArray_(0);
   glDrawArrays_(GL_TRIANGLE_STRIP, 0, 4);
}

static GLuint
fbo_for(GLuint tex)
{
   GLuint f;

   glGenFramebuffers_(1, &f);
   glBindFramebuffer_(GL_FRAMEBUFFER, f);
   glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex, 0);
   return f;
}

static GLuint
texture(GLenum ifmt, GLenum fmt, GLenum type, const void *data)
{
   GLuint t;

   glGenTextures_(1, &t);
   glBindTexture_(GL_TEXTURE_2D, t);
   glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri_(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glPixelStorei_(GL_UNPACK_ALIGNMENT, 1);
   glTexImage2D_(GL_TEXTURE_2D, 0, ifmt, W, H, 0, fmt, type, data);
   return t;
}

/* the value of channel c of texel i (8 bits used for the 8-bit formats) */
static uint32_t
pattern(unsigned i, unsigned c)
{
   static const uint32_t k[4] = {0x9e3779b1u, 0x85ebca77u, 0xc2b2ae3du,
                                 0x27d4eb2fu};
   return (i + 1) * k[c] ^ (c << 29);
}

struct sfmt {
   const char *name;
   GLenum ifmt, fmt, type;
   unsigned comps, bytes; /* channels stored, bytes a channel */
   int sign;
};

static void
sample_case(const struct sfmt *f)
{
   uint8_t data[W * H * 16];
   uint8_t px[W * H * 4];
   char fs[1024];
   const char *s = f->sign ? "isampler2D" : "usampler2D";

   for (unsigned i = 0; i < W * H; i++)
      for (unsigned c = 0; c < f->comps; c++) {
         uint32_t v = pattern(i, c);
         memcpy(data + (i * f->comps + c) * f->bytes, &v, f->bytes);
      }
   /* the texel's bytes into the colour: an R32 value's four, else each
    * channel's low byte */
   if (f->bytes == 4)
      snprintf(fs, sizeof(fs),
               "#version 130\n"
               "uniform %s t;\n"
               "uniform int c;\n"
               "void main() {\n"
               "   uint v = uint(texelFetch(t, ivec2(gl_FragCoord.xy), 0)[c]);\n"
               "   gl_FragColor = vec4(uvec4(v, v >> 8u, v >> 16u, v >> 24u) & 255u) / 255.0;\n"
               "}\n",
               s);
   else
      snprintf(fs, sizeof(fs),
               "#version 130\n"
               "uniform %s t;\n"
               "void main() {\n"
               "   uvec4 v = uvec4(texelFetch(t, ivec2(gl_FragCoord.xy), 0));\n"
               "   gl_FragColor = vec4(v & 255u) / 255.0;\n"
               "}\n",
               s);
   GLuint p = program(fs);
   if (!p) {
      printf("FAIL sample %s: no program\n", f->name);
      failures++;
      return;
   }
   GLuint tex = texture(f->ifmt, f->fmt, f->type, data);
   GLuint dst = texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   GLuint fb = fbo_for(dst);
   GLenum e0 = glGetError_();

   glViewport_(0, 0, W, H);
   glBindTexture_(GL_TEXTURE_2D, tex);
   glUseProgram_(p);
   glUniform1i_(glGetUniformLocation_(p, "t"), 0);
   /* a 32-bit format: one pass a channel (its word's four bytes) */
   unsigned passes = f->bytes == 4 ? f->comps : 1, ok = 0;
   char map[H][W + 1];
   memset(map, 0, sizeof(map));
   for (unsigned ch = 0; ch < passes; ch++) {
      if (f->bytes == 4)
         glUniform1i_(glGetUniformLocation_(p, "c"), ch);
      glClearColor_(0.5f, 0.5f, 0.5f, 0.5f);
      glClear_(GL_COLOR_BUFFER_BIT);
      quad(1.0f);
      glReadPixels_(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
      for (unsigned i = 0; i < W * H; i++) {
         uint8_t want[4];
         if (f->bytes == 4) {
            uint32_t v = pattern(i, ch);
            memcpy(want, &v, 4);
         } else {
            for (unsigned c = 0; c < 4; c++)
               want[c] = c < f->comps ? (uint8_t)pattern(i, c) : c == 3 ? 1 : 0;
         }
         int good = !memcmp(px + 4 * i, want, 4);
         ok += good;
         if (!ch)
            map[i / W][i % W] = good ? '.' : 'x';
      }
   }
   GLenum e = glGetError_();
   printf("%s sample %s: %u/%u texel channels; last texel 1 %02x%02x%02x%02x; glerror %#x %#x\n",
          ok == W * H * passes && !e && !e0 ? "PASS" : "FAIL", f->name, ok,
          W * H * passes, px[4], px[5], px[6], px[7], e0, e);
   failures += !(ok == W * H * passes && !e && !e0);
   if (ok != W * H * passes)
      for (int y = H - 1; y >= 0; y--)
         printf("    y%u %s\n", y, map[y]);
   glBindFramebuffer_(GL_FRAMEBUFFER, 0);
   glDeleteFramebuffers_(1, &fb);
   glDeleteTextures_(1, &tex);
   glDeleteTextures_(1, &dst);
}

struct rfmt {
   const char *name;
   GLenum ifmt, fmt, type;
   unsigned comps;
   int r32;  /* R32UI */
   int wide; /* RG32UI / RGBA32UI: a pass a channel into planes */
};

static void
render_case(const struct rfmt *f)
{
   static const GLuint clear[4] = {0x11, 0x22, 0x33, 0x44};
   static const uint32_t clear32 = 0xc0ffee42u;
   static const GLuint clearw[4] = {0x01020304u, 0xa0b0c0d0u, 5u, 0xffffffffu};
   GLuint out[W * H * 4];
   const char *fs =
      f->wide ? "#version 130\n"
                "out uvec4 o;\n"
                "void main() {\n"
                "   uvec2 q = uvec2(gl_FragCoord.xy);\n"
                "   o = uvec4(0x80000000u | q.x, q.y + 100000u, 7u, 0xdeadbeefu);\n"
                "}\n"
      : f->r32 ? "#version 130\n"
               "out uint o;\n"
               "void main() {\n"
               "   uvec2 q = uvec2(gl_FragCoord.xy);\n"
               "   o = 0x80000000u | q.x << 16u | q.y << 4u | 5u;\n"
               "}\n"
             : "#version 130\n"
               "out uvec4 o;\n"
               "void main() {\n"
               "   uvec2 q = uvec2(gl_FragCoord.xy);\n"
               "   o = uvec4(q.x + 1u, q.y + 100u, 7u, 200u);\n"
               "}\n";
   GLuint p = program(fs);

   if (!p) {
      printf("FAIL render %s: no program\n", f->name);
      failures++;
      return;
   }
   GLuint tex = texture(f->ifmt, f->fmt, f->type, NULL);
   GLuint fb = fbo_for(tex);
   GLenum st = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
   if (st != GL_FRAMEBUFFER_COMPLETE) {
      printf("FAIL render %s: framebuffer %#x\n", f->name, st);
      failures++;
      goto out;
   }
   glViewport_(0, 0, W, H);
   if (f->wide)
      glClearColorIuiEXT_(clearw[0], clearw[1], clearw[2], clearw[3]);
   else if (f->r32)
      glClearColorIuiEXT_(clear32, 0, 0, 0);
   else
      glClearColorIuiEXT_(clear[0], clear[1], clear[2], clear[3]);
   glClear_(GL_COLOR_BUFFER_BIT);
   glUseProgram_(p);
   quad(0.0f); /* the left half */
   memset(out, 0xee, sizeof(out));
   glReadPixels_(0, 0, W, H, GL_RGBA_INTEGER, GL_UNSIGNED_INT, out);
   unsigned ok = 0;
   for (unsigned y = 0; y < H; y++)
      for (unsigned x = 0; x < W; x++) {
         const GLuint *o = out + 4 * (y * W + x);
         GLuint want[4];
         int drawn = x < W / 2;

         if (f->wide) {
            const GLuint d[4] = {0x80000000u | x, y + 100000u, 7u, 0xdeadbeefu};
            for (unsigned c = 0; c < 4; c++)
               want[c] = c < f->comps ? (drawn ? d[c] : clearw[c])
                         : c == 3 ? 1 : 0;
         } else if (f->r32) {
            want[0] = drawn ? (0x80000000u | x << 16 | y << 4 | 5u) : clear32;
            want[1] = want[2] = 0;
            want[3] = 1;
         } else {
            const GLuint d[4] = {x + 1, y + 100, 7, 200};
            for (unsigned c = 0; c < 4; c++)
               want[c] = c < f->comps ? (drawn ? d[c] : clear[c]) & 0xff
                         : c == 3 ? 1 : 0;
         }
         ok += !memcmp(o, want, sizeof(want));
      }
   GLenum e = glGetError_();
   printf("%s render %s: %u/%u pixels; (0,0) %x %x %x %x, (15,7) %x %x %x %x; "
          "glerror %#x\n",
          ok == W * H && !e ? "PASS" : "FAIL", f->name, ok, W * H, out[0],
          out[1], out[2], out[3], out[4 * (W * H - 1)],
          out[4 * (W * H - 1) + 1], out[4 * (W * H - 1) + 2],
          out[4 * (W * H - 1) + 3], e);
   failures += !(ok == W * H && !e);
out:
   glBindFramebuffer_(GL_FRAMEBUFFER, 0);
   glDeleteFramebuffers_(1, &fb);
   glDeleteTextures_(1, &tex);
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
   GET(glViewport);
   GET(glGetString);
   GET(glGetError);
   GET(glReadPixels);
   GET(glPixelStorei);
   GET(glGenTextures);
   GET(glBindTexture);
   GET(glTexImage2D);
   GET(glTexParameteri);
   GET(glDeleteTextures);
   GET(glGenFramebuffers);
   GET(glBindFramebuffer);
   GET(glFramebufferTexture2D);
   GET(glCheckFramebufferStatus);
   GET(glDeleteFramebuffers);
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
   GET(glGetProgramInfoLog);
   GET(glUseProgram);
   GET(glGetUniformLocation);
   GET(glUniform1i);
   GET(glVertexAttribPointer);
   GET(glEnableVertexAttribArray);
   GET(glDrawArrays);

   const char *ext = (const char *)glGetString_(GL_EXTENSIONS);
   printf("renderer: %s, GLSL %s, EXT_texture_integer %s\n",
          glGetString_(GL_RENDERER),
          glGetString_(GL_SHADING_LANGUAGE_VERSION),
          ext && strstr(ext, "GL_EXT_texture_integer") ? "yes" : "no");
   if (!ext || !strstr(ext, "GL_EXT_texture_integer")) {
      printf("FAIL: no EXT_texture_integer (SGX_DEBUG=glsl130?)\n");
      return 1;
   }
   GET(glClearColorIuiEXT);

   static const struct sfmt sf[] = {
      {"RGBA8UI", GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, 4, 1, 0},
      {"RGBA8I", GL_RGBA8I, GL_RGBA_INTEGER, GL_BYTE, 4, 1, 1},
      {"RG8UI", GL_RG8UI, GL_RG_INTEGER, GL_UNSIGNED_BYTE, 2, 1, 0},
      {"R8UI", GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, 1, 1, 0},
      {"R32UI", GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, 1, 4, 0},
      {"R32I", GL_R32I, GL_RED_INTEGER, GL_INT, 1, 4, 1},
      /* word by word (the compiler's tex_r32x2 / x4) */
      {"RG32UI", GL_RG32UI, GL_RG_INTEGER, GL_UNSIGNED_INT, 2, 4, 0},
      {"RGBA32UI", GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, 4, 4, 0},
      {"RGBA32I", GL_RGBA32I, GL_RGBA_INTEGER, GL_INT, 4, 4, 1},
   };
   static const struct rfmt rf[] = {
      {"RGBA8UI", GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, 4, 0, 0},
      {"RG8UI", GL_RG8UI, GL_RG_INTEGER, GL_UNSIGNED_BYTE, 2, 0, 0},
      {"R8UI", GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, 1, 0, 0},
      {"R32UI", GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, 1, 1, 0},
      {"RG32UI", GL_RG32UI, GL_RG_INTEGER, GL_UNSIGNED_INT, 2, 0, 1},
      {"RGBA32UI", GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, 4, 0, 1},
   };
   for (unsigned i = 0; i < sizeof(sf) / sizeof(sf[0]); i++)
      sample_case(&sf[i]);
   for (unsigned i = 0; i < sizeof(rf) / sizeof(rf[0]); i++)
      render_case(&rf[i]);
   return failures ? 1 : 0;
}
