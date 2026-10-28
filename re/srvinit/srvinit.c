/*
 * srvinit: bring the SGX545 up at boot without libsrv_init.
 *
 * Does what the DDK's SrvInit() does (decompiled from libsrv_init 1.7.862890
 * and checked against a pvrtrace of it on an EA3, see SRVINIT.md): connect as
 * the init process, allocate the microkernel's buffers in the kernel heaps,
 * upload and patch the microkernel and its PDS programs, build the init
 * scripts, hand it all to the kernel with SGX_DEVINITPART2, then disconnect,
 * which is when the kernel actually starts the microkernel.
 *
 * The only Imagination bytes are the firmware file that fetch-firmware.sh pulls
 * from your own DDK. Every address and register value is computed here.
 *
 *   srvinit [-f firmware]           initialise the GPU (once per boot, as root)
 *   srvinit -n plan [-f fw] outdir   offline: build everything from the
 *                                    addresses in plan, write it to outdir
 *                                    (check.py compares it with a trace)
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pvr.h"
#include "sgxdefs.h"

#define FW_DEFAULT "/lib/firmware/sgx545-ukernel-1.7.862890.bin"
#define FW_SIZE 0xd148

/* Firmware file layout: libsrv_init .rodata 0x1e20..0xef68. */
#define FW_UKERNEL 0x1e0
#define UKERNEL_SIZE 0xcf68

enum {
	B_CCB,		/* kernel CCB */
	B_CCBCTL,	/* CCB read/write offsets */
	B_KICKER,	/* EUR_CR_EVENT_KICKER target */
	B_TA3DCTL,	/* the microkernel's own control block */
	B_HOSTCTL,	/* SGXMKIF_HOST_CTL */
	B_MISC,		/* PVRSRV_SGX_MISCINFO_INFO */
	B_HWPERF,	/* HWPerf circular buffer */
	B_TASIG,	/* TA signature register list */
	B_3DSIG,	/* 3D signature register list */
	B_UKERNEL,	/* USSE microkernel */
	B_PDS0, B_PDS1, B_PDS2, B_PDS3, B_PDS4,	/* PDS programs that enter it */
	NBUF
};

#define KD SGX_KERNEL_DATA_HEAP_ID
#define KC SGX_KERNEL_CODE_HEAP_ID
#define RW (PVRSRV_MEM_READ | PVRSRV_MEM_WRITE | PVRSRV_MEM_CACHE_CONSISTENT | PVRSRV_MEM_NO_SYNCOBJ | \
	    PVRSRV_MEM_EDM_PROTECT)
#define RO (PVRSRV_MEM_READ | PVRSRV_MEM_NO_SYNCOBJ)

/* In SrvInit's allocation order, which the device addresses depend on. */
static const struct {
	const char *name;
	uint32_t heap, attribs, size, align;
	uint32_t fw;		/* offset of its template in the firmware file, or ~0 */
} bufs[NBUF] = {
	[B_CCB] = { "ccb", KD, RW, 0x2000, 0x1000, ~0u },
	[B_CCBCTL] = { "ccbctl", KD, RW, 0x8, 0x1000, ~0u },
	[B_KICKER] = { "kicker", KD, RW, 0x4, 0x10, ~0u },
	[B_TA3DCTL] = { "ta3dctl", KD, RW | PVRSRV_HAP_MULTI_PROCESS, 0xec, 0x20, ~0u },
	[B_HOSTCTL] = { "hostctl", KD, RW | PVRSRV_HAP_MULTI_PROCESS, 0x44, 0x20, ~0u },
	[B_MISC] = { "misc", KD, RW | PVRSRV_HAP_MULTI_PROCESS, 0x54, 0x20, ~0u },
	[B_HWPERF] = { "hwperf", KD, RW, 0x640c, 0x20, ~0u },
	[B_TASIG] = { "tasig", KD, RW, 0x620, 0x20, ~0u },
	[B_3DSIG] = { "3dsig", KD, RW, 0x2088, 0x20, ~0u },
	[B_UKERNEL] = { "ukernel", KC, RO, UKERNEL_SIZE, 0x8, FW_UKERNEL },
	[B_PDS0] = { "pds0", KC, RO, 0x18, 0x10, 0x168 },
	[B_PDS1] = { "pds1", KC, RO, 0x38, 0x10, 0x180 },
	[B_PDS2] = { "pds2", KC, RO, 0x18, 0x10, 0x1b8 },
	[B_PDS3] = { "pds3", KC, RO, 0x100, 0x10, 0x000 },
	[B_PDS4] = { "pds4", KC, RO, 0x68, 0x10, 0x100 },
};

/* Microkernel entry points (byte offsets into it) the PDS programs jump to. */
#define UK_EVENT 0xd60
#define UK_ENTRY1 0xdf0
#define UK_ENTRY2 0xe20

/* What the rest of the build needs to know about where things landed. */
struct init {
	struct pvr_buf b[NBUF];
	uint32_t code_heap, pixel_heap, vertex_heap, pds_pixel_heap;
	uint32_t pd;		/* page directory, device physical */
	uint32_t hwperf_flags;	/* app hint */
};

/* Heap type is the low 24 bits of ui32HeapID. */
static void heap_base(struct init *s, uint32_t type, uint32_t base)
{
	if (type == SGX_KERNEL_CODE_HEAP_ID)
		s->code_heap = base;
	else if (type == SGX_PIXELSHADER_HEAP_ID)
		s->pixel_heap = base;
	else if (type == SGX_VERTEXSHADER_HEAP_ID)
		s->vertex_heap = base;
	else if (type == SGX_PDSPIXEL_CODEDATA_HEAP_ID)
		s->pds_pixel_heap = base;
}

static uint32_t *w(struct init *s, int i) { return s->b[i].cpu; }
static uint32_t va(struct init *s, int i) { return s->b[i].devva; }

/*
 * A PDS DOUTU word 0 for microkernel code at device address a: USE code base
 * register 15 (+ the 512 KB segment) and the offset in that segment in 8-byte
 * instructions, << 4. Bit 27 is set in every one libsrv_init writes.
 */
static uint32_t doutu(struct init *s, uint32_t a)
{
	uint32_t off = a - s->code_heap, seg = off >> 19;

	return ((seg + 15) & 15) | 0x8000000 | (((off - (seg << 19)) * 2) & 0x3ffff0);
}

/*
 * Fill one of the microkernel's limm placeholders (0x?dbeef / 0xfc237150) with
 * a 32-bit immediate: bits 0-20 into word 0, 21-25 into word 1 bits 4-8,
 * 26-31 into word 1 bits 12-17.
 */
static int limm(struct init *s, uint32_t off, uint32_t v)
{
	uint32_t *i = (uint32_t *)((char *)w(s, B_UKERNEL) + off);

	if ((i[0] & 0x1fffff) != 0xdbeef || i[1] != 0xfc237150) {
		fprintf(stderr, "srvinit: no limm placeholder at ukernel+0x%x\n", off);
		return -1;
	}
	i[0] = (i[0] & 0xffe00000) | (v & 0x1fffff);
	i[1] = (i[1] & 0xfffc0e0f) | ((v >> 17) & 0x1f0) | ((v >> 26) << 12);
	return 0;
}

struct script {
	SGX_INIT_COMMAND *c;
	unsigned n, max;
};

static void wr(struct script *sc, uint32_t reg, uint32_t val)
{
	if (sc->n + 1 < sc->max) {
		sc->c[sc->n].sWriteHWReg.eOp = SGX_INIT_OP_WRITE_HW_REG;
		sc->c[sc->n].sWriteHWReg.ui32Offset = reg;
		sc->c[sc->n].sWriteHWReg.ui32Value = val;
		sc->n++;
	}
}

static void end(struct script *sc) { sc->c[sc->n++].eOp = SGX_INIT_OP_HALT; }

/* Write the buffers' contents and the bridge's init info (handles aside). */
static int build(struct init *s, const uint8_t *fw, SGX_BRIDGE_INIT_INFO *ii)
{
	uint32_t uk = va(s, B_UKERNEL);
	uint32_t *t;
	struct script sc;
	static const uint32_t tasig[] = { 0x304, 0x26c, 0x270, 0x2a0, 0x2a4, 0x378 };
	static const uint32_t sig3d[] = { 0x438, 0x4cc, 0x4d0, 0x4d4, 0x4d8, 0x510, 0x514, 0x518,
					  0x51c, 0x524, 0x528, 0x52c, 0x530, 0x534, 0x538, 0x53c,
					  0x540, 0x544, 0x550, 0x554, 0x558, 0x55c, 0x84c, 0x850,
					  0x854, 0x858, 0x560, 0x564, 0x568, 0x56c, 0x308, 0x30c };
	static const uint32_t kick[SGXMKIF_CMD_MAX] = {
		[SGXMKIF_CMD_TA] = 0x11d, [SGXMKIF_CMD_TRANSFER] = 0x103,
		[SGXMKIF_CMD_POWER] = 0xd4, [SGXMKIF_CMD_CONTEXTSUSPEND] = 0xeb,
		[SGXMKIF_CMD_CLEANUP] = 0x13a, [SGXMKIF_CMD_GETMISCINFO] = 0x173,
		[SGXMKIF_CMD_PROCESS_QUEUES] = 0x156, [9] = 0x157,
	};

	for (int i = 0; i < NBUF; i++) {
		memset(s->b[i].cpu, 0, bufs[i].size);
		if (bufs[i].fw != ~0u)
			memcpy(s->b[i].cpu, fw + bufs[i].fw, bufs[i].size);
	}

	/* The microkernel's control block: where everything else is. */
	t = w(s, B_TA3DCTL);
	t[0x00] = va(s, B_TA3DCTL);
	t[0x01] = va(s, B_HOSTCTL);
	t[0x02] = va(s, B_CCBCTL);
	t[0x13] = s->hwperf_flags;
	t[0x16] = va(s, B_CCB);
	t[0x17] = s->pd;
	t[0x2b] = va(s, B_HWPERF);
	t[0x2c] = va(s, B_TASIG);
	t[0x2d] = va(s, B_3DSIG);

	/* Signature registers the microkernel samples to spot a lockup. */
	t = w(s, B_TASIG);
	t[0] = sizeof tasig / 4;
	memcpy(t + 2, tasig, sizeof tasig);
	t = w(s, B_3DSIG);
	t[0] = sizeof sig3d / 4;
	memcpy(t + 2, sig3d, sizeof sig3d);

	/* PDS programs: their data segments name the microkernel code they start. */
	t = w(s, B_PDS0);
	t[0] = doutu(s, uk + UK_EVENT);
	t[1] = 2;
	t = w(s, B_PDS1);
	t[0] = va(s, B_TA3DCTL);
	t[1] = 0x3c0003f;
	{
		uint32_t off = uk + UK_ENTRY1 - s->code_heap;
		t[4] = ((off * 2) & 0xffff0) | (((off >> 19) + 15) & 15);
	}
	t[5] = 2;
	t = w(s, B_PDS2);
	t[0] = doutu(s, uk + UK_ENTRY2);
	t[1] = 2;
	t = w(s, B_PDS3);
	t[0] = 2;
	t[3] = 0x100;
	t[6] = 0x200;
	t[7] = doutu(s, uk + 0x7050);
	t[10] = doutu(s, uk + 0x2cd0);
	t[11] = doutu(s, uk + 0x5670);
	t[14] = doutu(s, uk + 0x30);
	t = w(s, B_PDS4);
	t[0] = doutu(s, uk + 0x73a0);
	t[1] = 2;
	t[3] = 0x200;
	t[4] = doutu(s, uk + 0x2ea0);
	t[5] = 2;
	t[7] = 0x4000f;
	t[8] = doutu(s, uk + 0x5740);
	t[9] = 2;
	t[10] = 0x120080;

	/* The microkernel's own immediates: buffer and program addresses. */
	if (limm(s, 0xbaf8, 0x1800100) || limm(s, 0xbb10, va(s, B_PDS4) >> 4) ||
	    limm(s, 0xd60, 0x1001000) || limm(s, 0xd70, va(s, B_PDS1) >> 4) ||
	    limm(s, 0xd78, va(s, B_TA3DCTL)) || limm(s, 0xdb0, 0x800100) ||
	    limm(s, 0xdc0, va(s, B_PDS2) >> 4) || limm(s, 0xe40, va(s, B_PDS3) & ~0xfu) ||
	    limm(s, 0xe50, 6))
		return -1;

	memset(ii, 0, sizeof *ii);
	memcpy(ii->aui32HostKickAddr, kick, sizeof kick);

	sc = (struct script){ ii->sScripts.asInitCommandsPart1, 0, SGX_MAX_INIT_COMMANDS };
	wr(&sc, 0xa58, 0);
	wr(&sc, EUR_CR_EVENT_TIMER, 0);
	end(&sc);

	sc = (struct script){ ii->sScripts.asInitCommandsPart2, 0, SGX_MAX_INIT_COMMANDS };
	wr(&sc, 0x818, 0x7ff);
	wr(&sc, 0x810, 1);
	wr(&sc, 0x814, 0xffff);
	wr(&sc, EUR_CR_EVENT_HOST_ENABLE, 0x4000);
	wr(&sc, EUR_CR_EVENT_HOST_ENABLE2, 0);
	wr(&sc, 0x13c, 0);
	wr(&sc, 0xa7c, 0);
	wr(&sc, 0xa80, 0);
	wr(&sc, 0xa74, 0x208200);
	wr(&sc, 0x4e8, 0x100);
	wr(&sc, 0xa78, 0x2c);
	wr(&sc, 0xaac, 0x45);
	wr(&sc, 0xabc, 0xe);
	wr(&sc, 0x800, 0x30);
	wr(&sc, 0x804, 0xffff);
	wr(&sc, 0xa00, 0x7c000);
	/* USE code bases: 0-9 the pixel shader heap, 10-13 the vertex shader
	   heap, 512 KB each; 15 the microkernel's heap. */
	for (uint32_t i = 0; i < 10; i++)
		wr(&sc, EUR_CR_USE_CODE_BASE_0 + 4 * i, ((s->pixel_heap + (i << 19)) >> 7) | 0x2000000);
	for (uint32_t i = 0; i < 4; i++)
		wr(&sc, EUR_CR_USE_CODE_BASE_10 + 4 * i, (s->vertex_heap + (i << 19)) >> 7);
	wr(&sc, EUR_CR_USE_CODE_BASE_15, (s->code_heap >> 7) | 0x6000000);
	wr(&sc, EUR_CR_PDS_EXEC_BASE, s->pds_pixel_heap);
	wr(&sc, 0xb30, 0x100);
	wr(&sc, EUR_CR_EVENT_KICKER, va(s, B_KICKER));
	/* The event PDS program: address, then two control words. */
	wr(&sc, 0xa68, va(s, B_PDS0) & ~0xfu);
	wr(&sc, 0xa6c, 1);
	wr(&sc, 0xa70, 6);
	wr(&sc, 0xa58, 0x2204b00f);
	end(&sc);

	sc = (struct script){ ii->sScripts.asDeinitCommands, 0, SGX_MAX_DEINIT_COMMANDS };
	wr(&sc, 0xa58, 0);
	wr(&sc, 0xa68, 0);
	wr(&sc, 0xa6c, 0);
	wr(&sc, 0xa70, 0);
	wr(&sc, EUR_CR_USE_CODE_BASE_15, 0);
	wr(&sc, EUR_CR_EVENT_HOST_ENABLE, 0);
	wr(&sc, EUR_CR_EVENT_HOST_ENABLE2, 0);
	wr(&sc, EUR_CR_CLKGATECTL, 0);
	wr(&sc, EUR_CR_CLKGATECTL2, 0);
	end(&sc);

	/* What this microkernel was built with; the kernel checks both. */
	ii->ui32ClientBuildOptions = 0x60218;
	ii->sSGXStructSizes = (SGX_MISCINFO_STRUCT_SIZES){
		.ui32Sizeof_CMDTA = 0x2b0,
		.ui32Sizeof_CMDTA_SHARED = 0x1e0,
		.ui32Sizeof_TRANSFERCMD = 0x4c8,
		.ui32Sizeof_TRANSFERCMD_SHARED = 0xdc,
		.ui32Sizeof_3DREGISTERS = 0xd0,
		.ui32Sizeof_HWPBDESC = 0x3c,
		.ui32Sizeof_HWRENDERCONTEXT = 0x44,
		.ui32Sizeof_HWRENDERDETAILS = 0x1ec,
		.ui32Sizeof_HWRTDATA = 0x38,
		.ui32Sizeof_HWRTDATASET = 0x50,
		.ui32Sizeof_HWTRANSFERCONTEXT = 0xb4,
		.ui32Sizeof_HOST_CTL = 0x44,
		.ui32Sizeof_COMMAND = 0x20,
	};

	/* Registers the kernel reads to see that the EDM is still making progress. */
	ii->ui32EDMTaskReg0 = 0xb14;
	ii->ui32EDMTaskReg1 = 0xb20;
	ii->ui32ClkGateCtl = 0xa6aa;
	ii->ui32ClkGateCtl2 = 0xaaaaaa;
	ii->ui32ClkGateStatusReg = 0x8;
	ii->ui32ClkGateStatusMask = 0xffffffdf;
	ii->ui32CacheControl = 2;

	ii->asInitDevData[8] = 2;
	ii->asInitDevData[10] = 0x300;
	ii->asInitDevData[11] = 0x940;
	ii->asInitDevData[12] = s->pds_pixel_heap;
	ii->asInitDevData[13] = s->pixel_heap;
	return 0;
}

/* /etc/powervr.ini, as the DDK's app hints read it: KEY=value lines. */
static uint32_t apphint(const char *key)
{
	FILE *f = fopen("/etc/powervr.ini", "r");
	char l[256];
	size_t n = strlen(key);
	uint32_t v = 0;

	if (!f)
		return 0;
	while (fgets(l, sizeof l, f))
		if (!strncmp(l, key, n) && l[n] == '=')
			v = (uint32_t)strtoul(l + n + 1, NULL, 0);
	fclose(f);
	return v;
}

static uint8_t *load_fw(const char *path)
{
	FILE *f = fopen(path, "rb");
	uint8_t *d = malloc(FW_SIZE + 1);
	size_t n;

	if (!f || !d) {
		fprintf(stderr, "srvinit: %s: %s\n", path, strerror(errno));
		exit(2);
	}
	n = fread(d, 1, FW_SIZE + 1, f);
	fclose(f);
	if (n != FW_SIZE) {
		fprintf(stderr, "srvinit: %s: %zu bytes, want %u (run fetch-firmware.sh)\n", path, n, FW_SIZE);
		exit(2);
	}
	return d;
}

static void fill_handles(struct init *s, SGX_BRIDGE_INIT_INFO *ii)
{
	ii->hKernelCCBMemInfo = s->b[B_CCB].mem;
	ii->hKernelCCBCtlMemInfo = s->b[B_CCBCTL].mem;
	ii->hKernelCCBEventKickerMemInfo = s->b[B_KICKER].mem;
	ii->hKernelSGXHostCtlMemInfo = s->b[B_HOSTCTL].mem;
	ii->hKernelSGXTA3DCtlMemInfo = s->b[B_TA3DCTL].mem;
	ii->hKernelSGXMiscMemInfo = s->b[B_MISC].mem;
	ii->hKernelHWPerfCBMemInfo = s->b[B_HWPERF].mem;
	ii->hKernelTASigBufferMemInfo = s->b[B_TASIG].mem;
	ii->hKernel3DSigBufferMemInfo = s->b[B_3DSIG].mem;
	for (int i = B_UKERNEL; i < NBUF; i++)
		ii->asInitMemHandles[i - B_UKERNEL] = s->b[i].mem;
}

/* Offline: addresses from a plan file ("buf <i> <devva>", "heap <id> <base>", "pd <a>"). */
static int plan(const char *path, const uint8_t *fw, const char *outdir)
{
	static struct init s;
	static SGX_BRIDGE_INIT_INFO ii;
	FILE *f = fopen(path, "r");
	char l[128], k[16], fn[512];
	unsigned long a, b;

	if (!f)
		return perror(path), 2;
	while (fgets(l, sizeof l, f)) {
		if (sscanf(l, "%15s %li %li", k, &a, &b) == 3 && !strcmp(k, "buf") && a < NBUF)
			s.b[a].devva = (uint32_t)b, s.b[a].mem = (IMG_HANDLE)(a + 1);
		else if (sscanf(l, "%15s %li %li", k, &a, &b) == 3 && !strcmp(k, "heap"))
			heap_base(&s, (uint32_t)a, (uint32_t)b);
		else if (sscanf(l, "%15s %li", k, &a) == 2 && !strcmp(k, "pd"))
			s.pd = (uint32_t)a;
	}
	fclose(f);
	for (int i = 0; i < NBUF; i++)
		s.b[i].cpu = calloc(1, bufs[i].size);
	if (build(&s, fw, &ii))
		return 1;
	fill_handles(&s, &ii);
	for (int i = 0; i < NBUF; i++) {
		snprintf(fn, sizeof fn, "%s/%s.bin", outdir, bufs[i].name);
		if (!(f = fopen(fn, "wb")))
			return perror(fn), 2;
		fwrite(s.b[i].cpu, 1, bufs[i].size, f);
		fclose(f);
	}
	snprintf(fn, sizeof fn, "%s/initinfo.bin", outdir);
	if (!(f = fopen(fn, "wb")))
		return perror(fn), 2;
	fwrite(&ii, 1, sizeof ii, f);
	fclose(f);
	return 0;
}

static const char *step = "start";
#define TRY(what, e) do { step = (what); if ((err = (e)) != 0) goto fail; } while (0)

int main(int argc, char **argv)
{
	const char *fwpath = FW_DEFAULT, *planpath = NULL;
	static struct pvr p;
	static struct init s;
	static PVRSRV_BRIDGE_IN_SGXDEVINITPART2 in2;
	PVRSRV_BRIDGE_OUT_SGXDEVINITPART2 out2 = { 0 };
	PVRSRV_BRIDGE_RETURN ret = { 0 };
	uint8_t *fw;
	int c, err = 0, connected = 0;

	while ((c = getopt(argc, argv, "f:n:")) != -1) {
		if (c == 'f')
			fwpath = optarg;
		else if (c == 'n')
			planpath = optarg;
		else
			return fprintf(stderr, "usage: srvinit [-f firmware] [-n plan outdir]\n"), 2;
	}
	fw = load_fw(fwpath);
	if (planpath)
		return optind < argc ? plan(planpath, fw, argv[optind]) : 2;

	TRY("open", pvr_open(&p, NULL));
	TRY("connect", pvr_connect(&p));
	{
		PVRSRV_BRIDGE_RETURN o = { 0 };
		TRY("initsrv connect", pvr_call(&p, PVRSRV_BRIDGE_INITSRV_CONNECT, NULL, 0, &o, sizeof o));
		connected = 1;
	}
	{
		static PVRSRV_BRIDGE_OUT_ENUMDEVICE o;
		IMG_UINT32 i;

		TRY("enum devices", pvr_call(&p, PVRSRV_BRIDGE_ENUM_DEVICES, NULL, 0, &o, sizeof o));
		for (i = 0; i < o.ui32NumDevices; i++)
			if (o.asDeviceIdentifier[i].eDeviceType == PVRSRV_DEVICE_TYPE_SGX &&
			    o.asDeviceIdentifier[i].eDeviceClass == PVRSRV_DEVICE_CLASS_3D)
				break;
		TRY("find SGX", i == o.ui32NumDevices ? -ENODEV : 0);
		TRY("acquire", pvr_acquire(&p, o.asDeviceIdentifier[i].ui32DeviceIndex, PVRSRV_DEVICE_TYPE_UNKNOWN));
	}
	{
		PVRSRV_BRIDGE_IN_SGXINFO_FOR_SRVINIT i = { .hDevCookie = p.devcookie };
		static PVRSRV_BRIDGE_OUT_SGXINFO_FOR_SRVINIT o;

		TRY("SGX info", pvr_call(&p, PVRSRV_BRIDGE_SGXINFO_FOR_SRVINIT, &i, sizeof i, &o, sizeof o));
		s.pd = (uint32_t)o.sInitInfo.sPDDevPAddr.uiAddr;
		memcpy(p.heaps, o.sInitInfo.asHeapInfo, sizeof p.heaps);
		p.nheaps = PVRSRV_MAX_CLIENT_HEAPS;
		for (int h = 0; h < PVRSRV_MAX_CLIENT_HEAPS; h++)
			if (p.heaps[h].ui32HeapID != (IMG_UINT32)PVRSRV_UNDEFINED_HEAP_ID)
				heap_base(&s, p.heaps[h].ui32HeapID & 0xffffff, p.heaps[h].sDevVAddrBase.uiAddr);
		TRY("heaps", s.code_heap && s.pixel_heap && s.vertex_heap && s.pds_pixel_heap ? 0 : -ENOENT);
	}
	for (int i = 0; i < NBUF; i++) {
		step = bufs[i].name;
		if ((err = pvr_alloc(&p, &s.b[i], bufs[i].heap, bufs[i].attribs, bufs[i].size, bufs[i].align)) ||
		    (err = pvr_map(&p, &s.b[i])))
			goto fail;
	}
	s.hwperf_flags = apphint("HWPerfFlags");
	TRY("build", build(&s, fw, &in2.sInitInfo));
	fill_handles(&s, &in2.sInitInfo);
	in2.hDevCookie = p.devcookie;

	/* As libsrv_um does: drop every CPU mapping before the kernel takes the buffers. */
	for (int i = 0; i < NBUF; i++) {
		PVRSRV_BRIDGE_IN_RELEASE_MMAP_DATA ri = { .hMHandle = s.b[i].mem };
		PVRSRV_BRIDGE_OUT_RELEASE_MMAP_DATA ro = { 0 };

		TRY("release mmap", pvr_call(&p, PVRSRV_BRIDGE_RELEASE_MMAP_DATA, &ri, sizeof ri, &ro, sizeof ro));
		if (ro.bMUnmap)
			munmap((void *)((uintptr_t)s.b[i].cpu & ~(uintptr_t)0xfff), ro.ui32RealByteSize);
		s.b[i].cpu = NULL;
	}

	TRY("devinitpart2", pvr_call(&p, PVRSRV_BRIDGE_SGX_DEVINITPART2, &in2, sizeof in2, &out2, sizeof out2));
	if (out2.ui32KMBuildOptions != in2.sInitInfo.ui32ClientBuildOptions)
		fprintf(stderr, "srvinit: kernel build options 0x%x, microkernel's 0x%x\n",
			out2.ui32KMBuildOptions, in2.sInitInfo.ui32ClientBuildOptions);
	step = "initsrv disconnect";
	{
		/* This is where the kernel resets the core and starts the microkernel. */
		PVRSRV_BRIDGE_IN_INITSRV_DISCONNECT i = { .bInitSuccesful = IMG_TRUE };

		connected = 0;
		if ((err = pvr_call(&p, PVRSRV_BRIDGE_INITSRV_DISCONNECT, &i, sizeof i, &ret, sizeof ret)))
			goto fail;
	}
	pvr_call(&p, PVRSRV_BRIDGE_DISCONNECT_SERVICES, NULL, 0, &ret, sizeof ret);
	printf("srvinit: SGX545 microkernel started (ukernel at 0x%08x, pd 0x%08x)\n", va(&s, B_UKERNEL), s.pd);
	return 0;

fail:
	fprintf(stderr, "srvinit: %s failed: %d\n", step, err);
	if (connected) {
		PVRSRV_BRIDGE_IN_INITSRV_DISCONNECT i = { .bInitSuccesful = IMG_FALSE };
		pvr_call(&p, PVRSRV_BRIDGE_INITSRV_DISCONNECT, &i, sizeof i, &ret, sizeof ret);
	}
	return 1;
}
