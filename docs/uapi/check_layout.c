// SPDX-License-Identifier: MIT
// Copyright © 2026 Joseph Telaak

/*
 * Static proof that powervr_sgx_drm.h has one layout. layout.inc holds literal
 * offsets and sizes, so this compiling on i386 and on x86_64 means the two layouts
 * are equal. -Wpadded (check.sh) separately refuses any implicit padding.
 *
 * Compiled only. Built with -DCHECK_NEGATIVE it must fail: that shows the asserts
 * catch the classic i386 trap of a __u64 after a lone __u32.
 */

#include <stddef.h>

#include "powervr_sgx_drm.h"

/* Natural alignment of a member, regardless of the target's ABI quirk for __u64. */
#define NATURAL(off, size) ((off) % (size) == 0)

/* A struct: its size, and its 64-bit padding. */
#define STRUCT(s, sz)                                                     \
	_Static_assert(sizeof(struct s) == (sz), #s ": size");            \
	_Static_assert((sz) % 8 == 0, #s ": not padded to 64 bits");

/* A member: its offset and width, and that it is naturally aligned. */
#define FIELD(s, m, off, sz)                                              \
	_Static_assert(offsetof(struct s, m) == (off), #s "." #m ": offset"); \
	_Static_assert(sizeof(((struct s *)0)->m) == (sz), #s "." #m ": size"); \
	_Static_assert(NATURAL(off, (sz) > 8 ? 8 : (sz)), #s "." #m ": alignment");

#include "layout.inc"

#undef STRUCT
#undef FIELD

/*
 * The byte total of all members over all structs equals the total of all sizes.
 * gen_table.py checks the same per struct: each member ends where the next starts.
 */
#define STRUCT(s, sz)
#define FIELD(s, m, off, sz) (sz) +

enum {
	check_member_bytes =
#include "layout.inc"
	0
};

#undef STRUCT
#undef FIELD
#define STRUCT(s, sz) (sz) +
#define FIELD(s, m, off, sz)

enum {
	check_struct_bytes =
#include "layout.inc"
	0
};

#undef STRUCT
#undef FIELD

_Static_assert(check_member_bytes == check_struct_bytes,
	       "a struct has a member layout.inc does not list, or a hole");

/*
 * Ioctl numbers: _IOC(dir, 'd', 0x40 + n, size). Equal on both targets, so a
 * 32-bit process calls the same ioctl as a 64-bit one and no compat handler exists.
 */
#define IOW_NR(n, sz)  (0x40000000u | ((unsigned)(sz) << 16) | 0x6400u | (0x40u + (n)))
#define IOWR_NR(n, sz) (0xc0000000u | ((unsigned)(sz) << 16) | 0x6400u | (0x40u + (n)))

_Static_assert(DRM_IOCTL_POWERVR_SGX_DEV_QUERY == IOWR_NR(0x00, 16), "DEV_QUERY");
_Static_assert(DRM_IOCTL_POWERVR_SGX_CREATE_BO == IOWR_NR(0x01, 24), "CREATE_BO");
_Static_assert(DRM_IOCTL_POWERVR_SGX_GET_BO_MMAP_OFFSET == IOWR_NR(0x02, 16), "GET_BO_MMAP_OFFSET");
_Static_assert(DRM_IOCTL_POWERVR_SGX_CREATE_VM_CONTEXT == IOWR_NR(0x03, 8), "CREATE_VM_CONTEXT");
_Static_assert(DRM_IOCTL_POWERVR_SGX_DESTROY_VM_CONTEXT == IOW_NR(0x04, 8), "DESTROY_VM_CONTEXT");
_Static_assert(DRM_IOCTL_POWERVR_SGX_VM_BIND == IOW_NR(0x05, 40), "VM_BIND");
_Static_assert(DRM_IOCTL_POWERVR_SGX_CREATE_CONTEXT == IOWR_NR(0x06, 24), "CREATE_CONTEXT");
_Static_assert(DRM_IOCTL_POWERVR_SGX_DESTROY_CONTEXT == IOW_NR(0x07, 8), "DESTROY_CONTEXT");
_Static_assert(DRM_IOCTL_POWERVR_SGX_GET_CONTEXT_STATE == IOWR_NR(0x08, 16), "GET_CONTEXT_STATE");
_Static_assert(DRM_IOCTL_POWERVR_SGX_CREATE_RT_DATASET == IOWR_NR(0x09, 24), "CREATE_RT_DATASET");
_Static_assert(DRM_IOCTL_POWERVR_SGX_DESTROY_RT_DATASET == IOW_NR(0x0a, 8), "DESTROY_RT_DATASET");
/* 0x0b (revision 2's SUBMIT_JOBS) and 0x0d (revision 1's UPLOAD_CODE) are retired. */
_Static_assert(DRM_IOCTL_POWERVR_SGX_UPLOAD_CODE == IOWR_NR(0x0c, 40), "UPLOAD_CODE");
_Static_assert(DRM_IOCTL_POWERVR_SGX_SUBMIT_JOBS == IOWR_NR(0x0e, 24), "SUBMIT_JOBS");

#ifdef CHECK_NEGATIVE
/* 16 bytes with an implicit hole on x86_64, 12 on i386: the assert must fire. */
struct check_i386_trap {
	__u32 a;
	__u64 b;
};
_Static_assert(sizeof(struct check_i386_trap) == 16, "i386 packs a __u64 after a __u32");
_Static_assert(offsetof(struct check_i386_trap, b) == 8, "i386 packs a __u64 after a __u32");
#endif

/* Reference the ABI so the translation unit is not empty. */
extern const unsigned long powervr_sgx_uapi_struct_bytes;
const unsigned long powervr_sgx_uapi_struct_bytes = check_struct_bytes;
