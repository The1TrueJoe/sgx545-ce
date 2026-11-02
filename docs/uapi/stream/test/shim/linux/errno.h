/* SPDX-License-Identifier: MIT */
/* Host stand-in for the kernel header, for the stream test only. */
/* glibc includes <linux/errno.h> itself, so this one must not include <errno.h>. */
#ifndef EINVAL
#define EINVAL 22
#endif
