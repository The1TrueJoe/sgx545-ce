/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */

#ifndef USSE_FUZZ_SHIM_ARRAY_SIZE_H
#define USSE_FUZZ_SHIM_ARRAY_SIZE_H

#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))

#endif
