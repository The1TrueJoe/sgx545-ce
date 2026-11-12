/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */

/* Host build of the command checker: the kernel's one-argument static_assert. */
#ifndef SHIM_LINUX_BUILD_BUG_H
#define SHIM_LINUX_BUILD_BUG_H
#undef static_assert
#define static_assert(expr) _Static_assert(expr, #expr)
#endif
