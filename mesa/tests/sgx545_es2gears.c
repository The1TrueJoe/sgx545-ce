/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

/*
 * sgx545_es2gears: es2gears on the EGL fbdev window: three turning gears,
 * transformed and lit per vertex on the GPU by es2gears' own shaders
 * (ModelViewProjectionMatrix, NormalMatrix, LightSourcePosition,
 * MaterialColor; a diffuse Color varying), compiled by the sgx545 compiler.
 * The gear geometry is the same as sgx545_cpugears'.
 *
 *   sgx545_es2gears [-n frames] [-o frame.rgba -f N] [-p W H] [-r]
 *     -o: glReadPixels of frame N; -p: draw into a W x H pbuffer instead of
 *     the window; -r: glReadPixels every frame (as gl4es' pbuffer mode does);
 *     -2: first render one frame into a 64x64 pbuffer (a second render target)
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

static const char vertex_shader[] =
   "attribute vec3 position;\n"
   "attribute vec3 normal;\n"
   "uniform mat4 ModelViewProjectionMatrix;\n"
   "uniform mat4 NormalMatrix;\n"
   "uniform vec4 LightSourcePosition;\n"
   "uniform vec4 MaterialColor;\n"
   "varying vec4 Color;\n"
   "void main(void)\n"
   "{\n"
   "    vec3 N = normalize(vec3(NormalMatrix * vec4(normal, 1.0)));\n"
   "    vec3 L = normalize(LightSourcePosition.xyz);\n"
   "    float diffuse = max(dot(N, L), 0.0);\n"
   "    Color = diffuse * MaterialColor;\n"
   "    gl_Position = ModelViewProjectionMatrix * vec4(position, 1.0);\n"
   "}";

static const char fragment_shader[] =
   "precision mediump float;\n"
   "varying vec4 Color;\n"
   "void main(void)\n"
   "{\n"
   "    gl_FragColor = Color;\n"
   "}";

static GLuint
shader(GLenum type, const char *src)
{
   GLuint s = glCreateShader(type);
   GLint ok = 0;

   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[1024];

      glGetShaderInfoLog(s, sizeof(log), NULL, log);
      printf("shader: %s\n", log);
   }
   return s;
}

/* the inverse transpose of the modelview's upper 3x3 is the matrix itself
 * here (rotations and translations), as es2gears uses it */
static void
normal_matrix(float *n, const float *mv)
{
   memcpy(n, mv, 16 * sizeof(float));
   n[12] = n[13] = n[14] = 0;
}

int
main(int argc, char **argv)
{
   static const EGLint cattr[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                   EGL_DEPTH_SIZE, 16, EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                                   EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
   static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   int frames = 600, dump_frame = -1, opt, pb_w = 0, pb_h = 0, readback = 0, second = 0;
   unsigned char *rb = NULL;
   const char *out = NULL;
   EGLDisplay dpy;
   EGLConfig cfg;
   EGLSurface surf;
   EGLContext ctx;
   EGLint n, w, h;
   struct gear g[3] = { { .color = { 0.8f, 0.1f, 0.0f, 1 } }, { .color = { 0.0f, 0.8f, 0.2f, 1 } },
                        { .color = { 0.2f, 0.2f, 1.0f, 1 } } };
   const float place[3][3] = { { -3, -2, 0 }, { 3.1f, -2, 0 }, { -3.1f, 4.2f, 0 } };
   float *vbo[3];
   GLuint prog;
   GLint u_mvp, u_nm, u_light, u_mat;
   struct timespec t0, t1;

   while ((opt = getopt(argc, argv, "n:o:f:p:r2")) != -1) {
      if (opt == '2')
         second = 1;
      else if (opt == 'p' && optind < argc) {
         pb_w = atoi(optarg);
         pb_h = atoi(argv[optind++]);
      } else if (opt == 'r')
         readback = 1;
      else if (opt == 'n')
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
   if (pb_w) {
      const EGLint pb[] = { EGL_WIDTH, pb_w, EGL_HEIGHT, pb_h, EGL_NONE };

      surf = eglCreatePbufferSurface(dpy, cfg, pb);
   } else {
      surf = eglCreateWindowSurface(dpy, cfg, 0, NULL);
   }
   ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
   if (second) {
      const EGLint pb[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
      EGLSurface s2 = eglCreatePbufferSurface(dpy, cfg, pb);
      unsigned char px[64 * 64 * 4];

      eglMakeCurrent(dpy, s2, s2, ctx);
      glClearColor(1, 0, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, px);
      printf("es2gears: 64x64 pbuffer first, pixel %u %u %u %u\n", px[0], px[1], px[2], px[3]);
   }
   if (!surf || !ctx || !eglMakeCurrent(dpy, surf, surf, ctx))
      return printf("surface/context failed %#x\n", eglGetError()), 1;
   eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
   eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
   printf("es2gears: %dx%d %s, %s\n", w, h, pb_w ? "pbuffer" : "window", glGetString(GL_RENDERER));
   rb = malloc((size_t)w * h * 4);

   make_gear(&g[0], 1.0f, 4.0f, 1.0f, 20, 0.7f);
   make_gear(&g[1], 0.5f, 2.0f, 2.0f, 10, 0.7f);
   make_gear(&g[2], 1.3f, 2.0f, 0.5f, 10, 0.7f);
   /* interleaved position + normal per vertex */
   for (int k = 0; k < 3; k++) {
      vbo[k] = malloc((size_t)g[k].n * 3 * 6 * sizeof(float));
      for (int i = 0; i < g[k].n; i++)
         for (int v = 0; v < 3; v++) {
            memcpy(vbo[k] + (i * 3 + v) * 6, g[k].t[i].v[v], 12);
            memcpy(vbo[k] + (i * 3 + v) * 6 + 3, g[k].t[i].n, 12);
         }
   }

   prog = glCreateProgram();
   glAttachShader(prog, shader(GL_VERTEX_SHADER, vertex_shader));
   glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fragment_shader));
   glBindAttribLocation(prog, 0, "position");
   glBindAttribLocation(prog, 1, "normal");
   glLinkProgram(prog);
   glUseProgram(prog);
   u_mvp = glGetUniformLocation(prog, "ModelViewProjectionMatrix");
   u_nm = glGetUniformLocation(prog, "NormalMatrix");
   u_light = glGetUniformLocation(prog, "LightSourcePosition");
   u_mat = glGetUniformLocation(prog, "MaterialColor");
   glUniform4f(u_light, 5.0f, 5.0f, 10.0f, 1.0f);
   glEnable(GL_DEPTH_TEST);
   glEnable(GL_CULL_FACE);
   glViewport(0, 0, w, h);
   glEnableVertexAttribArray(0);
   glEnableVertexAttribArray(1);

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
         float mv[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }, mvp[16], nm[16];

         trans(mv, 0, 0, -40);
         rot(mv, 20, 1, 0, 0);
         rot(mv, 30, 0, 1, 0);
         trans(mv, place[k][0], place[k][1], place[k][2]);
         rot(mv, k == 0 ? angle : k == 1 ? -2 * angle - 9 : -2 * angle - 25, 0, 0, 1);
         mul(mvp, proj, mv);
         normal_matrix(nm, mv);
         glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp);
         glUniformMatrix4fv(u_nm, 1, GL_FALSE, nm);
         glUniform4fv(u_mat, 1, g[k].color);
         glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, vbo[k]);
         glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, vbo[k] + 3);
         glDrawArrays(GL_TRIANGLES, 0, g[k].n * 3);
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
      if (readback)
         glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rb);
      eglSwapBuffers(dpy, surf);
   }
   clock_gettime(CLOCK_MONOTONIC, &t1);
   {
      double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

      printf("es2gears: %d frames in %.2f s = %.1f FPS (%d triangles/frame), glerr %#x\n", frames, s,
             frames / s, g[0].n + g[1].n + g[2].n, glGetError());
   }
   eglTerminate(dpy);
   return 0;
}
