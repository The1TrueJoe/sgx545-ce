/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

/*
 * sgx545_frame_test: the winsys on its own, no Mesa.  Draws the probe's
 * reference scene (clear (0, 0, 0.5, 1), a green triangle) for n frames in
 * one context and checks every frame's pixels against the first; frame k
 * clears with red = k/n when -r is given (probe-st's PROBE_FRAMES).
 *
 *   sgx545_frame_test [-n frames] [-r] [-c] [-f] [-o last.bin]
 *     -c   check every frame (reading uncached GPU memory: slow), not only the last
 *     -f   render straight into /dev/fb0 (wrapped), not a GPU buffer
 *     -o   dump the last frame (736-pixel pitch BGRA, as re/decode/cmpfb.py reads)
 */

#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "sgx545_frame.h"

static uint64_t
fnv(const void *p, size_t n)
{
   const unsigned char *s = p;
   uint64_t h = 0xcbf29ce484222325ull;

   while (n--)
      h = (h ^ *s++) * 0x100000001b3ull;
   return h;
}

int
main(int argc, char **argv)
{
   struct sgxws_dev dev;
   struct sgxws_ctx *c;
   struct sgxws_rt *rt;
   struct sgxws_bo color;
   struct sgxws_target t = { 0 };
   struct sgxws_prog vs, fs;
   struct pvr_buf syncmap, fbbuf;
   struct sgxws_state st = { 0 };
   const char *out = NULL;
   int frames = 1, ramp = 0, use_fb = 0, check_all = 0, opt, fd, bad = 0;
   uint32_t w = 720, h = 480, stride = 736, *pixels;
   uint64_t first = 0;
   float verts[6] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f };
   uint32_t fsc[SGXWS_FS_UNIFORM_DWORDS] = { 0 };
   struct timespec t0, t1;

   while ((opt = getopt(argc, argv, "n:rcfo:")) != -1) {
      switch (opt) {
      case 'n': frames = atoi(optarg); break;
      case 'r': ramp = 1; break;
      case 'c': check_all = 1; break;
      case 'f': use_fb = 1; break;
      case 'o': out = optarg; break;
      default: return 2;
      }
   }

   fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
   if (fd < 0 || sgxws_dev_init(&dev, fd)) {
      fprintf(stderr, "services init failed\n");
      return 1;
   }
   c = sgxws_ctx_create(&dev);
   if (!c)
      return 1;
   rt = sgxws_rt_create(c, w, h);
   if (!rt)
      return 1;

   if (use_fb) {
      int fb = open("/dev/fb0", O_RDWR);
      struct fb_fix_screeninfo fix;
      void *m;

      if (fb < 0 || ioctl(fb, FBIOGET_FSCREENINFO, &fix)) {
         perror("/dev/fb0");
         return 1;
      }
      m = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
      if (m == MAP_FAILED || pvr_wrap(&dev.pvr, &fbbuf, m, fix.smem_len)) {
         fprintf(stderr, "wrapping fb0 failed\n");
         return 1;
      }
      stride = fix.line_length / 4;
      t.va = fbbuf.devva;
      t.sync = fbbuf.sync;
      pixels = m;
   } else {
      if (sgxws_bo_alloc(&dev, &color, SGX545_HEAP_GENERAL, SGXWS_ATTRIBS_COLOR, stride * h * 4,
                         0x1000))
         return 1;
      t.va = color.va;
      t.sync = color.b.sync;
      pixels = color.map;
   }
   t.stride = stride;
   t.sync_data = pvr_sync_data(&dev.pvr, t.sync, &syncmap);

   sgxws_prog_builtin(c, SGXWS_VS_XY, &vs);
   sgxws_prog_builtin(c, SGXWS_FS_UNIFORM, &fs);
   fsc[SGXWS_FS_UNIFORM_OFF] = sgxws_f16(0) | sgxws_f16(1) << 16;
   fsc[SGXWS_FS_UNIFORM_OFF + 1] = sgxws_f16(0) | sgxws_f16(1) << 16;
   st.color_mask = 0xf;
   st.vp_scale[0] = w / 2.0f;
   st.vp_translate[0] = w / 2.0f;
   st.vp_scale[1] = -(h / 2.0f);
   st.vp_translate[1] = h / 2.0f;
   st.vp_scale[2] = 0.5f;
   st.vp_translate[2] = 0.5f;

   clock_gettime(CLOCK_MONOTONIC, &t0);
   for (int f = 0; f < frames; f++) {
      float clear[4] = { ramp ? (float)f / frames : 0.0f, 0, 0.5f, 1 };
      struct sgxws_draw d = {
         .prim = SGXWS_TRIANGLES, .count = 3, .verts = (const uint32_t *)verts,
         .vs = &vs, .fs = &fs, .state = &st, .fs_consts = fsc,
      };
      uint64_t sum;
      int e;

      if (sgxws_begin(c, rt, &t) || sgxws_clear(c, clear, 1.0f, false) || sgxws_draw(c, &d))
         return 1;
      if ((e = sgxws_flush(c, 2000))) {
         fprintf(stderr, "frame %d: flush failed %d\n", f, e);
         return 1;
      }
      if (!check_all && f && f != frames - 1)
         continue;
      sum = fnv(pixels, (size_t)stride * h * 4);
      if (!f)
         first = sum;
      else if (!ramp && sum != first && bad++ < 10)
         fprintf(stderr, "frame %d differs from frame 0\n", f);
   }
   clock_gettime(CLOCK_MONOTONIC, &t1);
   printf("sgx545_frame_test: %d frames, %d differ, %.2f ms/frame, last %016llx\n", frames, bad,
          ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / frames,
          (unsigned long long)fnv(pixels, (size_t)stride * h * 4));
   if (out) {
      FILE *fp = fopen(out, "wb");

      if (fp) {
         fwrite(pixels, 4, (size_t)stride * h, fp);
         fclose(fp);
      }
   }
   sgxws_rt_destroy(c, rt);
   sgxws_ctx_destroy(c);
   sgxws_dev_fini(&dev);
   return bad ? 1 : 0;
}
