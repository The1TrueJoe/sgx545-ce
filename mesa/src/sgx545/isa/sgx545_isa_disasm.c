/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * The disassembler: isaspec's generated decoder plus what the XML leaves
 * to C:
 *  - "custom" fields, printed only when they differ from their default;
 *  - format control, tracked through setfc and handed to the XML's
 *    expressions as ISA_GPU_ID() (bit 0 = C10 enable, bit 1 = F16 enable);
 *  - tidying the trailing "[...]" (empty brackets, leading spaces).
 */

#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "sgx545-isa.h"
#include "sgx545_isa.h"

static void
print_custom(struct isa_print_state *print, const char *name, uint64_t v)
{
   /* Numbers print in decimal (ISA.md "Text notation and defaults"). */
   if (!strcmp(name, "NODST")) {
      if (v != 0x280)
         isa_print(print, " nodst=%" PRIu64, v);
   } else if (!strcmp(name, "UNUSED2")) {
      if (v != 0x300)
         isa_print(print, " unused2=%" PRIu64, v);
   } else if (!strcmp(name, "IMM16") || !strcmp(name, "LIMM")) {
      isa_print(print, "#%" PRIu64, v);
   } else if (!strncmp(name, "SL", 2)) {
      /* smlsi operand: bit 8 = swizzle, else a signed 8-bit increment */
      if (v & 0x100) {
         isa_print(print, "#0.");
         for (unsigned i = 0; i < 4; i++)
            isa_print(print, "%c", "xyzw"[(v >> (2 * i)) & 3]);
      } else {
         isa_print(print, "#%d", (int)(int8_t)(v & 0xff));
      }
   } else if (v) {
      static const struct {
         const char *field, *text;
      } names[] = {
         {"ROT", "rot"},   {"PATH", "path"}, {"EN", "n"},     {"AM0", "am0"},
         {"AM1", "am1"},   {"AM2", "am2"},   {"AM3", "am3"},  {"LIM0", "lim0"},
         {"LIM1", "lim1"}, {"LIM2", "lim2"}, {"EIMM", "imm"},
      };
      for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
         if (!strcmp(name, names[i].field)) {
            isa_print(print, " %s=%" PRIu64, names[i].text, v);
            return;
         }
      }
      isa_print(print, " ");
      for (const char *c = name; *c; c++)
         isa_print(print, "%c", tolower((unsigned char)*c));
      isa_print(print, "=%" PRIu64, v);
   }
}

/* setfc: hw 1f, MOE control (h20-22 = 1), op 5; a0 = l0, a1 = l8. */
static bool
is_setfc(uint64_t w, struct sgx545_isa_fc *fc)
{
   uint32_t hi = w >> 32;

   if ((hi >> 27) != 0x1f || ((hi >> 20) & 7) != 1 || ((hi >> 24) & 7) != 5)
      return false;
   fc->f16 = w & 1;
   fc->c10 = (w >> 8) & 1;
   return true;
}

/*
 * The XML separates bracket items with spaces whether or not they print:
 * collapse the runs, trim inside the brackets, drop an empty " []".
 */
static void
tidy(char *s)
{
   char *d = s, *e;

   for (char *p = s; *p; p++) {
      if (*p == '\n' || *p == '\t')
         *p = ' ';
   }
   e = s + strlen(s);
   while (e > s && e[-1] == ' ')
      *--e = '\0';

   char *lb = strrchr(s, '[');
   if (!lb || e == s || e[-1] != ']' || (lb > s && lb[-1] != ' '))
      return;

   d = lb + 1;
   bool space = false;
   for (char *p = lb + 1; *p; p++) {
      if (*p == ' ') {
         space = true;
         continue;
      }
      if (space && d > lb + 1 && *p != ']')
         *d++ = ' ';
      space = false;
      *d++ = *p;
   }
   *d = '\0';
   if (!strcmp(lb, "[]")) {
      lb[-1] = '\0';
   }
}

bool
sgx545_disasm_one(uint64_t word, const struct sgx545_isa_fc *fc, char *buf,
                  size_t size)
{
   struct sgx545_isa_fc f = fc ? *fc : SGX545_ISA_FC_START;
   uint32_t words[2] = {(uint32_t)word, (uint32_t)(word >> 32)};
   char *text = NULL;
   size_t len = 0;
   FILE *out = open_memstream(&text, &len);

   if (!out)
      return false;

   sgx545_isa_disasm(words, sizeof(words), out,
                     &(struct isa_decode_options){
                        .gpu_id = (f.c10 ? 1 : 0) | (f.f16 ? 2 : 0),
                        .show_errors = true,
                        .field_print_cb = print_custom,
                     });
   fclose(out);

   tidy(text);
   bool ok = !strstr(text, "no match") && !strstr(text, "unexpected bits") &&
             !strstr(text, "dontcare");
   snprintf(buf, size, "%s", text);
   free(text);
   return ok;
}

void
sgx545_disasm(const uint32_t *words, unsigned n, FILE *out,
              const struct sgx545_isa_fc *fc)
{
   struct sgx545_isa_fc f = fc ? *fc : SGX545_ISA_FC_START;
   char line[512];

   for (unsigned i = 0; i < n; i++) {
      uint64_t w = words[2 * i] | (uint64_t)words[2 * i + 1] << 32;

      sgx545_disasm_one(w, &f, line, sizeof(line));
      fprintf(out, "%08x %08x  %s\n", words[2 * i], words[2 * i + 1], line);
      is_setfc(w, &f);
   }
}
