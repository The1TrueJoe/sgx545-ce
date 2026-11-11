/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 *
 * LD_PRELOAD=segvtrace.so: on SIGSEGV, SIGBUS, SIGFPE or SIGABRT, each frame
 * of the faulting thread as "object+offset" on stderr, for llvm-symbolizer
 * against the same objects with their debug info (crash-sym.sh), then the
 * signal's default action.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void
handler(int sig, siginfo_t *si, void *uc)
{
   void *pcs[64];
   int n = backtrace(pcs, 64);
   char line[512];

   (void)uc;
   snprintf(line, sizeof(line), "segvtrace: signal %d at %p\n", sig, si->si_addr);
   write(2, line, strlen(line));
   for (int i = 0; i < n; i++) {
      Dl_info d;

      if (dladdr(pcs[i], &d) && d.dli_fname)
         snprintf(line, sizeof(line), "segvtrace: #%d %s+0x%lx %s\n", i,
                  d.dli_fname,
                  (unsigned long)((char *)pcs[i] - (char *)d.dli_fbase),
                  d.dli_sname ? d.dli_sname : "");
      else
         snprintf(line, sizeof(line), "segvtrace: #%d %p\n", i, pcs[i]);
      write(2, line, strlen(line));
   }
   signal(sig, SIG_DFL);
   raise(sig);
}

__attribute__((constructor)) static void
init(void)
{
   struct sigaction sa;

   memset(&sa, 0, sizeof(sa));
   sa.sa_sigaction = handler;
   sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
   sigaction(SIGSEGV, &sa, NULL);
   sigaction(SIGBUS, &sa, NULL);
   sigaction(SIGFPE, &sa, NULL);
   sigaction(SIGABRT, &sa, NULL);
}
