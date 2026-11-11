/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */

#ifndef USSE_FUZZ_SHIM_BITOPS_H
#define USSE_FUZZ_SHIM_BITOPS_H

#include <linux/types.h>

static inline s64 sign_extend64(u64 value, int index)
{
	int shift = 63 - index;

	return (s64)(value << shift) >> shift;
}

#endif
