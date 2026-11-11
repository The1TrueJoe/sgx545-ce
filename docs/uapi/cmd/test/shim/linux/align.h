/* SPDX-License-Identifier: MIT */
/* Host stand-in for the kernel header, for the stream test only. */
#define ALIGN(x, a) (((x) + (a) - 1) & ~((__typeof__(x))(a) - 1))
