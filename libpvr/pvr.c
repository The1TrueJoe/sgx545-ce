#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "pvr.h"

/* pvr_drm.h: DRM_IOW(DRM_COMMAND_BASE + PVR_DRM_SRVKM_CMD, PVRSRV_BRIDGE_PACKAGE) */
#define DRM_IOCTL_PVR_SRVKM _IOW('d', 0x40 + 0x12, PVRSRV_BRIDGE_PACKAGE)

/* pvrtrace records (tools/trace/pvrtrace.c; read by tools/decode/pvrlog.py) */
enum { REC_BRIDGE = 1, REC_MMAP = 2, REC_SNAP = 4, REC_UBUF = 6 };

static void trace_put(struct pvr *p, const void *a, size_t n)
{
	const char *s = a;

	while (n) {
		ssize_t w = write(p->trace_fd, s, n);

		if (w <= 0)
			return;
		s += w;
		n -= (size_t)w;
	}
}

static void trace_rec(struct pvr *p, uint32_t type, size_t len)
{
	uint32_t h[2] = { type, (uint32_t)len };

	trace_put(p, h, sizeof h);
}

int pvr_trace_open(struct pvr *p, const char *path)
{
	/* "PVRT", version 2 (bridge records carry the services handle), pointer size */
	uint32_t h[3] = { 0x54525650, 2, sizeof(void *) };

	p->trace_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (p->trace_fd < 0)
		return -errno;
	trace_put(p, h, sizeof h);
	return 0;
}

void pvr_trace_close(struct pvr *p)
{
	if (p->trace_fd >= 0)
		close(p->trace_fd);
	p->trace_fd = -1;
}

static void trace_bridge(struct pvr *p, uint32_t id, int ret, int err, const void *in, size_t in_size,
			 const void *out, size_t out_size)
{
	struct timespec ts;
	uint32_t h[5] = { id, (uint32_t)ret, (uint32_t)err, (uint32_t)in_size, (uint32_t)out_size };
	uint64_t t, svc = (uint64_t)(uintptr_t)p->services;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	t = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
	trace_rec(p, REC_BRIDGE, sizeof h + 16 + in_size + out_size);
	trace_put(p, h, sizeof h);
	trace_put(p, &t, 8);
	trace_put(p, &svc, 8);
	trace_put(p, in, in_size);
	trace_put(p, out, out_size);
}

static void trace_mmap(struct pvr *p, void *addr, size_t len, int prot, int flags, uint64_t off)
{
	uint64_t r[5] = { (uintptr_t)addr, len, (uint32_t)prot | (uint64_t)(uint32_t)flags << 32, off,
			  (uintptr_t)addr };

	trace_rec(p, REC_MMAP, sizeof r);
	trace_put(p, r, sizeof r);
}

static void trace_ubuf(struct pvr *p, uint32_t nr, const void *ptr, size_t len)
{
	uint32_t h[2] = { nr, 0 };
	uint64_t a = (uintptr_t)ptr;

	trace_rec(p, REC_UBUF, sizeof h + sizeof a + len);
	trace_put(p, h, sizeof h);
	trace_put(p, &a, sizeof a);
	trace_put(p, ptr, len);
}

/* a copy of every mapping, as pvrtrace takes before each kick */
static void trace_snap(struct pvr *p, uint32_t kick)
{
	for (unsigned i = 0; i < p->nmaps; i++) {
		uint32_t h[2] = { 1, kick };
		uint64_t a[2] = { (uintptr_t)p->maps[i].addr, p->maps[i].len };

		trace_rec(p, REC_SNAP, sizeof h + sizeof a + p->maps[i].len);
		trace_put(p, h, sizeof h);
		trace_put(p, a, sizeof a);
		trace_put(p, p->maps[i].addr, p->maps[i].len);
	}
}

int pvr_call(struct pvr *p, uint32_t id, void *in, size_t in_size, void *out, size_t out_size)
{
	PVRSRV_BRIDGE_PACKAGE pkg = {
		.ui32BridgeID = id,
		.ui32Size = sizeof pkg,
		.pvParamIn = in,
		.ui32InBufferSize = (IMG_UINT32)in_size,
		.pvParamOut = out,
		.ui32OutBufferSize = (IMG_UINT32)out_size,
		.hKernelServices = p->services,
	};

	int r, e;

	do
		r = ioctl(p->fd, DRM_IOCTL_PVR_SRVKM, &pkg);
	while (r < 0 && (errno == EINTR || errno == EAGAIN));
	e = r < 0 ? errno : 0;

	if (p->trace_fd >= 0)
		trace_bridge(p, id, r, e, in, in_size, out, out_size);
	if (r < 0)
		return -e;
	return out_size >= 4 ? (int)*(PVRSRV_ERROR *)out : 0;
}

void pvr_open_fd(struct pvr *p, int fd)
{
	memset(p, 0, sizeof *p);
#ifdef MAP_32BIT
	p->map_flags = MAP_32BIT;
#endif
	p->trace_fd = -1;
	p->fd = fd;
}

int pvr_open(struct pvr *p, const char *node)
{
	pvr_open_fd(p, open(node ? node : "/dev/dri/card0", O_RDWR | O_CLOEXEC));
	return p->fd < 0 ? -errno : 0;
}

int pvr_init(struct pvr *p)
{
	int e;

	if ((e = pvr_connect(p)) || (e = pvr_acquire(p, 0, 0)) || (e = pvr_memctx(p)) || (e = pvr_heapinfo(p)))
		return e;
	return 0;
}

/* resman frees what is left when services disconnect; the call has no IN */
int pvr_disconnect(struct pvr *p)
{
	PVRSRV_BRIDGE_RETURN out = { 0 };

	return pvr_call(p, PVRSRV_BRIDGE_DISCONNECT_SERVICES, NULL, 0, &out, sizeof out);
}

int pvr_connect(struct pvr *p)
{
	PVRSRV_BRIDGE_IN_CONNECT_SERVICES in = { 0 };
	PVRSRV_BRIDGE_OUT_CONNECT_SERVICES out = { 0 };
	int e = pvr_call(p, PVRSRV_BRIDGE_CONNECT_SERVICES, &in, sizeof in, &out, sizeof out);

	if (!e)
		p->services = out.hKernelServices;
	return e;
}

int pvr_acquire(struct pvr *p, uint32_t index, uint32_t type)
{
	PVRSRV_BRIDGE_IN_ACQUIRE_DEVICEINFO in = { .uiDevIndex = index,
						   .eDeviceType = (PVRSRV_DEVICE_TYPE)type };
	PVRSRV_BRIDGE_OUT_ACQUIRE_DEVICEINFO out = { 0 };
	int e = pvr_call(p, PVRSRV_BRIDGE_ACQUIRE_DEVICEINFO, &in, sizeof in, &out, sizeof out);

	if (!e)
		p->devcookie = out.hDevCookie;
	return e;
}

int pvr_memctx(struct pvr *p)
{
	PVRSRV_BRIDGE_IN_CREATE_DEVMEMCONTEXT in = { .hDevCookie = p->devcookie };
	PVRSRV_BRIDGE_OUT_CREATE_DEVMEMCONTEXT out;
	int e;

	memset(&out, 0, sizeof out);
	e = pvr_call(p, PVRSRV_BRIDGE_CREATE_DEVMEMCONTEXT, &in, sizeof in, &out, sizeof out);
	if (e)
		return e;
	p->memctx = out.hDevMemContext;
	p->nheaps = out.ui32ClientHeapCount;
	memcpy(p->heaps, out.sHeapInfo, sizeof p->heaps);
	return 0;
}

/* Heaps are looked up by the low byte of ui32HeapID (SGX_*_HEAP_ID). */
IMG_HANDLE pvr_heap(struct pvr *p, uint32_t heap_id)
{
	for (IMG_UINT32 i = 0; i < p->nheaps; i++)
		if ((p->heaps[i].ui32HeapID & 0xff) == (heap_id & 0xff))
			return p->heaps[i].hDevMemHeap;
	return NULL;
}

int pvr_alloc(struct pvr *p, struct pvr_buf *b, uint32_t heap_id, uint32_t attribs, uint32_t size,
	      uint32_t align)
{
	PVRSRV_BRIDGE_IN_ALLOCDEVICEMEM in = {
		.hDevCookie = p->devcookie,
		.hDevMemHeap = pvr_heap(p, heap_id),
		.ui32Attribs = attribs,
		.ui32Size = size,
		.ui32Alignment = align,
	};
	PVRSRV_BRIDGE_OUT_ALLOCDEVICEMEM out;
	int e;

	memset(&out, 0, sizeof out);
	memset(b, 0, sizeof *b);
	if (!in.hDevMemHeap)
		return -ENOENT;
	e = pvr_call(p, PVRSRV_BRIDGE_ALLOC_DEVICEMEM, &in, sizeof in, &out, sizeof out);
	if (e)
		return e;
	b->mem = out.sClientMemInfo.hKernelMemInfo;
	b->sync = out.sClientSyncInfo.hKernelSyncInfo;
	b->devva = out.sClientMemInfo.sDevVAddr.uiAddr;
	b->size = (uint32_t)out.sClientMemInfo.uAllocSize;
	b->client_mem = out.sClientMemInfo;
	b->client_sync = out.sClientSyncInfo;
	return 0;
}

int pvr_map(struct pvr *p, struct pvr_buf *b)
{
	PVRSRV_BRIDGE_IN_MHANDLE_TO_MMAP_DATA in = { .hMHandle = b->mem };
	PVRSRV_BRIDGE_OUT_MHANDLE_TO_MMAP_DATA out = { 0 };
	void *m;
	int e = pvr_call(p, PVRSRV_BRIDGE_MHANDLE_TO_MMAP_DATA, &in, sizeof in, &out, sizeof out);

	if (e)
		return e;
	/*
	 * The offset is a services cookie in pages, as libsrv_um passes it to mmap2.
	 * MAP_32BIT by default: drivers before the user-address fix walk the VMA
	 * with an IMG_UINT32 in DoMapToUser, so a mapping above 4 GB fails
	 * VM_INSERT_PAGE with -EFAULT there. Clear p->map_flags on a fixed driver.
	 */
	m = mmap(NULL, out.ui32RealByteSize, PROT_READ | PROT_WRITE, MAP_SHARED | p->map_flags, p->fd,
		 (off_t)out.ui32MMapOffset << 12);
	if (m == MAP_FAILED)
		return -errno;
	if (p->trace_fd >= 0)
		trace_mmap(p, m, out.ui32RealByteSize, PROT_READ | PROT_WRITE, MAP_SHARED | p->map_flags,
			   (uint64_t)out.ui32MMapOffset << 12);
	if (p->nmaps < sizeof p->maps / sizeof *p->maps) {
		p->maps[p->nmaps].addr = m;
		p->maps[p->nmaps++].len = out.ui32RealByteSize;
	}
	b->cpu = (char *)m + out.ui32ByteOffset;
	b->mmap_base = m;
	b->mmap_len = out.ui32RealByteSize;
	return 0;
}

void pvr_unmap(struct pvr *p, struct pvr_buf *b)
{
	if (!b->mmap_base)
		return;
	for (unsigned i = 0; i < p->nmaps; i++)
		if (p->maps[i].addr == b->mmap_base) {
			p->maps[i] = p->maps[--p->nmaps];
			break;
		}
	munmap(b->mmap_base, b->mmap_len);
	b->mmap_base = NULL;
	b->cpu = NULL;
}

volatile PVRSRV_SYNC_DATA *pvr_sync_data(struct pvr *p, IMG_HANDLE sync, struct pvr_buf *keep)
{
	memset(keep, 0, sizeof *keep);
	keep->mem = sync;
	return pvr_map(p, keep) ? NULL : keep->cpu;
}

int pvr_wrap(struct pvr *p, struct pvr_buf *b, void *cpu, uint32_t size)
{
	PVRSRV_BRIDGE_IN_WRAP_EXT_MEMORY in = {
		.hDevCookie = p->devcookie,
		.hDevMemContext = p->memctx,
		.pvLinAddr = cpu,
		.ui32ByteSize = size,
	};
	PVRSRV_BRIDGE_OUT_WRAP_EXT_MEMORY out;
	int e;

	memset(&out, 0, sizeof out);
	memset(b, 0, sizeof *b);
	e = pvr_call(p, PVRSRV_BRIDGE_WRAP_EXT_MEMORY, &in, sizeof in, &out, sizeof out);
	if (e)
		return e;
	b->mem = out.sClientMemInfo.hKernelMemInfo;
	b->sync = out.sClientSyncInfo.hKernelSyncInfo;
	b->devva = out.sClientMemInfo.sDevVAddr.uiAddr;
	b->size = size;
	b->cpu = cpu;
	b->wrapped = 1;
	b->client_mem = out.sClientMemInfo;
	b->client_sync = out.sClientSyncInfo;
	return 0;
}

void pvr_free(struct pvr *p, struct pvr_buf *b)
{
	PVRSRV_BRIDGE_RETURN out = { 0 };

	if (!b->mem)
		return;
	pvr_unmap(p, b);
	if (b->wrapped) {
		PVRSRV_BRIDGE_IN_UNWRAP_EXT_MEMORY in;

		memset(&in, 0, sizeof in);
		in.hKernelMemInfo = b->mem;
		in.sClientMemInfo = b->client_mem;
		in.sClientSyncInfo = b->client_sync;
		pvr_call(p, PVRSRV_BRIDGE_UNWRAP_EXT_MEMORY, &in, sizeof in, &out, sizeof out);
	} else {
		PVRSRV_BRIDGE_IN_FREEDEVICEMEM in;

		memset(&in, 0, sizeof in);
		in.hDevCookie = p->devcookie;
		in.psKernelMemInfo = b->mem;
		in.sClientMemInfo = b->client_mem;
		pvr_call(p, PVRSRV_BRIDGE_FREE_DEVICEMEM, &in, sizeof in, &out, sizeof out);
	}
	memset(b, 0, sizeof *b);
}

int pvr_register_render_ctx(struct pvr *p, const void *ctx, uint32_t size, uint32_t pd_off,
			    IMG_HANDLE *handle, uint32_t *devva)
{
	PVRSRV_BRIDGE_IN_SGX_REGISTER_HW_RENDER_CONTEXT in = {
		.hDevCookie = p->devcookie,
		.pHWRenderContextCpuVAddr = (IMG_CPU_VIRTADDR)ctx,
		.ui32HWRenderContextSize = size,
		.ui32OffsetToPDDevPAddr = pd_off,
		.hDevMemContext = p->memctx,
	};
	PVRSRV_BRIDGE_OUT_SGX_REGISTER_HW_RENDER_CONTEXT out = { 0 };
	int e;

	if (p->trace_fd >= 0)
		trace_ubuf(p, PVRSRV_BRIDGE_SGX_REGISTER_HW_RENDER_CONTEXT & 0xff, ctx, size);
	e = pvr_call(p, PVRSRV_BRIDGE_SGX_REGISTER_HW_RENDER_CONTEXT, &in, sizeof in, &out,
			 sizeof out);

	if (!e) {
		*handle = out.hHWRenderContext;
		*devva = out.sHWRenderContextDevVAddr.uiAddr;
	}
	return e;
}

int pvr_kick(struct pvr *p, SGX_CCB_KICK *kick)
{
	PVRSRV_BRIDGE_IN_DOKICK in;
	PVRSRV_BRIDGE_RETURN out = { 0 };

	memset(&in, 0, sizeof in);
	in.hDevCookie = p->devcookie;
	in.sCCBKick = *kick;
	p->kicks++;
	if (p->trace_fd >= 0) {
		if (kick->ui32NumDstSyncObjects)
			trace_ubuf(p, PVRSRV_BRIDGE_SGX_DOKICK & 0xff, kick->pahDstSyncHandles,
				   kick->ui32NumDstSyncObjects * sizeof(IMG_HANDLE));
		if (!p->trace_kicks || p->kicks <= p->trace_kicks)
			trace_snap(p, p->kicks);
	}
	return pvr_call(p, PVRSRV_BRIDGE_SGX_DOKICK, &in, sizeof in, &out, sizeof out);
}

/* Heap info as GET_DEVMEM_HEAPINFO reports it (the decoders name heaps from this call). */
int pvr_heapinfo(struct pvr *p)
{
	PVRSRV_BRIDGE_IN_GET_DEVMEM_HEAPINFO in = { .hDevCookie = p->devcookie, .hDevMemContext = p->memctx };
	PVRSRV_BRIDGE_OUT_GET_DEVMEM_HEAPINFO out;
	int e;

	memset(&out, 0, sizeof out);
	e = pvr_call(p, PVRSRV_BRIDGE_GET_DEVMEM_HEAPINFO, &in, sizeof in, &out, sizeof out);
	if (e)
		return e;
	p->nheaps = out.ui32ClientHeapCount;
	memcpy(p->heaps, out.sHeapInfo, sizeof p->heaps);
	return 0;
}

int pvr_flush_rt(struct pvr *p, uint32_t rtdata)
{
	PVRSRV_BRIDGE_IN_SGX_FLUSH_HW_RENDER_TARGET in = { .hDevCookie = p->devcookie };
	PVRSRV_BRIDGE_RETURN out = { 0 };

	in.sHWRTDataSetDevVAddr.uiAddr = rtdata;
	return pvr_call(p, PVRSRV_BRIDGE_SGX_FLUSH_HW_RENDER_TARGET, &in, sizeof in, &out, sizeof out);
}

int pvr_unregister_render_ctx(struct pvr *p, IMG_HANDLE ctx)
{
	PVRSRV_BRIDGE_IN_SGX_UNREGISTER_HW_RENDER_CONTEXT in = { .hDevCookie = p->devcookie,
								 .hHWRenderContext = ctx };
	PVRSRV_BRIDGE_RETURN out = { 0 };

	return pvr_call(p, PVRSRV_BRIDGE_SGX_UNREGISTER_HW_RENDER_CONTEXT, &in, sizeof in, &out, sizeof out);
}
