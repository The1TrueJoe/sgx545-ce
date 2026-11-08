/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/* memdump PHYS LEN: physical memory to stdout through an mmap of /dev/mem (the
 * EA's scanout, 0x7fc00000, stride 2944, is what simpledrm copies a frame to) */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
   unsigned long pa, len;
   int fd;
   void *p;

   if (argc != 3)
      return 2;
   pa = strtoul(argv[1], NULL, 0);
   len = strtoul(argv[2], NULL, 0);
   fd = open("/dev/mem", O_RDONLY | O_SYNC);
   p = fd < 0 ? MAP_FAILED : mmap(NULL, len, PROT_READ, MAP_SHARED, fd, pa);
   if (p == MAP_FAILED) {
      perror("mmap");
      return 1;
   }
   return fwrite(p, 1, len, stdout) == len ? 0 : 1;
}
