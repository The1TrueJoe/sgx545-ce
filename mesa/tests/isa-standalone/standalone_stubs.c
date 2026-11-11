/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/* Stand-ins for Mesa util pieces, for standalone.sh only. */
#include <stdarg.h>
#include <stdio.h>
void
mesa_log(int level, const char *tag, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
}
#include <stddef.h>
size_t
u_printf_length(const char *fmt, va_list untouched_args)
{
   va_list ap;
   va_copy(ap, untouched_args);
   int n = vsnprintf(NULL, 0, fmt, ap);
   va_end(ap);
   return n;
}
/* CPU detection, done and empty: half floats take the portable path (x86) */
#include "util/u_cpu_detect.h"
struct _util_cpu_caps_state_t _util_cpu_caps_state = {.detect_done = 1};
void
_util_cpu_detect_once(void)
{
}
