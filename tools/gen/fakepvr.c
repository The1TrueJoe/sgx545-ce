/*
 * fakepvr: a stand-in for the services kernel driver, so the generator can run
 * on any x86_64 Linux without touching a GPU (tools/gen/build.sh builds
 * frame-fake with it). It answers the bridge calls sgxgen makes, hands out
 * device addresses from the heaps' bases the way the EA3's kernel does (bump
 * allocation per heap), and backs every "device" buffer with ordinary memory.
 * With -t, frame-fake writes a pvrtrace log that the decoders read like a
 * real run: the frame's every word can be checked before it goes near the GPU.
 *
 * pvr.c is compiled with -Dioctl=fake_ioctl -Dmmap=fake_mmap for this.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "pvr.h"

/* the EA3's client heaps (GET_DEVMEM_HEAPINFO in a DDK capture): id, base, size */
static const uint32_t heaps[][3] = {
	{ 0x7000000, 0x3e000000, 0xb1fff000 }, { 0x7000001, 0x20000000, 0xcfff000 },
	{ 0x7000002, 0x30000000, 0x7f000 },    { 0x7000003, 0x32000000, 0x4fff000 },
	{ 0x7000004, 0x37000000, 0x4ff000 },   { 0x7000005, 0x3c000000, 0x1ff000 },
	{ 0x7000006, 0x2e000000, 0x1fff000 },  { 0x7000007, 0x30400000, 0x1bff000 },
	{ 0x7000008, 0x2d000000, 0xfff000 },   { 0x7000009, 0x10000000, 0 },
	{ 0x700000a, 0x10000000, 0xffff000 },  { 0x700000e, 0xf0000000, 0xffff000 },
};
#define NHEAPS (sizeof heaps / sizeof *heaps)

static uint32_t next[NHEAPS];

struct fbuf {
	uint32_t size;
	void *mem;
};
static struct fbuf bufs[1024];		/* handle = index + 1; sync handle = index + 0x1001 */
static unsigned nbufs;
static char syncpages[64][4096] __attribute__((aligned(4096)));	/* sync data: all zero, complete == pending */

static void heapinfo(PVRSRV_HEAP_INFO *h, IMG_UINT32 *n)
{
	for (unsigned i = 0; i < NHEAPS; i++) {
		h[i].ui32HeapID = heaps[i][0];
		h[i].hDevMemHeap = (IMG_HANDLE)(uintptr_t)(0x100 + i);
		h[i].sDevVAddrBase.uiAddr = heaps[i][1];
		h[i].ui32HeapByteSize = heaps[i][2];
		h[i].ui32Attribs = 0x2014200;
	}
	*n = NHEAPS;
}

int fake_ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	PVRSRV_BRIDGE_PACKAGE *pkg;
	void *in, *out;

	(void)fd;
	(void)req;
	va_start(ap, req);
	pkg = va_arg(ap, PVRSRV_BRIDGE_PACKAGE *);
	va_end(ap);
	in = pkg->pvParamIn;
	out = pkg->pvParamOut;
	memset(out, 0, pkg->ui32OutBufferSize);
	switch (pkg->ui32BridgeID) {
	case PVRSRV_BRIDGE_CONNECT_SERVICES:
		((PVRSRV_BRIDGE_OUT_CONNECT_SERVICES *)out)->hKernelServices = (IMG_HANDLE)0x11;
		break;
	case PVRSRV_BRIDGE_ACQUIRE_DEVICEINFO:
		((PVRSRV_BRIDGE_OUT_ACQUIRE_DEVICEINFO *)out)->hDevCookie = (IMG_HANDLE)0x22;
		break;
	case PVRSRV_BRIDGE_CREATE_DEVMEMCONTEXT: {
		PVRSRV_BRIDGE_OUT_CREATE_DEVMEMCONTEXT *o = out;

		o->hDevMemContext = (IMG_HANDLE)0x33;
		heapinfo(o->sHeapInfo, &o->ui32ClientHeapCount);
		/* what the kernel had already taken in the shared heaps on the EA3 */
		next[3] = 0xe000;
		next[8] = 0x3000;
		break;
	}
	case PVRSRV_BRIDGE_GET_DEVMEM_HEAPINFO: {
		PVRSRV_BRIDGE_OUT_GET_DEVMEM_HEAPINFO *o = out;

		heapinfo(o->sHeapInfo, &o->ui32ClientHeapCount);
		break;
	}
	case PVRSRV_BRIDGE_ALLOC_DEVICEMEM: {
		PVRSRV_BRIDGE_IN_ALLOCDEVICEMEM *i = in;
		PVRSRV_BRIDGE_OUT_ALLOCDEVICEMEM *o = out;
		unsigned h = (unsigned)((uintptr_t)i->hDevMemHeap - 0x100);
		uint32_t a = i->ui32Alignment ? i->ui32Alignment : 1, size = (i->ui32Size + 0xfff) & ~0xfffu;

		if (h >= NHEAPS || nbufs >= 1024)
			return errno = EINVAL, -1;
		next[h] = (next[h] + a - 1) & ~(a - 1);
		o->sClientMemInfo.sDevVAddr.uiAddr = heaps[h][1] + next[h];
		o->sClientMemInfo.uAllocSize = i->ui32Size;
		o->sClientMemInfo.ui32Flags = i->ui32Attribs;
		o->sClientMemInfo.hKernelMemInfo = (IMG_HANDLE)(uintptr_t)(nbufs + 1);
		o->sClientSyncInfo.hKernelSyncInfo = (IMG_HANDLE)(uintptr_t)(nbufs + 0x1001);
		next[h] += i->ui32Size;
		bufs[nbufs].size = size;
		bufs[nbufs].mem = aligned_alloc(4096, size);
		memset(bufs[nbufs].mem, 0, size);
		nbufs++;
		break;
	}
	case PVRSRV_BRIDGE_MHANDLE_TO_MMAP_DATA: {
		PVRSRV_BRIDGE_IN_MHANDLE_TO_MMAP_DATA *i = in;
		PVRSRV_BRIDGE_OUT_MHANDLE_TO_MMAP_DATA *o = out;
		uintptr_t h = (uintptr_t)i->hMHandle;

		o->ui32MMapOffset = (uint32_t)h;	/* the cookie fake_mmap resolves */
		o->ui32RealByteSize = h > 0x1000 ? 4096 : bufs[h - 1].size;
		break;
	}
	case PVRSRV_BRIDGE_SGX_REGISTER_HW_RENDER_CONTEXT:
		((PVRSRV_BRIDGE_OUT_SGX_REGISTER_HW_RENDER_CONTEXT *)out)->sHWRenderContextDevVAddr.uiAddr = 0x3000e000;
		((PVRSRV_BRIDGE_OUT_SGX_REGISTER_HW_RENDER_CONTEXT *)out)->hHWRenderContext = (IMG_HANDLE)0x44;
		break;
	case PVRSRV_BRIDGE_SGX_DOKICK:
	case PVRSRV_BRIDGE_SGX_FLUSH_HW_RENDER_TARGET:
	case PVRSRV_BRIDGE_SGX_UNREGISTER_HW_RENDER_CONTEXT:
		break;
	default:
		return errno = ENOTTY, -1;
	}
	return 0;
}

void *fake_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	uintptr_t h = (uintptr_t)(off >> 12);

	(void)addr;
	(void)len;
	(void)prot;
	(void)flags;
	(void)fd;
	if (h > 0x1000)
		return h - 0x1001 < 64 ? syncpages[h - 0x1001] : MAP_FAILED;
	if (h == 0 || h > nbufs)
		return MAP_FAILED;
	return bufs[h - 1].mem;
}
