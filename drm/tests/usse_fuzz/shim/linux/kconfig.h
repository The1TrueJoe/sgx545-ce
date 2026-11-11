/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */

#ifndef USSE_FUZZ_SHIM_KCONFIG_H
#define USSE_FUZZ_SHIM_KCONFIG_H

/* No kernel options on the host: the KUnit include stays out. */
#define IS_ENABLED(option)	0

#endif
