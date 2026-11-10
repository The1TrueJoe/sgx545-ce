/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 *
 * GLX_EXT_texture_from_pixmap, as a GLX compositor uses it: the configs that
 * bind a pixmap of depth 24 (RGB) and 32 (RGBA), then a pixmap filled through
 * X (left red, right blue), bound, drawn to a window and read back; then
 * refilled (green, white) and bound again, as a compositor does every frame.
 *
 *   glxtfp [-v] [-r] [-s WxH]   exit 0 when every depth the server has passes
 *     -r     GL_TEXTURE_RECTANGLE (unnormalised coordinates), not 2D
 *     -s     the pixmap's size (64x64 by default; a compositor's is the screen's)
 * First, the swap itself: the window cleared to a colour, swapped, and read
 * back through X (XGetImage), to see that a GL swap reaches X at all.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#ifndef GLX_BIND_TO_TEXTURE_RGB_EXT
#define GLX_BIND_TO_TEXTURE_RGB_EXT     0x20D0
#define GLX_BIND_TO_TEXTURE_RGBA_EXT    0x20D1
#define GLX_BIND_TO_TEXTURE_TARGETS_EXT 0x20D3
#define GLX_Y_INVERTED_EXT              0x20D4
#define GLX_TEXTURE_FORMAT_EXT          0x20D5
#define GLX_TEXTURE_TARGET_EXT          0x20D6
#define GLX_TEXTURE_FORMAT_RGB_EXT      0x20D9
#define GLX_TEXTURE_FORMAT_RGBA_EXT     0x20DA
#define GLX_TEXTURE_2D_BIT_EXT          0x00000002
#define GLX_TEXTURE_2D_EXT              0x20DC
#define GLX_FRONT_LEFT_EXT              0x20DE
#endif
#ifndef GLX_TEXTURE_RECTANGLE_EXT
#define GLX_TEXTURE_RECTANGLE_BIT_EXT 0x00000004
#define GLX_TEXTURE_RECTANGLE_EXT     0x20DD
#endif
#ifndef GL_TEXTURE_RECTANGLE_ARB
#define GL_TEXTURE_RECTANGLE_ARB 0x84F5
#endif

typedef void (*bind_fn)(Display *, GLXDrawable, int, const int *);
typedef void (*release_fn)(Display *, GLXDrawable, int);

static int verbose, rect, pw = 64, ph = 64;
static int xerrors;

static int
on_xerror(Display *dpy, XErrorEvent *e)
{
   char s[128];

   XGetErrorText(dpy, e->error_code, s, sizeof(s));
   fprintf(stderr, "  X error: %s (request %d.%d)\n", s, e->request_code,
           e->minor_code);
   xerrors++;
   return 0;
}

/* a config that binds a pixmap of this depth, or NULL */
static GLXFBConfig
tfp_config(Display *dpy, int screen, int depth, int *y_inverted)
{
   int n, found = -1;
   GLXFBConfig *c = glXGetFBConfigs(dpy, screen, &n);
   int rgb = 0, rgba = 0;

   for (int i = 0; i < n; i++) {
      int v_rgb = 0, v_rgba = 0, dt = 0, targets = 0, vid = 0;
      XVisualInfo *vi = glXGetVisualFromFBConfig(dpy, c[i]);

      glXGetFBConfigAttrib(dpy, c[i], GLX_BIND_TO_TEXTURE_RGB_EXT, &v_rgb);
      glXGetFBConfigAttrib(dpy, c[i], GLX_BIND_TO_TEXTURE_RGBA_EXT, &v_rgba);
      glXGetFBConfigAttrib(dpy, c[i], GLX_DRAWABLE_TYPE, &dt);
      glXGetFBConfigAttrib(dpy, c[i], GLX_BIND_TO_TEXTURE_TARGETS_EXT, &targets);
      glXGetFBConfigAttrib(dpy, c[i], GLX_VISUAL_ID, &vid);
      rgb += v_rgb != 0;
      rgba += v_rgba != 0;
      if (verbose)
         printf("  config %d: visual %#x depth %d, drawable %#x, bind rgb %d "
                "rgba %d, targets %#x\n",
                i, vid, vi ? vi->depth : 0, dt, v_rgb, v_rgba, targets);
      if (found < 0 && vi && vi->depth == depth && (dt & GLX_PIXMAP_BIT) &&
          (targets & (rect ? GLX_TEXTURE_RECTANGLE_BIT_EXT
                         : GLX_TEXTURE_2D_BIT_EXT)) &&
          (depth == 32 ? v_rgba : v_rgb))
         found = i;
      XFree(vi);
   }
   printf("depth %d: %d configs, %d bind RGB, %d bind RGBA; %s\n", depth, n, rgb,
          rgba, found >= 0 ? "a usable one found" : "NONE usable");
   GLXFBConfig r = found >= 0 ? c[found] : NULL;
   if (r)
      glXGetFBConfigAttrib(dpy, r, GLX_Y_INVERTED_EXT, y_inverted);
   XFree(c);
   return r;
}

static unsigned long
pixel(int depth, unsigned r, unsigned g, unsigned b)
{
   return (depth == 32 ? 0xff000000ul : 0) | r << 16 | g << 8 | b;
}

static int
near(const unsigned char *p, unsigned r, unsigned g, unsigned b)
{
   return abs(p[0] - (int)r) < 8 && abs(p[1] - (int)g) < 8 &&
          abs(p[2] - (int)b) < 8;
}

static int
test_depth(Display *dpy, int screen, Window win, GLXContext ctx, int depth)
{
   int y_inverted = 0, ok = 1;
   GLXFBConfig cfg = tfp_config(dpy, screen, depth, &y_inverted);
   bind_fn bind =
      (bind_fn)glXGetProcAddress((const GLubyte *)"glXBindTexImageEXT");
   release_fn release =
      (release_fn)glXGetProcAddress((const GLubyte *)"glXReleaseTexImageEXT");

   if (!cfg)
      return -1;
   if (!bind || !release) {
      printf("depth %d: no glXBindTexImageEXT\n", depth);
      return 0;
   }
   Pixmap pix = XCreatePixmap(dpy, RootWindow(dpy, screen), pw, ph, depth);
   GLenum target = rect ? GL_TEXTURE_RECTANGLE_ARB : GL_TEXTURE_2D;
   float sx = rect ? pw : 1, sy = rect ? ph : 1;
   GC gc = XCreateGC(dpy, pix, 0, NULL);
   const int attrs[] = {GLX_TEXTURE_TARGET_EXT,
                        rect ? GLX_TEXTURE_RECTANGLE_EXT : GLX_TEXTURE_2D_EXT,
                        GLX_TEXTURE_FORMAT_EXT,
                        depth == 32 ? GLX_TEXTURE_FORMAT_RGBA_EXT
                                    : GLX_TEXTURE_FORMAT_RGB_EXT,
                        None};
   GLXPixmap gp = glXCreatePixmap(dpy, cfg, pix, attrs);
   const unsigned colours[2][2][3] = {{{255, 0, 0}, {0, 0, 255}},
                                      {{0, 255, 0}, {255, 255, 255}}};
   GLuint tex;

   glGenTextures(1, &tex);
   for (int round = 0; round < 2; round++) {
      unsigned char left[4], right[4];
      const unsigned *l = colours[round][0], *r = colours[round][1];

      XSetForeground(dpy, gc, pixel(depth, l[0], l[1], l[2]));
      XFillRectangle(dpy, pix, gc, 0, 0, pw / 2, ph);
      XSetForeground(dpy, gc, pixel(depth, r[0], r[1], r[2]));
      XFillRectangle(dpy, pix, gc, pw / 2, 0, pw - pw / 2, ph);
      XSync(dpy, False);

      glClearColor(0.5f, 0.5f, 0.5f, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      glBindTexture(target, tex);
      glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      bind(dpy, gp, GLX_FRONT_LEFT_EXT, NULL);
      glEnable(target);
      glBegin(GL_QUADS);
      glTexCoord2f(0, 0);
      glVertex2f(-1, -1);
      glTexCoord2f(sx, 0);
      glVertex2f(1, -1);
      glTexCoord2f(sx, sy);
      glVertex2f(1, 1);
      glTexCoord2f(0, sy);
      glVertex2f(-1, 1);
      glEnd();
      glDisable(target);
      glFinish();
      glReadPixels(16, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, left);
      glReadPixels(48, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, right);
      release(dpy, gp, GLX_FRONT_LEFT_EXT);
      glXSwapBuffers(dpy, win);

      int pass = near(left, l[0], l[1], l[2]) && near(right, r[0], r[1], r[2]);

      printf("depth %d round %d: left %02x%02x%02x (want %02x%02x%02x) right "
             "%02x%02x%02x (want %02x%02x%02x): %s\n",
             depth, round, left[0], left[1], left[2], l[0], l[1], l[2],
             right[0], right[1], right[2], r[0], r[1], r[2],
             pass ? "pass" : "FAIL");
      ok &= pass;
   }
   printf("depth %d: y inverted %d\n", depth, y_inverted);
   glDeleteTextures(1, &tex);
   glXDestroyPixmap(dpy, gp);
   XFreeGC(dpy, gc);
   XFreePixmap(dpy, pix);
   (void)ctx;
   return ok;
}

int
main(int argc, char **argv)
{
   Display *dpy = XOpenDisplay(NULL);
   int screen, att[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8,
                        GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
   int fails = 0, tested = 0;

   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "-v"))
         verbose = 1;
      else if (!strcmp(argv[i], "-r"))
         rect = 1;
      else if (!strcmp(argv[i], "-s") && i + 1 < argc &&
               sscanf(argv[++i], "%dx%d", &pw, &ph) == 2)
         ;
      else {
         fprintf(stderr, "usage: glxtfp [-v] [-r] [-s WxH]\n");
         return 2;
      }
   }
   if (!dpy) {
      fprintf(stderr, "no display\n");
      return 2;
   }
   XSetErrorHandler(on_xerror);
   screen = DefaultScreen(dpy);
   printf("GLX extensions: %s\n",
          strstr(glXQueryExtensionsString(dpy, screen),
                 "GLX_EXT_texture_from_pixmap")
             ? "GLX_EXT_texture_from_pixmap present"
             : "GLX_EXT_texture_from_pixmap MISSING");
   XVisualInfo *vi = glXChooseVisual(dpy, screen, att);
   if (!vi) {
      fprintf(stderr, "no double-buffered RGB visual\n");
      return 2;
   }
   XSetWindowAttributes swa = {0};
   swa.colormap = XCreateColormap(dpy, RootWindow(dpy, screen), vi->visual,
                                  AllocNone);
   Window win = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0, 64, 64, 0,
                              vi->depth, InputOutput, vi->visual, CWColormap,
                              &swa);
   XMapWindow(dpy, win);
   XSync(dpy, False);
   GLXContext ctx = glXCreateContext(dpy, vi, NULL, True);
   glXMakeCurrent(dpy, win, ctx);
   printf("GL_RENDERER: %s\n", (const char *)glGetString(GL_RENDERER));
   glViewport(0, 0, 64, 64);

   /* the swap reaches X: a clear, two swaps (the second makes the first's
    * copy done), then the window's pixels as X has them */
   {
      int swap_ok = 0;

      for (int i = 0; i < 3; i++) {
         glClearColor(0, 1, 0, 1);
         glClear(GL_COLOR_BUFFER_BIT);
         glXSwapBuffers(dpy, win);
      }
      glFinish();
      XSync(dpy, False);
      XImage *im = XGetImage(dpy, win, 32, 32, 1, 1, AllPlanes, ZPixmap);
      if (im) {
         unsigned long p = XGetPixel(im, 0, 0);

         swap_ok = (p & 0xffffff) == 0x00ff00;
         printf("swap: window pixel %06lx after a green clear and swap: %s\n",
                p & 0xffffff, swap_ok ? "pass" : "FAIL");
         XDestroyImage(im);
      } else {
         printf("swap: XGetImage failed\n");
      }
      fails += !swap_ok;
   }

   for (int depth = 24; depth <= 32; depth += 8) {
      int r = test_depth(dpy, screen, win, ctx, depth);

      if (r < 0)
         continue;
      tested++;
      fails += !r;
   }
   printf("%s: %d depths tested, %d failed, %d X errors\n",
          tested && !fails && !xerrors ? "PASS" : "FAIL", tested, fails,
          xerrors);
   glXMakeCurrent(dpy, None, NULL);
   glXDestroyContext(dpy, ctx);
   XCloseDisplay(dpy);
   return tested && !fails && !xerrors ? 0 : 1;
}
