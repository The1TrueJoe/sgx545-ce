/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * sgx_hwt: run hand-written fragment programs through the winsys and read
 * their result back (docs/ISA.md "Hardware checks"; docs/porting/isa.md).
 * Each test is ISA.md text, assembled here through the isa library. It ends
 * with `mov o0, rN [skipinv end mask=1]` (it never reaches the winsys's
 * colour epilogue; a PA write would land outside the task's PA), and the
 * pixel back end stores o0 unchanged: a pixel inside the triangle is the
 * 32-bit register. The clear colour (0xcc336699) shows a program that
 * never ran.
 *
 *   sgx_hwt [-d node] [-o frame.bin] tests.txt   (-o: the last frame, BGRA)
 *
 * tests.txt:
 *   test NAME
 *   buf 11223344 ...    optional: the data buffer's first dwords (up to 64)
 *   limm r1, BUF        lines of ISA.md text; BUF is the buffer's address
 *   raw HI LO           a word given as it is (one no form encodes)
 *   tex TEXEL           optional: a texture in the texture heap (1 KiB),
 *                       every dword TEXEL (its bytes in memory order); TEX in
 *                       a line is its address
 *   texpat              optional: the same heap with dword i = 0x5a5a0000 | i,
 *                       so a texel read back names the dword it came from
 *   ...
 *   end
 *
 * Prints, per test, "NAME <word>" (all checked pixels equal) or
 * "NAME mixed <first> <other>", then " buf <dwords>" when it had one.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sgx_frame.h"
#include "sgx_isa.h"
#include "sgx_isa_asm.h"

#define MAX_INSTS 256
#define BUF_DWORDS 64
#define TEX_DWORDS 256

/* a line with BUF / TEX replaced by the address */
static void
subst(char *out, size_t size, const char *in, const char *tok, uint32_t va)
{
   const char *b = strstr(in, tok);
   if (b)
      snprintf(out, size, "%.*s#0x%x%s", (int)(b - in), in, va,
               b + strlen(tok));
   else
      snprintf(out, size, "%s", in);
}

static int
run(struct sgxws_ctx *c, struct sgxws_rt *rt, struct sgxws_target *t,
    struct sgxws_bo *buf, struct sgxws_bo *tex, const char *name,
    char lines[][160], unsigned n, const uint32_t *init, unsigned n_init,
    int has_texel, uint32_t texel, const uint32_t *pixels, uint32_t w,
    uint32_t h, uint32_t stride)
{
   static const float verts[6] = {-0.9f, -0.9f, 0.9f, -0.9f, 0.0f, 0.9f};
   uint64_t code[MAX_INSTS];
   struct sgx_isa_fc fc = SGX_ISA_FC_START;
   struct sgxws_prog vs, fs = {0};
   struct sgxws_state st = {0};
   char err[128], text[200];

   for (unsigned i = 0; i < n; i++) {
      struct sgx_isa_instr I;
      char t1[200];
      unsigned hi, lo;

      if (sscanf(lines[i], "raw %x %x", &hi, &lo) == 2) {
         code[i] = (uint64_t)hi << 32 | lo;
         continue;
      }
      subst(t1, sizeof(t1), lines[i], "BUF", buf->va);
      subst(text, sizeof(text), t1, "TEX", tex->va);
      if (!sgx_isa_asm_line(text, &I, err, sizeof(err))) {
         printf("%s error %s: %s\n", name, text, err);
         return 0;
      }
      code[i] = sgx_isa_encode(&I, &fc);
      if (I.opc == SGX_ISA_OPC_SETFC) {
         fc.f16 = I.a[0];
         fc.c10 = I.a[1];
      }
   }
   memset(buf->map, 0, BUF_DWORDS * 4);
   memcpy(buf->map, init, n_init * 4);
   for (unsigned i = 0; i < TEX_DWORDS; i++)
      tex->map[i] = has_texel == 2 ? 0x5a5a0000u | i : has_texel ? texel : 0;

   sgxws_prog_builtin(c, SGXWS_VS_XY, &vs);
   /* the uniform-colour program's setup (its secondary loads sa9, which the
    * test doesn't read), the test as the main program */
   sgxws_prog_builtin(c, SGXWS_FS_UNIFORM, &fs);
   fs.code = sgxws_upload_fs(c, code, n, 0, NULL, -1, 16);
   fs.temps = 16;
   if (!fs.code) {
      printf("%s error upload\n", name);
      return 0;
   }
   st.color_mask = 0xf;
   st.clip = (struct sgxws_rect){0, 0, w, h};
   st.vp_scale[0] = w / 2.0f;
   st.vp_translate[0] = w / 2.0f;
   st.vp_scale[1] = -(h / 2.0f);
   st.vp_translate[1] = h / 2.0f;
   st.vp_scale[2] = 0.5f;
   st.vp_translate[2] = 0.5f;

   const float clear[4] = {0.2f, 0.4f, 0.6f, 0.8f};
   uint32_t fsc[SGXWS_FS_UNIFORM_DWORDS] = {0};
   struct sgxws_draw d = {
      .fs_consts = fsc,
      .prim = SGXWS_TRIANGLES,
      .count = 3,
      .verts = (const uint32_t *)verts,
      .vs = &vs,
      .fs = &fs,
      .state = &st,
   };
   int e;
   if (sgxws_begin(c, rt, t) || sgxws_clear(c, clear, 1.0f, false, 0xf, -1))
      return 1;
   sgxws_use_bo(c, buf, true);
   sgxws_use_bo(c, tex, false);
   if (sgxws_draw(c, &d))
      return 1;
   if ((e = sgxws_flush(c, 2000))) {
      printf("%s error flush %d\n", name, e);
      return 1; /* the GPU may need a reset: stop */
   }

   /* 32 pixels across the middle of the triangle */
   const uint32_t *row = pixels + (size_t)(h / 2) * stride + w / 2 - 16;
   uint32_t first = row[0], other = first;
   for (unsigned x = 1; x < 32; x++) {
      if (row[x] != first)
         other = row[x];
   }
   if (other == first)
      printf("%s %08x", name, first);
   else
      printf("%s mixed %08x %08x", name, first, other);
   if (n_init) {
      printf(" buf");
      for (unsigned i = 0; i < n_init; i++)
         printf(" %08x", buf->map[i]);
   }
   printf("\n");
   fflush(stdout);
   return 0;
}

int
main(int argc, char **argv)
{
   struct sgxws_dev_options opts = {.dry_run = -1};
   const char *node = NULL;
   /* the render target sgx_frame_test draws to */
   uint32_t w = 720, h = 480, stride = 736;
   const char *dump = NULL;
   int opt;

   while ((opt = getopt(argc, argv, "d:o:")) != -1) {
      if (opt == 'd')
         node = optarg;
      else if (opt == 'o')
         dump = optarg;
      else
         return 2;
   }
   if (optind >= argc) {
      fprintf(stderr, "usage: sgx_hwt [-d node] tests.txt\n");
      return 2;
   }
   FILE *f = fopen(argv[optind], "r");
   if (!f) {
      perror(argv[optind]);
      return 1;
   }
   if (!node)
      node = access("/dev/dri/renderD128", F_OK) ? "/dev/dri/card0"
                                                 : "/dev/dri/renderD128";
   int fd = open(node, O_RDWR | O_CLOEXEC);
   struct sgxws_dev *dev = fd < 0 ? NULL : sgxws_dev_create(fd, &opts);
   if (!dev) {
      fprintf(stderr, "no SGX kernel driver on %s\n", node);
      return 1;
   }
   struct sgxws_ctx *c = sgxws_ctx_create(dev, 0);
   struct sgxws_rt *rt = c ? sgxws_rt_create(c, w, h) : NULL;
   struct sgxws_bo color, buf, tex;
   if (!rt ||
       sgxws_bo_alloc(dev, &color, SGXWS_MEM_RENDER_TARGET, stride * h * 4,
                      0x1000) ||
       sgxws_bo_alloc(dev, &buf, SGXWS_MEM_VERTEX, BUF_DWORDS * 4, 0x1000) ||
       /* sampling outside the texture heap hangs (sgx_core) */
       sgxws_bo_alloc(dev, &tex, SGXWS_MEM_TEXTURE, TEX_DWORDS * 4, 0x1000))
      return 1;
   /* BGRA8888, strided (TEX.md sections 4, 11) */
   struct sgxws_target t = {.va = color.va, .stride = stride, .bo = &color,
                            .t1_format = 0x0c, .t1_layout = 6};

   static char lines[MAX_INSTS][160];
   char ln[256], name[64] = "";
   uint32_t init[BUF_DWORDS], texel = 0;
   unsigned n = 0, n_init = 0;
   int has_texel = 0;
   int status = 0;
   while (fgets(ln, sizeof(ln), f)) {
      ln[strcspn(ln, "\r\n")] = 0;
      char *s = ln + strspn(ln, " \t");
      if (!*s || *s == '#')
         continue;
      if (!strncmp(s, "test ", 5)) {
         snprintf(name, sizeof(name), "%s", s + 5);
         n = n_init = 0;
         has_texel = 0;
      } else if (!strcmp(s, "texpat")) {
         has_texel = 2;
      } else if (!strncmp(s, "tex ", 4)) {
         texel = strtoul(s + 4, NULL, 16);
         has_texel = 1;
      } else if (!strncmp(s, "buf ", 4)) {
         for (char *p = s + 4; *p && n_init < BUF_DWORDS;) {
            char *e;
            init[n_init] = strtoul(p, &e, 16);
            if (e == p)
               break;
            n_init++;
            p = e;
         }
      } else if (!strcmp(s, "end")) {
         if (run(c, rt, &t, &buf, &tex, name, lines, n, init, n_init,
                 has_texel, texel, color.map, w, h, stride)) {
            status = 1;
            break;
         }
      } else if (n < MAX_INSTS) {
         snprintf(lines[n++], sizeof(lines[0]), "%s", s);
      }
   }
   fclose(f);
   if (dump) {
      FILE *o = fopen(dump, "wb");
      if (o) {
         fwrite(color.map, 4, (size_t)stride * h, o);
         fclose(o);
      }
   }
   sgxws_bo_free(dev, &buf);
   sgxws_bo_free(dev, &tex);
   sgxws_bo_free(dev, &color);
   sgxws_rt_destroy(c, rt);
   sgxws_ctx_destroy(c);
   sgxws_dev_destroy(dev);
   return status;
}
