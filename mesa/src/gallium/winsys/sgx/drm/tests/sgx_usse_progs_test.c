/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 *
 * The fixed programs assemble to the words in usse_progs.txt.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sgx_usse_progs.h"

static const struct usse_prog *
find(const char *name)
{
   static const struct {
      const char *name;
      const struct usse_prog *p;
   } progs[] = {
      {"empty", &usse_empty},
      {"clear_vs", &usse_clear_vs},
      {"vpgen_vs", &usse_vpgen_vs},
      {"copy_fs", &usse_copy_fs},
      {"vs_xy", &usse_vs_xy},
      {"vs_xyz", &usse_vs_xyz},
      {"fs_uniform_sec", &usse_fs_uniform_sec},
      {"fs_uniform", &usse_fs_uniform},
      {"pe_ptoff", &usse_pe_ptoff},
      {"pe_emit", &usse_pe_emit},
   };
   unsigned n;

   if (sscanf(name, "state_emit_%u", &n) == 1 && n >= 1 && n <= 32)
      return usse_state_emit[n];
   for (unsigned i = 0; i < sizeof(progs) / sizeof(progs[0]); i++) {
      if (!strcmp(progs[i].name, name))
         return progs[i].p;
   }
   return NULL;
}

int
main(int argc, char **argv)
{
   FILE *f = argc > 1 ? fopen(argv[1], "r") : NULL;
   char line[128], name[64];
   unsigned i, lo, hi, checked = 0, bad = 0;

   if (!f || !sgx_usse_progs_init())
      return 1;
   while (fgets(line, sizeof(line), f)) {
      const struct usse_prog *p;
      char *dot;

      if (line[0] == '#' || sscanf(line, "%63s %x %x", name, &lo, &hi) != 3)
         continue;
      dot = strrchr(name, '.');
      if (!dot)
         continue;
      *dot = 0;
      i = (unsigned)atoi(dot + 1);
      p = find(name);
      checked++;
      if (!p || i >= p->n || p->w[2 * i] != lo || p->w[2 * i + 1] != hi) {
         printf("%s.%u: expected %08x %08x\n", name, i, lo, hi);
         bad++;
      }
   }
   printf("%u instructions, %u wrong\n", checked, bad);
   return bad || !checked;
}
