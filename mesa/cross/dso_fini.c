/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */

/*
 * What crtbeginS.o gives a shared library and zig cc's link leaves out: a
 * __dso_handle of its own and a call to __cxa_finalize when it is unloaded.
 * Without them a dlclose()d library's atexit() handlers stay registered and
 * exit() calls into the unmapped code (libgbm unloading dri_gbm.so, whose
 * util code registered atexit handlers). Linked into every -shared output by
 * zigcc.
 */

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((visibility("hidden"))) void *__dso_handle = &__dso_handle;

extern void __cxa_finalize(void *) __attribute__((weak));

__attribute__((destructor)) static void
dso_fini(void)
{
   if (__cxa_finalize)
      __cxa_finalize(__dso_handle);
}

#ifdef __cplusplus
}
#endif
