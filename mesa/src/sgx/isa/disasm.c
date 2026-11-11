/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * sgx-disasm: disassemble USSE instructions to the USSE text notation
 * (README.md).
 *
 *    sgx-disasm [-c core] [-F fc] [-x] [-w] [file...]
 *
 * Reads the files (or stdin): words, low word first, little endian, or with
 * -x hex text, the first two words of each line (so sgx-asm's output and
 * -w's read back; '#' and ';' lines are skipped). Writes one line per
 * instruction, the text alone, or with -w "<lo> <hi>  <text>". -c and -F as
 * for sgx-asm; setfc words change the format control for the words after
 * them. The exit status is 1 if a word didn't decode.
 */

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sgx_isa.h"

struct dis_state {
   struct sgx_isa_fc fc;
   bool words;
   unsigned errors;
};

static void
disasm_word(struct dis_state *st, uint32_t lo, uint32_t hi)
{
   uint64_t w = lo | (uint64_t)hi << 32;
   char dis[512];

   if (!sgx_disasm_one(w, &st->fc, dis, sizeof(dis)))
      st->errors++;
   if (st->words)
      printf("%08x %08x  %s\n", lo, hi, dis);
   else
      printf("%s\n", dis);
   sgx_isa_setfc(w, &st->fc);
}

static void
disasm_bin(struct dis_state *st, const char *name, FILE *in)
{
   uint8_t b[8];
   size_t n;

   while ((n = fread(b, 1, sizeof(b), in)) == sizeof(b)) {
      disasm_word(st, b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24,
                  b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24);
   }
   if (n) {
      fprintf(stderr, "%s: %zu trailing bytes\n", name, n);
      st->errors++;
   }
}

static void
disasm_hex(struct dis_state *st, const char *name, FILE *in)
{
   char line[1024];
   unsigned lineno = 0;

   while (fgets(line, sizeof(line), in)) {
      char *text = line + strspn(line, " \t");
      uint32_t lo, hi;

      lineno++;
      if (!*text || *text == '\n' || *text == '#' || *text == ';')
         continue;
      if (sscanf(text, "%x %x", &lo, &hi) != 2) {
         text[strcspn(text, "\r\n")] = '\0';
         fprintf(stderr, "%s:%u: not two hex words: %s\n", name, lineno, text);
         st->errors++;
         continue;
      }
      disasm_word(st, lo, hi);
   }
}

/* "545" or "sgx545"; 0 if sgx.xml doesn't describe it */
static unsigned
parse_core(const char *arg)
{
   char *end;

   if (!strncmp(arg, "sgx", 3))
      arg += 3;
   unsigned long core = strtoul(arg, &end, 10);
   return *arg && !*end && core && sgx_isa_core_supported(core) ? core : 0;
}

static void
usage(void)
{
   fprintf(stderr, "usage: sgx-disasm [-c core] [-F fc] [-x] [-w] [file...]\n");
   exit(2);
}

int
main(int argc, char **argv)
{
   struct dis_state st = {
      .fc = SGX_ISA_FC_START,
   };
   bool hex = false;
   int opt;

   while ((opt = getopt(argc, argv, "c:F:xwh")) != -1) {
      switch (opt) {
      case 'c':
         st.fc.core = parse_core(optarg);
         if (!st.fc.core) {
            fprintf(stderr, "sgx-disasm: core %s is not in sgx.xml\n", optarg);
            return 2;
         }
         break;
      case 'F': {
         unsigned fc = strtoul(optarg, NULL, 0);
         st.fc.c10 = fc & 1;
         st.fc.f16 = fc & 2;
         break;
      }
      case 'x':
         hex = true;
         break;
      case 'w':
         st.words = true;
         break;
      default:
         usage();
      }
   }

   if (optind == argc) {
      if (hex)
         disasm_hex(&st, "<stdin>", stdin);
      else
         disasm_bin(&st, "<stdin>", stdin);
   }
   for (int i = optind; i < argc; i++) {
      FILE *in = fopen(argv[i], hex ? "r" : "rb");

      if (!in) {
         fprintf(stderr, "sgx-disasm: %s: %s\n", argv[i], strerror(errno));
         st.errors++;
         continue;
      }
      if (hex)
         disasm_hex(&st, argv[i], in);
      else
         disasm_bin(&st, argv[i], in);
      fclose(in);
   }
   return st.errors ? 1 : 0;
}
