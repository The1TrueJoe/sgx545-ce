/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

/*
 * The fbdev platform: a window is the framebuffer console (/dev/fb0, or
 * $FRAMEBUFFER), the way the PowerVR DDK's LinuxFB WSEGL does it, so that
 * programs written for that (gl4es in framebuffer mode, es2gears for fbdev)
 * run unchanged.  The native window handle is not looked at; NULL is fine.
 *
 * The window's colour buffer is the framebuffer memory itself, made into a
 * pipe_resource with resource_from_user_memory: the GPU renders straight to
 * the scanout buffer and eglSwapBuffers only has to wait for the render.
 * Pbuffers are as on the surfaceless platform.
 *
 * The GPU is found the way surfaceless finds a software device: the first
 * DRM primary node whose driver the loader knows.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pipe/p_screen.h"
#include "util/libdrm.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"

#include "dri_screen.h"
#include "dri_util.h"
#include "egl_dri2.h"
#include "egldevice.h"
#include "eglglobals.h"
#include "loader.h"
#include "loader_dri_helper.h"

struct fbdev_device {
   int fd;
   void *map;
   size_t map_size;
   unsigned width, height, pitch;
   enum pipe_format format;
};

static struct fbdev_device fbdev = { .fd = -1 };

static bool
fbdev_open(void)
{
   struct fb_var_screeninfo var;
   struct fb_fix_screeninfo fix;
   const char *path = getenv("FRAMEBUFFER");

   if (fbdev.fd >= 0)
      return true;
   fbdev.fd = open(path && *path ? path : "/dev/fb0", O_RDWR | O_CLOEXEC);
   if (fbdev.fd < 0) {
      _eglLog(_EGL_WARNING, "fbdev: cannot open %s: %s", path ? path : "/dev/fb0",
              strerror(errno));
      return false;
   }
   if (ioctl(fbdev.fd, FBIOGET_VSCREENINFO, &var) || ioctl(fbdev.fd, FBIOGET_FSCREENINFO, &fix))
      goto fail;
   /* 32 bits, red at 16, green at 8, blue at 0: BGRA / BGRX in memory */
   if (var.bits_per_pixel != 32 || var.red.offset != 16 || var.green.offset != 8 ||
       var.blue.offset != 0) {
      _eglLog(_EGL_WARNING, "fbdev: unsupported framebuffer format (%u bpp, R%u G%u B%u)",
              var.bits_per_pixel, var.red.offset, var.green.offset, var.blue.offset);
      goto fail;
   }
   fbdev.format = var.transp.length ? PIPE_FORMAT_B8G8R8A8_UNORM : PIPE_FORMAT_B8G8R8X8_UNORM;
   fbdev.width = var.xres;
   fbdev.height = var.yres;
   fbdev.pitch = fix.line_length;
   fbdev.map_size = fix.smem_len;
   /*
    * Below 4 GB: the PowerVR services driver's OSAcquirePhysPageAddr keeps
    * the address it wraps in 32 bits (sgx545-ce kernel/src osfunc.c), so a
    * 64-bit process's fbdev mapping must be one a 32-bit pointer can name.
    */
   int flags = MAP_SHARED;
#ifdef MAP_32BIT
   flags |= MAP_32BIT;
#endif
   fbdev.map = mmap(NULL, fbdev.map_size, PROT_READ | PROT_WRITE, flags, fbdev.fd, 0);
   if (fbdev.map == MAP_FAILED)
      goto fail;
   return true;

fail:
   close(fbdev.fd);
   fbdev.fd = -1;
   return false;
}

static void
fbdev_free_images(struct dri2_egl_surface *dri2_surf)
{
   if (dri2_surf->front) {
      dri2_destroy_image(dri2_surf->front);
      dri2_surf->front = NULL;
   }
}

/* the framebuffer as a dri_image */
static struct dri_image *
fbdev_window_image(struct dri2_egl_display *dri2_dpy, struct dri2_egl_surface *dri2_surf)
{
   struct pipe_screen *pscreen = dri2_dpy->dri_screen_render_gpu->base.screen;
   struct pipe_resource templ = {
      .target = PIPE_TEXTURE_2D,
      .format = dri2_surf->visual,
      .width0 = fbdev.width,
      .height0 = fbdev.height,
      .depth0 = 1,
      .array_size = 1,
      .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_SCANOUT |
              PIPE_BIND_DISPLAY_TARGET,
   };
   struct pipe_resource *res;
   struct dri_image *img;

   if (!pscreen->resource_from_user_memory)
      return NULL;
   res = pscreen->resource_from_user_memory(pscreen, &templ, fbdev.map);
   if (!res)
      return NULL;
   img = CALLOC_STRUCT(dri_image);
   if (!img) {
      pipe_resource_reference(&res, NULL);
      return NULL;
   }
   img->texture = res;
   img->dri_format = dri2_surf->visual;
   img->screen = dri2_dpy->dri_screen_render_gpu;
   img->in_fence_fd = -1;
   return img;
}

static int
fbdev_image_get_buffers(struct dri_drawable *driDrawable, unsigned int format, uint32_t *stamp,
                        void *loaderPrivate, uint32_t buffer_mask, struct __DRIimageList *buffers)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dri2_surf->base.Resource.Display);
   bool window = dri2_surf->base.Type == EGL_WINDOW_BIT;

   buffers->image_mask = 0;
   buffers->front = NULL;
   buffers->back = NULL;

   if (!dri2_surf->front) {
      if (window)
         dri2_surf->front = fbdev_window_image(dri2_dpy, dri2_surf);
      else
         dri2_surf->front = dri_create_image(dri2_dpy->dri_screen_render_gpu, dri2_surf->base.Width,
                                             dri2_surf->base.Height, dri2_surf->visual, NULL, 0, 0,
                                             NULL);
      if (!dri2_surf->front)
         return 0;
   }

   /* A window draws into the scanout buffer whether GL calls it the back
    * buffer (double-buffered configs) or the front.  Pbuffers have only a
    * "front" buffer, as on the surfaceless platform. */
   if (window && (buffer_mask & __DRI_IMAGE_BUFFER_BACK)) {
      buffers->image_mask |= __DRI_IMAGE_BUFFER_BACK;
      buffers->back = dri2_surf->front;
   } else if (buffer_mask & __DRI_IMAGE_BUFFER_FRONT) {
      buffers->image_mask |= __DRI_IMAGE_BUFFER_FRONT;
      buffers->front = dri2_surf->front;
   }
   return 1;
}

static _EGLSurface *
dri2_fbdev_create_surface(_EGLDisplay *disp, EGLint type, _EGLConfig *conf,
                          const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_config *dri2_conf = dri2_egl_config(conf);
   struct dri2_egl_surface *dri2_surf;
   const struct dri_config *config;

   dri2_surf = calloc(1, sizeof(*dri2_surf));
   if (!dri2_surf) {
      _eglError(EGL_BAD_ALLOC, "eglCreateSurface");
      return NULL;
   }
   if (!dri2_init_surface(&dri2_surf->base, disp, type, conf, attrib_list, false, NULL))
      goto cleanup_surface;
   if (type == EGL_WINDOW_BIT) {
      dri2_surf->base.Width = fbdev.width;
      dri2_surf->base.Height = fbdev.height;
   }

   config = dri2_get_dri_config(dri2_conf, type, dri2_surf->base.GLColorspace);
   if (!config) {
      _eglError(EGL_BAD_MATCH, "Unsupported surfacetype/colorspace configuration");
      goto cleanup_surface;
   }
   if (type == EGL_WINDOW_BIT) {
      dri2_surf->visual = fbdev.format;
   } else {
      dri2_surf->visual = dri2_image_format_for_pbuffer_config(dri2_dpy, config);
      if (dri2_surf->visual == PIPE_FORMAT_NONE)
         goto cleanup_surface;
   }
   if (!dri2_create_drawable(dri2_dpy, config, dri2_surf, dri2_surf))
      goto cleanup_surface;
   return &dri2_surf->base;

cleanup_surface:
   free(dri2_surf);
   return NULL;
}

static _EGLSurface *
dri2_fbdev_create_window_surface(_EGLDisplay *disp, _EGLConfig *conf, void *native_window,
                                 const EGLint *attrib_list)
{
   return dri2_fbdev_create_surface(disp, EGL_WINDOW_BIT, conf, attrib_list);
}

static _EGLSurface *
dri2_fbdev_create_pbuffer_surface(_EGLDisplay *disp, _EGLConfig *conf, const EGLint *attrib_list)
{
   return dri2_fbdev_create_surface(disp, EGL_PBUFFER_BIT, conf, attrib_list);
}

static EGLBoolean
dri2_fbdev_destroy_surface(_EGLDisplay *disp, _EGLSurface *surf)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);

   fbdev_free_images(dri2_surf);
   driDestroyDrawable(dri2_surf->dri_drawable);
   dri2_fini_surface(surf);
   free(dri2_surf);
   return EGL_TRUE;
}

static EGLBoolean
dri2_fbdev_swap_buffers(_EGLDisplay *disp, _EGLSurface *draw)
{
   /* the render goes to the scanout buffer: finishing it is presenting it */
   dri2_flush_drawable_for_swapbuffers(disp, draw);
   return EGL_TRUE;
}

static const struct dri2_egl_display_vtbl dri2_fbdev_display_vtbl = {
   .create_window_surface = dri2_fbdev_create_window_surface,
   .create_pbuffer_surface = dri2_fbdev_create_pbuffer_surface,
   .destroy_surface = dri2_fbdev_destroy_surface,
   .create_image = dri2_create_image_khr,
   .swap_buffers = dri2_fbdev_swap_buffers,
   .get_dri_drawable = dri2_surface_get_dri_drawable,
};

static void
fbdev_flush_front_buffer(struct dri_drawable *driDrawable, void *loaderPrivate)
{
}

static unsigned
fbdev_get_capability(void *loaderPrivate, enum dri_loader_cap cap)
{
   switch (cap) {
   case DRI_LOADER_CAP_RGBA_ORDERING:
      return 1;
   default:
      return 0;
   }
}

static const __DRIimageLoaderExtension image_loader_extension = {
   .base = { __DRI_IMAGE_LOADER, 2 },
   .getBuffers = fbdev_image_get_buffers,
   .flushFrontBuffer = fbdev_flush_front_buffer,
   .getCapability = fbdev_get_capability,
};

static const __DRIextension *image_loader_extensions[] = {
   &image_loader_extension.base,
   &image_lookup_extension.base,
   NULL,
};

/*
 * The first DRM device (primary node) a driver loads on.  EGL's own device
 * list only has devices with a render node, and the PowerVR services driver
 * (pvrsrvkm) has none, so the platform looks at the primary nodes itself.
 */
static bool
fbdev_probe_device(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   drmDevicePtr devices[16];
   int n = drmGetDevices2(0, devices, ARRAY_SIZE(devices));
   bool found = false;

   for (int i = 0; i < n && !found; i++) {
      if (!(devices[i]->available_nodes & (1 << DRM_NODE_PRIMARY)))
         continue;
      dri2_dpy->fd_render_gpu = loader_open_device(devices[i]->nodes[DRM_NODE_PRIMARY]);
      if (dri2_dpy->fd_render_gpu < 0)
         continue;
      dri2_dpy->fd_display_gpu = dri2_dpy->fd_render_gpu;
      dri2_dpy->driver_name = loader_get_driver_for_fd(dri2_dpy->fd_render_gpu);
      if (dri2_dpy->driver_name) {
         disp->Device = _eglFindDevice(dri2_dpy->fd_render_gpu, false);
         if (!disp->Device)
            disp->Device = _eglFindDevice(-1, true);
         dri2_dpy->loader_extensions = image_loader_extensions;
         if (dri2_create_screen(disp)) {
            found = true;
            break;
         }
         _eglLog(_EGL_WARNING, "fbdev: failed to create a %s screen", dri2_dpy->driver_name);
      }
      free(dri2_dpy->driver_name);
      dri2_dpy->driver_name = NULL;
      close(dri2_dpy->fd_render_gpu);
      dri2_dpy->fd_render_gpu = dri2_dpy->fd_display_gpu = -1;
   }
   if (n > 0)
      drmFreeDevices(devices, n);
   return found;
}

EGLBoolean
dri2_initialize_fbdev(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   unsigned count = 0;

   if (!fbdev_open())
      return _eglError(EGL_NOT_INITIALIZED, "fbdev: no framebuffer");
   if (!fbdev_probe_device(disp))
      return _eglError(EGL_NOT_INITIALIZED, "fbdev: failed to load driver");

   dri2_setup_screen(disp);

   /* window configs: the ones in the framebuffer's format; pbuffers get every
    * format the surfaceless platform offers */
   for (unsigned i = 0; dri2_dpy->driver_configs[i]; i++) {
      const struct gl_config *gl_config = (const struct gl_config *)dri2_dpy->driver_configs[i];
      EGLint surface_type = EGL_PBUFFER_BIT;

      if (gl_config->color_format == fbdev.format ||
          (fbdev.format == PIPE_FORMAT_B8G8R8X8_UNORM &&
           gl_config->color_format == PIPE_FORMAT_B8G8R8A8_UNORM))
         surface_type |= EGL_WINDOW_BIT;
      else if (dri2_image_format_for_pbuffer_config(dri2_dpy, dri2_dpy->driver_configs[i]) ==
               PIPE_FORMAT_NONE)
         continue;
      if (dri2_add_config(disp, dri2_dpy->driver_configs[i], surface_type, NULL))
         count++;
   }
   if (!count)
      return _eglError(EGL_NOT_INITIALIZED, "fbdev: no usable configs");

   dri2_dpy->vtbl = &dri2_fbdev_display_vtbl;
   return EGL_TRUE;
}
