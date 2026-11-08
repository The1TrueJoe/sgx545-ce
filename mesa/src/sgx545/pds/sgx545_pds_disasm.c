/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * The disassembler: isaspec's generated decoder, data-store word numbers
 * printed as in docs/PDS.md (ds0[2], ds1[0x30]), and the program layout.
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "sgx545_pds.h"
#include "sgx545pds-isa.h"

static void
print_custom(struct isa_print_state *print, const char *name, uint64_t v)
{
   /* a MOVS row no select reads, only when it isn't 0 */
   if (!strcmp(name, "UR1") || !strcmp(name, "UR2")) {
      if (v)
         isa_print(print, " [r%c=", name[2]);
      else
         return;
   }
   /* numbers below 10 in decimal, the rest in hex (PDS.md "Text notation") */
   if (v < 10)
      isa_print(print, "%" PRIu64, v);
   else
      isa_print(print, "0x%" PRIx64, v);
   if (name[0] == 'U' && name[1] == 'R')
      isa_print(print, "]");
}

static void
tidy(char *s)
{
   size_t n = strlen(s);

   while (n && (s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t'))
      s[--n] = '\0';
   for (char *p = s; *p; p++) {
      if (*p == '\n' || *p == '\t')
         *p = ' ';
   }
}

bool
sgx545_pds_disasm_one(uint32_t word, char *buf, size_t size)
{
   char *text = NULL;
   size_t len = 0;
   FILE *out = open_memstream(&text, &len);

   if (!out)
      return false;
   sgx545pds_isa_disasm(&word, sizeof(word), out,
                        &(struct isa_decode_options){
                           .show_errors = true,
                           .field_print_cb = print_custom,
                        });
   fclose(out);
   tidy(text);
   bool ok = !strstr(text, "no match") && !strstr(text, "unexpected bits");
   snprintf(buf, size, "%s", text);
   free(text);
   return ok;
}

void
sgx545_pds_disasm(const uint32_t *code, unsigned n, FILE *out)
{
   char line[256];

   for (unsigned i = 0; i < n; i++) {
      sgx545_pds_disasm_one(code[i], line, sizeof(line));
      fprintf(out, "%3u: %08x  %s\n", i, code[i], line);
   }
}

void
sgx545_pds_disasm_program(const uint32_t *words, unsigned rows, unsigned n_code, FILE *out)
{
   for (unsigned n = 0; n < 2 * rows; n++) {
      fprintf(out, "ds0[%2u] = %08x   ds1[%2u] = %08x\n", n, words[sgx545_pds_ds0_at(n)], n,
              words[sgx545_pds_ds1_at(n)]);
   }
   sgx545_pds_disasm(words + 4 * rows, n_code, out);
}
