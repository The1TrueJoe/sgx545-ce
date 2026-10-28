#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pvr.h"

/* pvr_drm.h: DRM_IOW(DRM_COMMAND_BASE + PVR_DRM_SRVKM_CMD, PVRSRV_BRIDGE_PACKAGE) */
#define DRM_IOCTL_PVR_SRVKM _IOW('d', 0x40 + 0x12, PVRSRV_BRIDGE_PACKAGE)

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

	if (ioctl(p->fd, DRM_IOCTL_PVR_SRVKM, &pkg) < 0)
		return -errno;
	return out_size >= 4 ? (int)*(PVRSRV_ERROR *)out : 0;
}

int pvr_open(struct pvr *p, const char *node)
{
	memset(p, 0, sizeof *p);
	p->fd = open(node ? node : "/dev/dri/card0", O_RDWR | O_CLOEXEC);
	return p->fd < 0 ? -errno : 0;
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
	 * MAP_32BIT: the kernel's DoMapToUser walks the VMA with an IMG_UINT32
	 * (ulVMAPos), so a mapping above 4 GB fails VM_INSERT_PAGE with -EFAULT.
	 */
	m = mmap(NULL, out.ui32RealByteSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_32BIT, p->fd,
		 (off_t)out.ui32MMapOffset << 12);
	if (m == MAP_FAILED)
		return -errno;
	b->cpu = (char *)m + out.ui32ByteOffset;
	return 0;
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
	return 0;
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
	int e = pvr_call(p, PVRSRV_BRIDGE_SGX_REGISTER_HW_RENDER_CONTEXT, &in, sizeof in, &out,
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
	return pvr_call(p, PVRSRV_BRIDGE_SGX_DOKICK, &in, sizeof in, &out, sizeof out);
}
