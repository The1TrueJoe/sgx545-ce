/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */

/* Host build of the command checker: the uAPI's and the kernel's integer types. */
#ifndef SHIM_LINUX_TYPES_H
#define SHIM_LINUX_TYPES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int32_t __s32;
typedef int64_t __s64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
#endif
