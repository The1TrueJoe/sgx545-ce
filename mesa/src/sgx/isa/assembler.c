/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * sgx-asm: assemble the USSE text notation (README.md), one instruction per
 * line.
 *
 *    sgx-asm [-c core] [-F fc] [-b | -L] [-o out] [file...]
 *
 * Reads the files (or stdin) in one pass and writes one instruction per input
 * instruction, in order:
 *    (default) "<lo> <hi>" in hex, one line each;
 *    -b        the words, low word first, little endian;
 *    -L        a listing, "<offset>: <lo> <hi>  <disassembly>".
 * Blank lines and lines starting with '#' are skipped; ';' starts a comment.
 * -c selects the core (545 or sgx545, the default); -F the format control at
 * the start (bit 0 C10, bit 1 F16; default 1, the program-start state). setfc
 * instructions change it for the lines after them. Errors go to stderr as
 * "<file>:<line>: <text>: <message>"; the exit status is 1 if there was one.
 */

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sgx_isa.h"
#include "sgx_isa_asm.h"

enum out_mode {
   OUT_HEX,
   OUT_BIN,
   OUT_LIST,
};

struct asm_state {
   struct sgx_isa_fc fc;
   enum out_mode mode;
   FILE *out;
   unsigned offset;
   unsigned errors;
};

static void
put_le32(FILE *out, uint32_t v)
{
   uint8_t b[4] = {v, v >> 8, v >> 16, v >> 24};

   fwrite(b, 1, sizeof(b), out);
}

static void
assemble_line(struct asm_state *st, const char *name, unsigned lineno,
              char *line)
{
   struct sgx_isa_instr I;
   char err[256];

   line[strcspn(line, "\r\n")] = '\0';
   char *text = line + strspn(line, " \t");
   if (!*text || *text == '#' || *text == ';')
      return;

   if (!sgx_isa_asm_line(text, &I, err, sizeof(err))) {
      fprintf(stderr, "%s:%u: %s: %s\n", name, lineno, text, err);
      st->errors++;
      return;
   }

   uint64_t w = sgx_isa_encode(&I, &st->fc);
   uint32_t lo = w, hi = w >> 32;

   switch (st->mode) {
   case OUT_HEX:
      fprintf(st->out, "%08x %08x\n", lo, hi);
      break;
   case OUT_BIN:
      put_le32(st->out, lo);
      put_le32(st->out, hi);
      break;
   case OUT_LIST: {
      char dis[512];

      sgx_disasm_one(w, &st->fc, dis, sizeof(dis));
      fprintf(st->out, "%06x: %08x %08x  %s\n", st->offset, lo, hi, dis);
      break;
   }
   }
   st->offset += 8;
   sgx_isa_setfc(w, &st->fc);
}

static void
assemble_file(struct asm_state *st, const char *name, FILE *in)
{
   char line[1024];
   unsigned lineno = 0;

   while (fgets(line, sizeof(line), in)) {
      lineno++;
      assemble_line(st, name, lineno, line);
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
   fprintf(stderr,
           "usage: sgx-asm [-c core] [-F fc] [-b | -L] [-o out] [file...]\n");
   exit(2);
}

int
main(int argc, char **argv)
{
   struct asm_state st = {
      .fc = SGX_ISA_FC_START,
      .mode = OUT_HEX,
   };
   const char *out_name = NULL;
   int opt;

   while ((opt = getopt(argc, argv, "c:F:bLo:h")) != -1) {
      switch (opt) {
      case 'c':
         st.fc.core = parse_core(optarg);
         if (!st.fc.core) {
            fprintf(stderr, "sgx-asm: core %s is not in sgx.xml\n", optarg);
            return 2;
         }
         break;
      case 'F': {
         unsigned fc = strtoul(optarg, NULL, 0);
         st.fc.c10 = fc & 1;
         st.fc.f16 = fc & 2;
         break;
      }
      case 'b':
         st.mode = OUT_BIN;
         break;
      case 'L':
         st.mode = OUT_LIST;
         break;
      case 'o':
         out_name = optarg;
         break;
      default:
         usage();
      }
   }

   st.out =
      out_name ? fopen(out_name, st.mode == OUT_BIN ? "wb" : "w") : stdout;
   if (!st.out) {
      fprintf(stderr, "sgx-asm: %s: %s\n", out_name, strerror(errno));
      return 2;
   }

   if (optind == argc)
      assemble_file(&st, "<stdin>", stdin);
   for (int i = optind; i < argc; i++) {
      FILE *in = fopen(argv[i], "r");

      if (!in) {
         fprintf(stderr, "sgx-asm: %s: %s\n", argv[i], strerror(errno));
         st.errors++;
         continue;
      }
      assemble_file(&st, argv[i], in);
      fclose(in);
   }

   if (fclose(st.out)) {
      fprintf(stderr, "sgx-asm: write error: %s\n", strerror(errno));
      return 2;
   }
   return st.errors ? 1 : 0;
}
