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
	int trace_fd;		/* pvrtrace-format log (tools/trace/pvrtrace.c), or -1 */
	unsigned kicks;
	unsigned trace_kicks;	/* snapshot only the first this many kicks (0: all) */
	struct { void *addr; size_t len; } maps[1024];	/* device memory mapped so far */
	unsigned nmaps;
};

struct pvr_buf {
	IMG_HANDLE mem;		/* kernel mem info handle */
	IMG_HANDLE sync;	/* kernel sync info handle */
	uint32_t devva;
	uint32_t size;
	void *cpu;		/* CPU mapping, or NULL */
	/* for pvr_free / pvr_unmap */
	void *mmap_base;	/* what mmap returned (cpu may be an offset into it) */
	size_t mmap_len;
	int wrapped;		/* WRAP_EXT_MEMORY, not ALLOC_DEVICEMEM */
	PVRSRV_CLIENT_MEM_INFO client_mem;
	PVRSRV_CLIENT_SYNC_INFO client_sync;
};

/* One bridge call; returns the bridge's eError (first word of OUT) or -errno. */
int pvr_call(struct pvr *p, uint32_t id, void *in, size_t in_size, void *out, size_t out_size);

int pvr_open(struct pvr *p, const char *node);
/* pvr_open on a node someone else opened (the fd is used, not dup'ed) */
void pvr_open_fd(struct pvr *p, int fd);
/* the whole bring-up a client needs: connect, acquire device 0, a memory context, the heaps */
int pvr_init(struct pvr *p);
int pvr_disconnect(struct pvr *p);
int pvr_connect(struct pvr *p);
int pvr_acquire(struct pvr *p, uint32_t index, uint32_t type);
int pvr_memctx(struct pvr *p);
IMG_HANDLE pvr_heap(struct pvr *p, uint32_t heap_id);
int pvr_alloc(struct pvr *p, struct pvr_buf *b, uint32_t heap_id, uint32_t attribs, uint32_t size,
	      uint32_t align);
int pvr_map(struct pvr *p, struct pvr_buf *b);
int pvr_wrap(struct pvr *p, struct pvr_buf *b, void *cpu, uint32_t size);
/* FREE_DEVICEMEM or UNWRAP_EXT_MEMORY, and the CPU mapping */
void pvr_free(struct pvr *p, struct pvr_buf *b);
/* drop b's CPU mapping only */
void pvr_unmap(struct pvr *p, struct pvr_buf *b);
/* map the PVRSRV_SYNC_DATA of a kernel sync info handle; keep holds the mapping */
volatile PVRSRV_SYNC_DATA *pvr_sync_data(struct pvr *p, IMG_HANDLE sync, struct pvr_buf *keep);
int pvr_register_render_ctx(struct pvr *p, const void *ctx, uint32_t size, uint32_t pd_off,
			    IMG_HANDLE *handle, uint32_t *devva);
int pvr_kick(struct pvr *p, SGX_CCB_KICK *kick);
int pvr_heapinfo(struct pvr *p);
/* Tell the microkernel to drop its cached state for an RT data set (the DDK's
 * SGXRemoveRenderTarget does this). A client that skips it leaves the next
 * client with a different memory layout a stale pointer, and its TA hangs. */
int pvr_flush_rt(struct pvr *p, uint32_t rtdata);
int pvr_unregister_render_ctx(struct pvr *p, IMG_HANDLE ctx);

/*
 * Write a pvrtrace log (the format of tools/trace/pvrtrace.c, x86_64 layouts) of
 * this client: every bridge call, every device-memory mmap, the render-context
 * buffer, and a snapshot of all mapped device memory before each kick. The
 * decoders in tools/decode, tools/state and tools/pds read it like a DDK capture.
 */
int pvr_trace_open(struct pvr *p, const char *path);
void pvr_trace_close(struct pvr *p);

#endif
