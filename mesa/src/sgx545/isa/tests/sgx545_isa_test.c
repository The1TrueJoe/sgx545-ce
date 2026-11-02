/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * Round trip for the isaspec encoder and disassembler: each line of the
 * input is an instruction in ISA.md's text notation, optionally followed
 * by "; <lo> <hi>", the expected words. The line is assembled, encoded,
 * compared with the expected words, disassembled, and the text compared
 * with the line. setfc lines update the format-control state the later
 * lines are encoded and decoded with.
 *
 *    sgx545_isa_test [-t] file...
 *
 * -t prints the encoding table ("<text> ; <lo> <hi>") on stdout.
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
      if (!sgx545_disasm_one(w, &fc, dis, sizeof(dis)) || strcmp(dis, text)) {
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
      fails += run_file(argv[i], table, &count);
   }
   fprintf(stderr, "%u instructions, %u failures\n", count, fails);
   return fails ? 1 : 0;
}
