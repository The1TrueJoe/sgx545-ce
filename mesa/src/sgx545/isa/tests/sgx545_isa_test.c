/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * Round trip for the isaspec encoder and disassembler: each line of the
 * input is an instruction in ISA.md's text notation, optionally followed
 * by "; <lo> <hi>", the expected words. The line is assembled, encoded,
 * compared with the expected words, disassembled, and the text compared
 * with the line. A line "<a> == <b>" checks another spelling instead: a and
 * b must assemble to the same words. setfc lines update the format-control
 * state the later lines are encoded and decoded with.
 *
 *    sgx545_isa_test [-t] [-f N] file...
 *
 * -t prints the encoding table ("<text> ; <lo> <hi>") on stdout.
 * -f N decodes N random words (with each format-control state); every word
 *      that decodes cleanly must assemble back to itself.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sgx545_isa.h"
#include "sgx545_isa_asm.h"

static char *
trim(char *s)
{
   while (*s == ' ' || *s == '\t')
      s++;
   char *e = s + strlen(s);
   while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
      *--e = '\0';
   return s;
}

/* a == b up to runs of whitespace (ISA.md allows any whitespace before "[") */
static bool
same_text(const char *a, const char *b)
{
   for (;;) {
      bool sa = *a == ' ' || *a == '\t', sb = *b == ' ' || *b == '\t';
      if (sa != sb)
         return false;
      while (*a == ' ' || *a == '\t')
         a++;
      while (*b == ' ' || *b == '\t')
         b++;
      if (*a != *b)
         return false;
      if (!*a)
         return true;
      a++;
      b++;
   }
}

static unsigned
run_file(const char *path, bool table, unsigned *count)
{
   FILE *f = fopen(path, "r");
   char line[512];
   unsigned fails = 0, lineno = 0;
   struct sgx545_isa_fc fc = SGX545_ISA_FC_START;

   if (!f) {
      perror(path);
      return 1;
   }

   while (fgets(line, sizeof(line), f)) {
      lineno++;
      char *text = trim(line);
      if (!*text || *text == '#') {
         if (table)
            printf("%s\n", text);
         continue;
      }

      char *semi = strchr(text, ';');
      uint32_t want[2];
      bool have_want = false;
      if (semi) {
         *semi = '\0';
         have_want = sscanf(semi + 1, "%x %x", &want[0], &want[1]) == 2;
         text = trim(text);
      }

      struct sgx_isa_instr I;
      char err[256], dis[512];
      char *eq = strstr(text, " == ");
      if (eq) {
         struct sgx_isa_instr J;
         *eq = '\0';
         char *alt = trim(text), *canon = trim(eq + 4);
         if (!sgx545_isa_asm_line(alt, &I, err, sizeof(err)) ||
             !sgx545_isa_asm_line(canon, &J, err, sizeof(err))) {
            fprintf(stderr, "%s:%u: %s == %s: %s\n", path, lineno, alt, canon, err);
            fails++;
            continue;
         }
         (*count)++;
         if (sgx545_isa_encode(&I, &fc) != sgx545_isa_encode(&J, &fc)) {
            fprintf(stderr, "%s:%u: %s and %s encode differently\n", path, lineno, alt, canon);
            fails++;
         }
         continue;
      }
      if (!sgx545_isa_asm_line(text, &I, err, sizeof(err))) {
         fprintf(stderr, "%s:%u: %s: %s\n", path, lineno, text, err);
         fails++;
         continue;
      }
      (*count)++;

      uint64_t w = sgx545_isa_encode(&I, &fc);
      uint32_t lo = w, hi = w >> 32;
      bool ok = true;

      if (have_want && (lo != want[0] || hi != want[1])) {
         fprintf(stderr, "%s:%u: %s: encoded %08x %08x, expected %08x %08x\n",
                 path, lineno, text, lo, hi, want[0], want[1]);
         ok = false;
      }
      if (!sgx545_disasm_one(w, &fc, dis, sizeof(dis)) || !same_text(dis, text)) {
         fprintf(stderr, "%s:%u: %s: %08x %08x disassembles as: %s\n",
                 path, lineno, text, lo, hi, dis);
         ok = false;
      }
      if (!ok)
         fails++;
      if (table)
         printf("%-60s ; %08x %08x\n", text, lo, hi);

      if (I.opc == SGX_ISA_OPC_SETFC) {
         fc.f16 = I.a[0];
         fc.c10 = I.a[1];
      }
   }
   fclose(f);
   return fails;
}

static uint64_t
rnd64(uint64_t *x)
{
   /* xorshift64* */
   *x ^= *x >> 12;
   *x ^= *x << 25;
   *x ^= *x >> 27;
   return *x * 0x2545f4914f6cdd1dull;
}

static unsigned
fuzz(unsigned n, unsigned *count)
{
   uint64_t seed = 0x5a5a1234deadbeefull;
   unsigned fails = 0, decoded = 0;

   for (unsigned i = 0; i < n; i++) {
      uint64_t w = rnd64(&seed);
      /* bias towards valid words: clear a random set of low-probability bits;
       * every other word gets a random class, so rare classes are reached */
      w &= rnd64(&seed) | rnd64(&seed);
      if (i & 4)
         w = (w & ~(0x1full << 59)) | (rnd64(&seed) & 0x1full) << 59;
      struct sgx545_isa_fc fc = { .c10 = i & 1, .f16 = (i >> 1) & 1 };
      char dis[512], err[256], dis2[512];
      struct sgx_isa_instr I;

      if (!sgx545_disasm_one(w, &fc, dis, sizeof(dis)))
         continue;
      decoded++;
      if (!sgx545_isa_asm_line(dis, &I, err, sizeof(err))) {
         if (fails++ < 40)
            fprintf(stderr, "fuzz: %08x %08x: %s: %s\n", (uint32_t)w,
                    (uint32_t)(w >> 32), dis, err);
         continue;
      }
      uint64_t w2 = sgx545_isa_encode(&I, &fc);
      if (w2 != w) {
         sgx545_disasm_one(w2, &fc, dis2, sizeof(dis2));
         if (fails++ < 40)
            fprintf(stderr, "fuzz: %08x %08x: %s -> %08x %08x %s\n",
                    (uint32_t)w, (uint32_t)(w >> 32), dis, (uint32_t)w2,
                    (uint32_t)(w2 >> 32), dis2);
      }
   }
   *count += decoded;
   fprintf(stderr, "fuzz: %u of %u random words decoded\n", decoded, n);
   return fails;
}

int
main(int argc, char **argv)
{
   bool table = false;
   unsigned fails = 0, count = 0;

   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "-t")) {
         table = true;
         continue;
      }
      if (!strcmp(argv[i], "-f") && i + 1 < argc) {
         fails += fuzz(atoi(argv[++i]), &count);
         continue;
      }
      fails += run_file(argv[i], table, &count);
   }
   fprintf(stderr, "%u instructions, %u failures\n", count, fails);
   return fails ? 1 : 0;
}
