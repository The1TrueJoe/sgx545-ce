/*
 * pvr: a minimal native client of the PVR services kernel driver.
 *
 * Built against the GPL headers in src/ with this driver's defines, so the
 * bridge structs are whatever the kernel itself was compiled with: on x86_64
 * that is the kernel's own native ABI, with no 32-bit compat layer involved.
 */
#ifndef PVR_H
#define PVR_H

#include <stddef.h>
#include <stdint.h>

#include "services.h"
#include "pvr_bridge.h"
#include "sgx_bridge.h"

struct pvr {
	int fd;
	IMG_HANDLE services;
	IMG_HANDLE devcookie;
	IMG_HANDLE memctx;
	PVRSRV_HEAP_INFO heaps[PVRSRV_MAX_CLIENT_HEAPS];
	IMG_UINT32 nheaps;
	int map_flags;		/* extra mmap flags for device memory: MAP_32BIT unless cleared */
};

struct pvr_buf {
	IMG_HANDLE mem;		/* kernel mem info handle */
	IMG_HANDLE sync;	/* kernel sync info handle */
	uint32_t devva;
	uint32_t size;
	void *cpu;		/* CPU mapping, or NULL */
};

/* One bridge call; returns the bridge's eError (first word of OUT) or -errno. */
int pvr_call(struct pvr *p, uint32_t id, void *in, size_t in_size, void *out, size_t out_size);

int pvr_open(struct pvr *p, const char *node);
int pvr_connect(struct pvr *p);
int pvr_acquire(struct pvr *p, uint32_t index, uint32_t type);
int pvr_memctx(struct pvr *p);
IMG_HANDLE pvr_heap(struct pvr *p, uint32_t heap_id);
int pvr_alloc(struct pvr *p, struct pvr_buf *b, uint32_t heap_id, uint32_t attribs, uint32_t size,
	      uint32_t align);
int pvr_map(struct pvr *p, struct pvr_buf *b);
int pvr_wrap(struct pvr *p, struct pvr_buf *b, void *cpu, uint32_t size);
int pvr_register_render_ctx(struct pvr *p, const void *ctx, uint32_t size, uint32_t pd_off,
			    IMG_HANDLE *handle, uint32_t *devva);
int pvr_kick(struct pvr *p, SGX_CCB_KICK *kick);

#endif
