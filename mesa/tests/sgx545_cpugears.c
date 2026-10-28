/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

/*
 * sgx545_cpugears: three turning gears on the EGL fbdev window, drawn through
 * GLES2 with the two shaders the driver can run before its compiler is wired
 * in (gl_Position = attribute; gl_FragColor = uniform).  So the transform
 * and the lighting are done here on the CPU: every frame each gear's
 * triangles are projected, and sorted into eight shades of its colour by
 * their facing, one glDrawArrays per shade.  Depth testing does the rest.
 *
 *   sgx545_cpugears [-n frames] [-o frame.rgba -f N]   (-o: glReadPixels of frame N)
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#define SHADES 8

struct tri { float v[3][4]; float n[3]; };
struct gear { struct tri *t; int n, cap; float color[4]; };

static void
add(struct gear *g, const float a[3], const float b[3], const float c[3], const float n[3])
{
   if (g->n == g->cap) {
      g->cap = g->cap ? 2 * g->cap : 256;
      g->t = realloc(g->t, g->cap * sizeof(*g->t));
   }
   struct tri *t = &g->t[g->n++];
   const float *p[3] = { a, b, c };
   for (int i = 0; i < 3; i++) {
      memcpy(t->v[i], p[i], 12);
      t->v[i][3] = 1;
   }
   memcpy(t->n, n, 12);
}

static void
quad(struct gear *g, const float a[3], const float b[3], const float c[3], const float d[3],
     const float n[3])
{
   add(g, a, b, c, n);
   add(g, a, c, d, n);
}

/* a gear: inner radius r0, outer r1, tooth depth td, width w, teeth n */
static void
make_gear(struct gear *g, float r0, float r1, float w, int teeth, float td)
{
   float rr0 = r0, rr1 = r1 - td / 2, rr2 = r1 + td / 2, da = 2 * M_PI / teeth / 4;

   for (int i = 0; i < teeth; i++) {
      float a = i * 2 * M_PI / teeth, z = w / 2;
      float c[5], s[5];

      for (int k = 0; k < 5; k++) {
         c[k] = cosf(a + k * da);
         s[k] = sinf(a + k * da);
      }
      for (int side = 0; side < 2; side++) {
         float zz = side ? -z : z, nz[3] = { 0, 0, side ? -1.0f : 1.0f };
         float p0[3] = { rr0 * c[0], rr0 * s[0], zz }, p1[3] = { rr1 * c[0], rr1 * s[0], zz };
         float p2[3] = { rr0 * c[4], rr0 * s[4], zz }, p3[3] = { rr1 * c[4], rr1 * s[4], zz };
         float t0[3] = { rr1 * c[0], rr1 * s[0], zz }, t1[3] = { rr2 * c[1], rr2 * s[1], zz };
         float t2[3] = { rr2 * c[2], rr2 * s[2], zz }, t3[3] = { rr1 * c[3], rr1 * s[3], zz };

         /* the face between the hole and the root, and the tooth */
         if (side) {
            quad(g, p0, p2, p3, p1, nz);
            quad(g, t0, t3, t2, t1, nz);
         } else {
            quad(g, p0, p1, p3, p2, nz);
            quad(g, t0, t1, t2, t3, nz);
         }
      }
      /* the outward faces: tooth sides, top, the gap to the next tooth */
      float ring[5][2] = { { rr1 * c[0], rr1 * s[0] }, { rr2 * c[1], rr2 * s[1] },
                           { rr2 * c[2], rr2 * s[2] }, { rr1 * c[3], rr1 * s[3] },
                           { rr1 * cosf(a + 4 * da), rr1 * sinf(a + 4 * da) } };
      for (int k = 0; k < 4; k++) {
         float u[3] = { ring[k][0], ring[k][1], z }, v[3] = { ring[k + 1][0], ring[k + 1][1], z };
         float u2[3] = { ring[k][0], ring[k][1], -z }, v2[3] = { ring[k + 1][0], ring[k + 1][1], -z };
         float dx = ring[k + 1][0] - ring[k][0], dy = ring[k + 1][1] - ring[k][1];
         float l = sqrtf(dx * dx + dy * dy), n[3] = { dy / l, -dx / l, 0 };

         quad(g, u, u2, v2, v, n);
      }
      /* the hole */
      {
         float u[3] = { rr0 * c[0], rr0 * s[0], z }, v[3] = { rr0 * c[4], rr0 * s[4], z };
         float u2[3] = { rr0 * c[0], rr0 * s[0], -z }, v2[3] = { rr0 * c[4], rr0 * s[4], -z };
         float n[3] = { -cosf(a + 2 * da), -sinf(a + 2 * da), 0 };

         quad(g, u, v, v2, u2, n);
      }
   }
}

/* column-major 4x4 */
static void
mul(float *r, const float *a, const float *b)
{
   float t[16];

   for (int i = 0; i < 4; i++)
      for (int j = 0; j < 4; j++) {
         t[j * 4 + i] = 0;
         for (int k = 0; k < 4; k++)
            t[j * 4 + i] += a[k * 4 + i] * b[j * 4 + k];
      }
   memcpy(r, t, sizeof(t));
}

static void
rot(float *m, float deg, float x, float y, float z)
{
   float a = deg * M_PI / 180, c = cosf(a), s = sinf(a), r[16] = {
      x * x * (1 - c) + c, y * x * (1 - c) + z * s, x * z * (1 - c) - y * s, 0,
      x * y * (1 - c) - z * s, y * y * (1 - c) + c, y * z * (1 - c) + x * s, 0,
      x * z * (1 - c) + y * s, y * z * (1 - c) - x * s, z * z * (1 - c) + c, 0,
      0, 0, 0, 1 };

   mul(m, m, r);
}

static void
trans(float *m, float x, float y, float z)
{
   float t[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1 };

   mul(m, m, t);
}

static GLuint
shader(GLenum type, const char *src)
{
   GLuint s = glCreateShader(type);

   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   return s;
}

int
main(int argc, char **argv)
{
   static const EGLint cattr[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                   EGL_DEPTH_SIZE, 16, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                   EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
   static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   int frames = 600, dump_frame = -1, opt;
   const char *out = NULL;
   EGLDisplay dpy;
   EGLConfig cfg;
   EGLSurface surf;
   EGLContext ctx;
   EGLint n, w, h;
   struct gear g[3] = { { .color = { 0.8f, 0.1f, 0.0f, 1 } }, { .color = { 0.0f, 0.8f, 0.2f, 1 } },
                        { .color = { 0.2f, 0.2f, 1.0f, 1 } } };
   const float place[3][3] = { { -3, -2, 0 }, { 3.1f, -2, 0 }, { -3.1f, 4.2f, 0 } };
   GLuint prog;
   GLint loc;
   float *buf;
   struct timespec t0, t1;

   while ((opt = getopt(argc, argv, "n:o:f:")) != -1) {
      if (opt == 'n')
         frames = atoi(optarg);
      else if (opt == 'o')
         out = optarg;
      else if (opt == 'f')
         dump_frame = atoi(optarg);
   }
   setvbuf(stdout, NULL, _IONBF, 0);
   dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   if (!eglInitialize(dpy, NULL, NULL) || !eglChooseConfig(dpy, cattr, &cfg, 1, &n) || !n)
      return printf("EGL init failed\n"), 1;
   surf = eglCreateWindowSurface(dpy, cfg, 0, NULL);
   ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
   if (!surf || !ctx || !eglMakeCurrent(dpy, surf, surf, ctx))
      return printf("surface/context failed %#x\n", eglGetError()), 1;
   eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
   eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
   printf("cpugears: %dx%d window, %s\n", w, h, glGetString(GL_RENDERER));

   make_gear(&g[0], 1.0f, 4.0f, 1.0f, 20, 0.7f);
   make_gear(&g[1], 0.5f, 2.0f, 2.0f, 10, 0.7f);
   make_gear(&g[2], 1.3f, 2.0f, 0.5f, 10, 0.7f);
   /* SHADES buckets, each big enough for a whole gear */
   buf = malloc((size_t)SHADES * g[0].n * 9 * sizeof(float));

   prog = glCreateProgram();
   glAttachShader(prog, shader(GL_VERTEX_SHADER, "attribute vec4 p; void main() { gl_Position = p; }"));
   glAttachShader(prog, shader(GL_FRAGMENT_SHADER,
                               "precision mediump float; uniform vec4 c; void main() { gl_FragColor = c; }"));
   glBindAttribLocation(prog, 0, "p");
   glLinkProgram(prog);
   glUseProgram(prog);
   loc = glGetUniformLocation(prog, "c");
   glEnable(GL_DEPTH_TEST);
   glViewport(0, 0, w, h);
   glEnableVertexAttribArray(0);

   clock_gettime(CLOCK_MONOTONIC, &t0);
   for (int f = 0; f < frames; f++) {
      float angle = f * 2.0f, aspect = (float)h / w;
      /* glFrustum(-1, 1, -aspect, aspect, 5, 60) */
      float near = 5, far = 60, proj[16] = { near, 0, 0, 0, 0, near / aspect, 0, 0, 0, 0,
                                             -(far + near) / (far - near), -1, 0, 0,
                                             -2 * far * near / (far - near), 0 };

      glClearColor(0, 0, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      for (int k = 0; k < 3; k++) {
         float mv[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }, mvp[16];
         int count[SHADES] = { 0 };
         float *bucket[SHADES];
         const float light[3] = { 0.408f, 0.408f, 0.816f };

         trans(mv, 0, 0, -40);
         rot(mv, 20, 1, 0, 0);
         rot(mv, 30, 0, 1, 0);
         trans(mv, place[k][0], place[k][1], place[k][2]);
         rot(mv, k == 0 ? angle : k == 1 ? -2 * angle - 9 : -2 * angle - 25, 0, 0, 1);
         mul(mvp, proj, mv);
         for (int s = 0; s < SHADES; s++)
            bucket[s] = buf + (size_t)s * g[0].n * 9;
         for (int i = 0; i < g[k].n; i++) {
            const struct tri *t = &g[k].t[i];
            /* the normal by the modelview's rotation (orthonormal) */
            float nx = mv[0] * t->n[0] + mv[4] * t->n[1] + mv[8] * t->n[2];
            float ny = mv[1] * t->n[0] + mv[5] * t->n[1] + mv[9] * t->n[2];
            float nz = mv[2] * t->n[0] + mv[6] * t->n[1] + mv[10] * t->n[2];
            float d = nx * light[0] + ny * light[1] + nz * light[2];
            int s = d <= 0 ? 0 : d >= 1 ? SHADES - 1 : (int)(d * SHADES);

            for (int v = 0; v < 3; v++) {
               float c[4];

               for (int r = 0; r < 4; r++)
                  c[r] = mvp[r] * t->v[v][0] + mvp[4 + r] * t->v[v][1] + mvp[8 + r] * t->v[v][2] +
                         mvp[12 + r];
               bucket[s][count[s] * 9 + v * 3 + 0] = c[0] / c[3];
               bucket[s][count[s] * 9 + v * 3 + 1] = c[1] / c[3];
               bucket[s][count[s] * 9 + v * 3 + 2] = c[2] / c[3];
            }
            count[s]++;
         }
         for (int s = 0; s < SHADES; s++) {
            float shade = 0.2f + 0.8f * (s + 0.5f) / SHADES;

            if (!count[s])
               continue;
            glUniform4f(loc, g[k].color[0] * shade, g[k].color[1] * shade, g[k].color[2] * shade, 1);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, bucket[s]);
            glDrawArrays(GL_TRIANGLES, 0, count[s] * 3);
         }
      }
      if (f == dump_frame && out) {
         unsigned char *img = malloc((size_t)w * h * 4);
         FILE *fp = fopen(out, "wb");

         glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, img);
         if (fp) {
            fwrite(img, 4, (size_t)w * h, fp);
            fclose(fp);
         }
         free(img);
      }
      eglSwapBuffers(dpy, surf);
   }
   clock_gettime(CLOCK_MONOTONIC, &t1);
   {
      double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

      printf("cpugears: %d frames in %.2f s = %.1f FPS (%d triangles/frame), glerr %#x\n", frames, s,
             frames / s, g[0].n + g[1].n + g[2].n, glGetError());
   }
   eglTerminate(dpy);
   return 0;
}
