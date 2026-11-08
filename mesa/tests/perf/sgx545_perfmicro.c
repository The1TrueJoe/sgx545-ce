/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * sgx545_perfmicro: GPU-bound microbenchmarks on an EGL pbuffer, the same
 * GLES2 program for any driver (ours through EGL surfaceless, the DDK through
 * its own default display), so the GPU's limits can be told from the CPU's.
 *
 *   sgx545_perfmicro [-s SECONDS] [-w W] [-h H]
 *
 *   fill        N full-target quads per frame, opaque (depth test off)
 *   fill-blend  the same with SRC_ALPHA, ONE_MINUS_SRC_ALPHA blending
 *   tex         the same, each sampling a 256x256 RGBA8 texture
 *   verts       one indexed mesh of small triangles in a VBO, drawn per frame
 *
 * Each test runs SECONDS (default 5) with a glFinish per frame and prints the
 * frame rate and what it amounts to (megapixels or megatriangles a second).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

static double
now(void)
{
   struct timespec ts;

   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec / 1e9;
}

static GLuint
program(const char *vs, const char *fs)
{
   GLuint p = glCreateProgram();
   const char *src[2] = { vs, fs };
   GLenum type[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };

   for (int i = 0; i < 2; i++) {
      GLuint s = glCreateShader(type[i]);
      GLint ok;

      glShaderSource(s, 1, &src[i], NULL);
      glCompileShader(s);
      glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
      if (!ok) {
         char log[512];

         glGetShaderInfoLog(s, sizeof(log), NULL, log);
         fprintf(stderr, "shader: %s\n", log);
         exit(1);
      }
      glAttachShader(p, s);
   }
   glBindAttribLocation(p, 0, "pos");
   glLinkProgram(p);
   return p;
}

/* frames a second of draw() over secs, a glFinish per frame */
static double
run(double secs, void (*draw)(void))
{
   unsigned frames = 0;
   double t0, t;

   draw();
   glFinish();
   t0 = now();
   do {
      draw();
      glFinish();
      frames++;
      t = now();
   } while (t - t0 < secs);
   return frames / (t - t0);
}

#define QUADS 8

static GLuint p_flat, p_tex, quad_vbo, mesh_vbo, mesh_ibo, tex;
static GLint u_color;
static unsigned mesh_indices;

static void
draw_quads(void)
{
   glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
   for (int i = 0; i < QUADS; i++) {
      glUniform4f(u_color, i / (float)QUADS, 0.5f, 0.25f, 0.5f);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   }
}

static void
draw_fill(void)
{
   glUseProgram(p_flat);
   glDisable(GL_BLEND);
   draw_quads();
}

static void
draw_blend(void)
{
   glUseProgram(p_flat);
   glEnable(GL_BLEND);
   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
   draw_quads();
}

static void
draw_tex(void)
{
   glUseProgram(p_tex);
   glDisable(GL_BLEND);
   glBindTexture(GL_TEXTURE_2D, tex);
   glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
   for (int i = 0; i < QUADS; i++)
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void
draw_verts(void)
{
   glUseProgram(p_flat);
   glDisable(GL_BLEND);
   glUniform4f(u_color, 0.2f, 0.8f, 0.4f, 1.0f);
   glBindBuffer(GL_ARRAY_BUFFER, mesh_vbo);
   glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh_ibo);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
   glDrawElements(GL_TRIANGLES, mesh_indices, GL_UNSIGNED_SHORT, 0);
}

int
main(int argc, char **argv)
{
   double secs = 5;
   int w = 720, h = 480, opt;
   EGLDisplay dpy;
   EGLConfig cfg;
   EGLint n;
   EGLSurface surf;
   EGLContext ctx;

   while ((opt = getopt(argc, argv, "s:w:h:")) != -1) {
      switch (opt) {
      case 's': secs = atof(optarg); break;
      case 'w': w = atoi(optarg); break;
      case 'h': h = atoi(optarg); break;
      default: return 2;
      }
   }
   setvbuf(stdout, NULL, _IONBF, 0);
   dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   if (!eglInitialize(dpy, NULL, NULL)) {
      fprintf(stderr, "eglInitialize: %#x\n", eglGetError());
      return 1;
   }
   {
      static const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                     EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                     EGL_NONE };
      static const EGLint cattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
      EGLint pattr[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };

      if (!eglChooseConfig(dpy, attr, &cfg, 1, &n) || !n) {
         fprintf(stderr, "no RGBA8 pbuffer config\n");
         return 1;
      }
      surf = eglCreatePbufferSurface(dpy, cfg, pattr);
      ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cattr);
   }
   if (!surf || !ctx || !eglMakeCurrent(dpy, surf, surf, ctx)) {
      fprintf(stderr, "EGL: %#x\n", eglGetError());
      return 1;
   }
   printf("renderer: %s (%s)\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
   glViewport(0, 0, w, h);

   p_flat = program("attribute vec2 pos;\nvoid main() { gl_Position = vec4(pos, 0.0, 1.0); }\n",
                    "precision mediump float;\nuniform vec4 color;\nvoid main() { gl_FragColor = color; }\n");
   u_color = glGetUniformLocation(p_flat, "color");
   p_tex = program("attribute vec2 pos;\nvarying vec2 uv;\n"
                   "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n",
                   "precision mediump float;\nuniform sampler2D t;\nvarying vec2 uv;\n"
                   "void main() { gl_FragColor = texture2D(t, uv); }\n");
   glEnableVertexAttribArray(0);
   {
      static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };

      glGenBuffers(1, &quad_vbo);
      glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
      glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
   }
   {
      uint32_t *texels = malloc(256 * 256 * 4);

      for (int i = 0; i < 256 * 256; i++)
         texels[i] = ((i >> 4) ^ (i >> 12)) & 1 ? 0xff40c0ffu : 0xff202020u;
      glGenTextures(1, &tex);
      glBindTexture(GL_TEXTURE_2D, tex);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      free(texels);
   }
   {
      /* a 128x128 grid over the target: 32,258 triangles of a few pixels each */
      enum { G = 128 };
      GLfloat *v = malloc(G * G * 2 * sizeof(*v));
      GLushort *ix = malloc((G - 1) * (G - 1) * 6 * sizeof(*ix));
      unsigned k = 0;

      for (int y = 0; y < G; y++)
         for (int x = 0; x < G; x++) {
            v[2 * (y * G + x)] = -0.9f + 1.8f * x / (G - 1);
            v[2 * (y * G + x) + 1] = -0.9f + 1.8f * y / (G - 1);
         }
      for (int y = 0; y < G - 1; y++)
         for (int x = 0; x < G - 1; x++) {
            GLushort a = y * G + x, b = a + 1, c = a + G, d = c + 1;

            ix[k++] = a, ix[k++] = b, ix[k++] = c, ix[k++] = b, ix[k++] = d, ix[k++] = c;
         }
      mesh_indices = k;
      glGenBuffers(1, &mesh_vbo);
      glBindBuffer(GL_ARRAY_BUFFER, mesh_vbo);
      glBufferData(GL_ARRAY_BUFFER, G * G * 2 * sizeof(*v), v, GL_STATIC_DRAW);
      glGenBuffers(1, &mesh_ibo);
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh_ibo);
      glBufferData(GL_ELEMENT_ARRAY_BUFFER, k * sizeof(*ix), ix, GL_STATIC_DRAW);
      free(v);
      free(ix);
   }

   {
      double px = (double)w * h * QUADS / 1e6, fps;

      fps = run(secs, draw_fill);
      printf("fill: %.1f fps, %.0f Mpix/s\n", fps, fps * px);
      fps = run(secs, draw_blend);
      printf("fill-blend: %.1f fps, %.0f Mpix/s\n", fps, fps * px);
      fps = run(secs, draw_tex);
      printf("tex: %.1f fps, %.0f Mtexel-pix/s\n", fps, fps * px);
      fps = run(secs, draw_verts);
      printf("verts: %.1f fps, %.2f Mtri/s\n", fps, fps * mesh_indices / 3 / 1e6);
   }
   if (glGetError() != GL_NO_ERROR)
      printf("GL error at the end\n");
   return 0;
}
