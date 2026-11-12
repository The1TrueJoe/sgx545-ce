/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */

#ifndef USSE_FUZZ_SHIM_BITS_H
#define USSE_FUZZ_SHIM_BITS_H

#define BIT(n)			(1UL << (n))
#define BIT_ULL(n)		(1ULL << (n))
#define GENMASK(h, l)		((~0UL >> (sizeof(long) * 8 - 1 - (h))) & (~0UL << (l)))
#define GENMASK_ULL(h, l)	((~0ULL >> (63 - (h))) & (~0ULL << (l)))

#endif
