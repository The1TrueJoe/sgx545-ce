/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */

/*
 * Host build of the command checker: user memory is host memory, and an
 * address the test marks unreadable (shim_fault) faults.
 */
#ifndef SHIM_LINUX_UACCESS_H
#define SHIM_LINUX_UACCESS_H

#include <linux/errno.h>
#include <stdint.h>
#include <string.h>

#include <linux/types.h>

#define __user
#define u64_to_user_ptr(x) ((void *)(uintptr_t)(x))

/* The one address the test makes unreadable. */
extern const void *shim_fault;
/* Run after each get_user(): the test's stand-in for a racing writer. */
extern void (*shim_after_get_user)(void);

static inline unsigned long copy_from_user(void *to, const void *from, unsigned long n)
{
	if (from == shim_fault)
		return n;
	memcpy(to, from, n);
	return 0;
}

static inline int shim_get_user_done(int err)
{
	if (shim_after_get_user)
		shim_after_get_user();
	return err;
}

#define get_user(x, ptr) \
	shim_get_user_done(copy_from_user(&(x), (ptr), sizeof(*(ptr))) ? -EFAULT : 0)

/* As the kernel's: zero-extend a short struct, refuse non-zero bytes past ksize. */
static inline int copy_struct_from_user(void *dst, size_t ksize, const void *src, size_t usize)
{
	size_t size = ksize < usize ? ksize : usize;

	if (usize > ksize) {
		const unsigned char *tail = (const unsigned char *)src + ksize;

		for (size_t i = 0; i < usize - ksize; i++)
			if (tail[i])
				return -E2BIG;
	}
	if (copy_from_user(dst, src, size))
		return -EFAULT;
	if (size < ksize)
		memset((char *)dst + size, 0, ksize - size);
	return 0;
}

#endif
