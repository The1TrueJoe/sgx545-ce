/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 *
 * A render that doesn't start with a clear must keep what the previous one
 * drew (the background object loads the target's old contents): clear red,
 * a green triangle on the left, glReadPixels (which flushes), then a blue one
 * on the right without a clear, and glReadPixels again. Minecraft's title
 * screen works that way (a flush per glCopyTexSubImage2D).
 *
 *   sgx545_reload [-p W H]     (-p: a pbuffer instead of the window)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

static GLuint
shader(GLenum type, const char *src)
{
   GLuint s = glCreateShader(type);

   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   return s;
}

static void
px(int x, int y, const char *what)
{
   unsigned char p[4];

   glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
   printf("reload: %-28s (%3d,%3d) = %3u %3u %3u %3u\n", what, x, y, p[0], p[1], p[2], p[3]);
}

int
main(int argc, char **argv)
{
   static const EGLint cattr[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                   EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                                   EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
   static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   static const float left[] = { -0.9f, -0.5f, -0.1f, -0.5f, -0.5f, 0.5f };
   static const float right[] = { 0.1f, -0.5f, 0.9f, -0.5f, 0.5f, 0.5f };
   EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   EGLConfig cfg;
   EGLSurface surf;
   EGLContext ctx;
   EGLint n, w, h;
   GLuint prog;
   GLint uc;

   setvbuf(stdout, NULL, _IONBF, 0);
   if (!eglInitialize(dpy, NULL, NULL) || !eglChooseConfig(dpy, cattr, &cfg, 1, &n) || !n)
      return printf("EGL init failed\n"), 1;
   if (argc > 3 && !strcmp(argv[1], "-p")) {
      const EGLint pb[] = { EGL_WIDTH, atoi(argv[2]), EGL_HEIGHT, atoi(argv[3]), EGL_NONE };

      surf = eglCreatePbufferSurface(dpy, cfg, pb);
   } else {
      surf = eglCreateWindowSurface(dpy, cfg, 0, NULL);
   }
   ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
   if (!surf || !ctx || !eglMakeCurrent(dpy, surf, surf, ctx))
      return printf("surface/context failed %#x\n", eglGetError()), 1;
   eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
   eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
   prog = glCreateProgram();
   glAttachShader(prog, shader(GL_VERTEX_SHADER, "attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }"));
   glAttachShader(prog, shader(GL_FRAGMENT_SHADER,
                               "precision mediump float; uniform vec4 c; void main() { gl_FragColor = c; }"));
   glBindAttribLocation(prog, 0, "p");
   glLinkProgram(prog);
   glUseProgram(prog);
   uc = glGetUniformLocation(prog, "c");
   glViewport(0, 0, w, h);
   glEnableVertexAttribArray(0);

   glClearColor(1, 0, 0, 1);
   glClear(GL_COLOR_BUFFER_BIT);
   glUniform4f(uc, 0, 1, 0, 1);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, left);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   px(w / 4, h / 2, "left, first render");
   glUniform4f(uc, 0, 0, 1, 1);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, right);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   px(w / 4, h / 2, "left, second render");
   px(3 * w / 4, h / 2, "right, second render");
   px(w / 2, h / 8, "clear, second render");
   eglTerminate(dpy);
   return 0;
}
