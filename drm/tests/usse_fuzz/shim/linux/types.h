/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */

/* Just enough of the kernel's headers to build pvrsgx_usse.c on the host. */
#ifndef USSE_FUZZ_SHIM_TYPES_H
#define USSE_FUZZ_SHIM_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;

#endif
