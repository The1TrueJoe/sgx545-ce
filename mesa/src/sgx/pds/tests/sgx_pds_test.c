/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * sgx_pds_test [-f N] [-t] FILE...
 * sgx_pds_test -d < words
 *
 * Each line of FILE is `<text> [; <word>]`: the text is assembled, encoded
 * and disassembled again, and must come back the same; with a word, the
 * encoding must equal it. Then a set of known DOUT data
 * words. -f N also checks N random words: every word that decodes must
 * assemble back to itself. -t prints `<text> ; <word>` for each line.
 * -d disassembles hex words read from stdin, one `<word>  <text>` per word.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sgx_pds.h"

static unsigned failures, checked;

static void
fail(const char *what, const char *line, const char *detail)
{
   failures++;
   fprintf(stderr, "FAIL %s: %s (%s)\n", what, line, detail);
}

static void
check_line(const char *line, bool table)
{
   char text[256], back[256];
   const char *semi = strchr(line, ';');
   size_t n = semi ? (size_t)(semi - line) : strlen(line);
   struct sgx_pds_instr in;

   while (n &&
          (line[n - 1] == ' ' || line[n - 1] == '\t' || line[n - 1] == '\n'))
      n--;
   memcpy(text, line, n);
   text[n] = '\0';
   checked++;

   const char *err = sgx_pds_asm(text, &in);
   if (err) {
      fail("asm", text, err);
      return;
   }
   uint32_t w = sgx_pds_encode(&in);
   if (semi) {
      uint32_t want = strtoul(semi + 1, NULL, 16);
      if (w != want) {
         snprintf(back, sizeof(back), "got %08x, want %08x", w, want);
         fail("encode", text, back);
      }
   }
   if (!sgx_pds_disasm_one(w, back, sizeof(back)) || strcmp(back, text)) {
      char d[300];
      snprintf(d, sizeof(d), "%08x disassembles to \"%s\"", w, back);
      fail("round trip", text, d);
   }
   if (table)
      printf("%s ; %08x\n", text, w);
}

static void
check_word(const char *what, uint32_t got, uint32_t want)
{
   char d[64];

   checked++;
   if (got != want) {
      snprintf(d, sizeof(d), "got %08x, want %08x", got, want);
      fail("data word", what, d);
   }
}

/* Known DOUT data words. */
static void
check_data_words(void)
{
   /* DOUTI */
   check_word("37c00800: tc0 x4, f16, last",
              sgx_pds_douti_w0(&(struct sgx_pds_douti){.src = 1,
                                                       .comps = 4,
                                                       .iterate = true,
                                                       .b25_26 = 3,
                                                       .prec1 = true,
                                                       .last_iter = true}),
              0x37c00800);
   check_word("17c00800: tc0 x4, f16",
              sgx_pds_douti_w0(&(struct sgx_pds_douti){.src = 1,
                                                       .comps = 4,
                                                       .iterate = true,
                                                       .b25_26 = 3,
                                                       .prec1 = true}),
              0x17c00800);
   check_word("37401000: tc1 x2, f16, last",
              sgx_pds_douti_w0(&(struct sgx_pds_douti){.src = 2,
                                                       .comps = 2,
                                                       .iterate = true,
                                                       .b25_26 = 3,
                                                       .prec1 = true,
                                                       .last_iter = true}),
              0x37401000);
   check_word("37c00a02: tc0 x4 merged with a 2-D sample of tc1",
              sgx_pds_douti_w0(&(struct sgx_pds_douti){.sample_set = 2,
                                                       .dim2 = true,
                                                       .src = 1,
                                                       .comps = 4,
                                                       .iterate = true,
                                                       .b25_26 = 3,
                                                       .prec1 = true,
                                                       .last_iter = true}),
              0x37c00a02);
   check_word("46012203: a pure 2-D sample of tc2, last sample",
              sgx_pds_douti_w0(&(struct sgx_pds_douti){.sample_set = 3,
                                                       .dim2 = true,
                                                       .src = 0x24,
                                                       .b25_26 = 3,
                                                       .last_sample = true}),
              0x46012203);
   check_word("21c10000: the clear's color x4",
              sgx_pds_douti_w0(&(struct sgx_pds_douti){.src = 0x20,
                                                       .comps = 4,
                                                       .iterate = true,
                                                       .last_iter = true}),
              0x21c10000);
   check_word("40012001: background object, sample tc0",
              sgx_pds_douti_w0(&(struct sgx_pds_douti){.sample_set = 1,
                                                       .src = 0x24,
                                                       .last_sample = true}),
              0x40012001);
   check_word("27800000: gl_FragCoord x3",
              sgx_pds_douti_w0(&(struct sgx_pds_douti){.src = 0,
                                                       .comps = 3,
                                                       .iterate = true,
                                                       .b25_26 = 3,
                                                       .last_iter = true}),
              0x27800000);
   check_word("37400800 + w1 0x10: sprite gl_PointCoord",
              sgx_pds_douti_w0(&(struct sgx_pds_douti){.src = 1,
                                                       .comps = 2,
                                                       .iterate = true,
                                                       .b25_26 = 3,
                                                       .prec1 = true,
                                                       .last_iter = true}),
              0x37400800);
   check_word("douti w1 0x10",
              sgx_pds_douti_w1(&(struct sgx_pds_douti){.w1 = 0x10}), 0x10);

   /* DOUTU word 1 */
   check_word(
      "02000004: tex1",
      sgx_pds_doutu_w1(&(struct sgx_pds_doutu){.pixel = true, .samples = true}),
      0x02000004);
   check_word("01000004: vary2",
              sgx_pds_doutu_w1(
                 &(struct sgx_pds_doutu){.pixel = true, .iterations = true}),
              0x01000004);
   check_word("03000004: tex2",
              sgx_pds_doutu_w1(&(struct sgx_pds_doutu){.pixel = true,
                                                       .iterations = true,
                                                       .samples = true}),
              0x03000004);
   check_word("00000002: microkernel",
              sgx_pds_doutu_w1(&(struct sgx_pds_doutu){.b1 = true}), 2);
   check_word(
      "00c00000: pixel event",
      sgx_pds_doutu_w1(&(struct sgx_pds_doutu){.b22 = true, .b23 = true}),
      0x00c00000);

   /* DOUTU word 0: base 3, offset 0x1230, 8 temporaries */
   check_word(
      "doutu w0",
      sgx_pds_doutu_w0(
         &(struct sgx_pds_doutu){.base = 3, .offset = 0x1230, .temps4 = 2}),
      0x02000000 | 0x1230 >> 3 << 4 | 3);

   /* DOUTD word 1: attr3's streams (vec3 at register 4), a state DMA */
   check_word(
      "doutd vec3 -> 4",
      sgx_pds_doutd_ctrl(&(struct sgx_pds_doutd){.dwords = 3, .reg = 4}),
      0x00000402);
   check_word("doutd state",
              sgx_pds_doutd_ctrl(&(struct sgx_pds_doutd){.dwords = 10,
                                                         .reg = 0,
                                                         .kind = 1,
                                                         .secondary = true}),
              0x02400009);

   /* DOUTA word 1: pds3's (in2, 0x100) */
   check_word("douta pa1",
              sgx_pds_douta_ctrl(&(struct sgx_pds_douta){.reg = 1}), 0x100);
   check_word("douta pa2",
              sgx_pds_douta_ctrl(&(struct sgx_pds_douta){.reg = 2}), 0x200);
}

static void
fuzz(unsigned n)
{
   uint32_t x = 0x2545f491;
   unsigned decoded = 0;

   for (unsigned i = 0; i < n; i++) {
      char text[256], back[256];
      struct sgx_pds_instr in;

      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      /* bias toward valid opcodes: keep the top byte random, clear some low
       * bits */
      uint32_t w = i & 1 ? x : x & 0xff00ffff;

      if (!sgx_pds_disasm_one(w, text, sizeof(text)))
         continue;
      decoded++;
      checked++;
      const char *err = sgx_pds_asm(text, &in);
      uint32_t got = err ? 0 : sgx_pds_encode(&in);
      if (err || got != w) {
         snprintf(back, sizeof(back), "%08x -> %s -> %08x", w, err ? err : "",
                  got);
         fail("fuzz", text, back);
      }
   }
   printf("fuzz: %u words, %u decode\n", n, decoded);
}

int
main(int argc, char **argv)
{
   bool table = false;
   unsigned nfuzz = 0;
   int i = 1;

   for (; i < argc && argv[i][0] == '-'; i++) {
      if (!strcmp(argv[i], "-d")) {
         unsigned w;
         char text[256];

         while (scanf("%x", &w) == 1) {
            if (!sgx_pds_disasm_one(w, text, sizeof(text)))
               failures++;
            printf("%08x  %s\n", w, text);
         }
         return failures != 0;
      }
      if (!strcmp(argv[i], "-t"))
         table = true;
      else if (!strcmp(argv[i], "-f") && i + 1 < argc)
         nfuzz = strtoul(argv[++i], NULL, 0);
   }
   for (; i < argc; i++) {
      FILE *f = fopen(argv[i], "r");
      char line[512];

      if (!f) {
         perror(argv[i]);
         return 2;
      }
      while (fgets(line, sizeof(line), f)) {
         char *p = line;
         while (*p == ' ' || *p == '\t')
            p++;
         if (*p == '#' || *p == '\n' || !*p)
            continue;
         check_line(p, table);
      }
      fclose(f);
   }
   check_data_words();
   if (nfuzz)
      fuzz(nfuzz);
   printf("%u checks, %u failures\n", checked, failures);
   return failures != 0;
}
