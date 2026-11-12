/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 *
 * Whether something outside the program moves a thread's stack pointer: each
 * thread runs leaf calls in a loop and checks, in assembly, that rsp is the
 * same after every call as before it, and that the return addresses it pushed
 * come back. No GPU, no libraries past libc. Run it under the load that makes
 * Xorg crash (perf record -a -g ...) and see whether it trips.
 *
 *   rspcheck [threads [seconds]]   exit 0: rsp never moved; 1: it did
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static volatile int stop;

/* rsp before and after one call to a leaf that does nothing; 0 when equal */
static uint64_t
probe(void)
{
   uint64_t before, after;

   __asm__ volatile("mov %%rsp, %0\n\t"
                    "call 1f\n\t"
                    "jmp 2f\n"
                    "1:\n\t"
                    /* a leaf: some work, no stack use */
                    "mov $64, %%ecx\n"
                    "3:\n\t"
                    "dec %%ecx\n\t"
                    "jnz 3b\n\t"
                    "ret\n"
                    "2:\n\t"
                    "mov %%rsp, %1\n\t"
                    : "=&r"(before), "=&r"(after)
                    :
                    : "rcx", "cc", "memory");
   return before ^ after;
}

static void *
run(void *arg)
{
   unsigned long n = 0;

   (void)arg;
   while (!stop) {
      uint64_t d = probe();

      n++;
      if (d) {
         fprintf(stderr, "rspcheck: rsp moved by xor %#llx after %lu calls\n",
                 (unsigned long long)d, n);
         _exit(1);
      }
   }
   return (void *)n;
}

int
main(int argc, char **argv)
{
   int nt = argc > 1 ? atoi(argv[1]) : 4, secs = argc > 2 ? atoi(argv[2]) : 60;
   pthread_t t[64];
   unsigned long total = 0;

   if (nt < 1 || nt > 64)
      nt = 4;
   for (int i = 0; i < nt; i++)
      pthread_create(&t[i], NULL, run, NULL);
   sleep(secs);
   stop = 1;
   for (int i = 0; i < nt; i++) {
      void *r;

      pthread_join(t[i], &r);
      total += (unsigned long)r;
   }
   printf("rspcheck: %lu calls on %d threads in %d s, rsp never moved\n", total,
          nt, secs);
   return 0;
}
