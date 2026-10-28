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
 * The only Imagination bytes are the USSE microkernel, from the firmware file
 * that fetch-firmware.sh pulls from your own DDK. The PDS programs that enter
 * it are assembled here, and every address is computed here.
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
/* The firmware file is the microkernel alone: libsrv_init .rodata 0x2000..0xef68. */
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
} bufs[NBUF] = {
	[B_CCB] = { "ccb", KD, RW, 0x2000, 0x1000 },
	[B_CCBCTL] = { "ccbctl", KD, RW, 0x8, 0x1000 },
	[B_KICKER] = { "kicker", KD, RW, 0x4, 0x10 },
	[B_TA3DCTL] = { "ta3dctl", KD, RW | PVRSRV_HAP_MULTI_PROCESS, 0xec, 0x20 },
	[B_HOSTCTL] = { "hostctl", KD, RW | PVRSRV_HAP_MULTI_PROCESS, 0x44, 0x20 },
	[B_MISC] = { "misc", KD, RW | PVRSRV_HAP_MULTI_PROCESS, 0x54, 0x20 },
	[B_HWPERF] = { "hwperf", KD, RW, 0x640c, 0x20 },
	[B_TASIG] = { "tasig", KD, RW, 0x620, 0x20 },
	[B_3DSIG] = { "3dsig", KD, RW, 0x2088, 0x20 },
	[B_UKERNEL] = { "ukernel", KC, RO, UKERNEL_SIZE, 0x8 },
	[B_PDS0] = { "pds0", KC, RO, 0x18, 0x10 },
	[B_PDS1] = { "pds1", KC, RO, 0x38, 0x10 },
	[B_PDS2] = { "pds2", KC, RO, 0x18, 0x10 },
	[B_PDS3] = { "pds3", KC, RO, 0x100, 0x10 },
	[B_PDS4] = { "pds4", KC, RO, 0x68, 0x10 },
};


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

/*
 * PDS assembly (re/pds/PDS.md). A program is its data segment, 16-byte rows of
 * DS0 DS0 DS1 DS1, then 32-bit code. Operands are unified register numbers:
 * DS0 words 0x00-0x3f, DS1 words 0x40-0x7f, the inputs from 0x80 (which are
 * DS0 words 0x40 on).
 */
#define D0(n) (n)
#define D1(n) (0x40 + (n))
#define IN(n) (0x80 + (n))
#define T0 D0(0x30)		/* scratch words */
#define T1 D1(0x30)
#define CC_P0 0
#define CC_ALWAYS 7
#define DOUTD 3
#define DOUTU 5
#define DOUTA 6

struct pds {
	uint32_t *w;
	unsigned data;		/* data segment size in words */
	unsigned pc;		/* next code word */
};

static void ds(struct pds *p, unsigned r, uint32_t v)
{
	unsigned k = r & 0x3f;

	p->w[(k >> 1) * 4 + (r >= 0x40 ? 2 : 0) + (k & 1)] = v;
}

static void emit(struct pds *p, uint32_t i) { p->w[p->data + p->pc++] = i; }

/* A DS0-side word index (inputs follow DS0 word 0x3f). */
static unsigned ds0w(unsigned r) { return r >= 0x80 ? r - 0x40 : r; }

/* MOVS: four words, from one DS0 pair and one DS1 pair, to a DOUT destination. */
static void movs(struct pds *p, unsigned dest, unsigned a, unsigned b, unsigned c, unsigned d)
{
	unsigned src[4] = { a, b, c, d }, r1 = 0, r2 = 0, sel = 0;

	for (int i = 0; i < 4; i++) {
		unsigned s;

		if (src[i] >= 0x40 && src[i] < 0x80) {
			r2 = (src[i] - 0x40) >> 1;
			s = 2 + (src[i] & 1);
		} else {
			r1 = ds0w(src[i]) >> 1;
			s = ds0w(src[i]) & 1;
		}
		sel |= s << (11 - 2 * i);
	}
	emit(p, (uint32_t)CC_ALWAYS << 24 | r1 << 18 | r2 << 13 | sel | dest);
}

/* Two-source ALU op: d = a (DS0 side) op b (DS1 side); sources in 16-bit units. */
static void alu(struct pds *p, unsigned cc, unsigned op, unsigned d, unsigned a, unsigned b)
{
	emit(p, op << 24 | cc << 24 | ds0w(a) * 2 << 16 | (b - 0x40) * 2 << 9 | d);
}

#define AND 0xc8	/* op 0xc, bit 27 */
#define XOR 0xd0

/* p0 = (r == 0) */
static void tst(struct pds *p, unsigned cc, unsigned r)
{
	if (r >= 0x40 && r < 0x80)
		emit(p, 0x80000000u | cc << 24 | (r - 0x40) * 2 << 9 | 1 << 8);
	else
		emit(p, 0x80000000u | cc << 24 | ds0w(r) * 2 << 16);
}

static void bra(struct pds *p, unsigned cc, unsigned target) { emit(p, 0x90000000u | cc << 24 | target); }
static void halt(struct pds *p) { emit(p, 0xaf000000u); }

static struct pds prog(struct init *s, int i, unsigned data_words)
{
	return (struct pds){ w(s, i), data_words, 0 };
}

/*
 * Start the microkernel at offset e: DOUTU with word 0 from doutu(), word 1 = 2,
 * word 2 = 0.
 */
static void pds_entry(struct init *s, int i, uint32_t e)
{
	struct pds p = prog(s, i, 4);

	ds(&p, D0(0), doutu(s, va(s, B_UKERNEL) + e));
	ds(&p, D0(1), 2);
	movs(&p, DOUTU, D0(0), D0(1), D1(0), D0(0));
	halt(&p);
}

/* pds1: if in0 == 0, first a DOUTD from the TA3D control block (control word 0x3c0003f), then enter at 0xdf0. */
static void pds_ta3d(struct init *s)
{
	struct pds p = prog(s, B_PDS1, 8);
	uint32_t off = va(s, B_UKERNEL) + 0xdf0 - s->code_heap;

	ds(&p, D0(0), va(s, B_TA3DCTL));
	ds(&p, D0(1), 0x3c0003f);
	/* As libsrv_init writes it: no bit 27 and no segment subtracted, unlike doutu(). */
	ds(&p, D0(2), ((off * 2) & 0xffff0) | (((off >> 19) + 15) & 15));
	ds(&p, D0(3), 2);
	tst(&p, CC_ALWAYS, IN(0));
	bra(&p, CC_P0, 3);
	bra(&p, CC_ALWAYS, 4);
	movs(&p, DOUTD, D0(0), D0(1), D0(0), D0(0));
	movs(&p, DOUTU, D0(2), D0(3), D1(0), D0(2));
	halt(&p);
}

/*
 * pds3: an event dispatcher. in0, in2, in4 go to the task's PA0-PA2; the event
 * bits in in1 (low 24) pick the handler: any of 0x100 -> 0x30; otherwise the
 * first of 0x400090, 0x200, 0x300400 that has a bit set selects 0x7050, 0x2cd0
 * or 0x5670, but only if no other bit is set and in0 is 0; anything else -> 0x30.
 */
static void pds_events(struct init *s)
{
	struct pds p = prog(s, B_PDS3, 24);
	uint32_t uk = va(s, B_UKERNEL);
	static const struct { uint32_t mask, entry; } ev[3] = {
		{ 0x400090, 0x7050 }, { 0x200, 0x2cd0 }, { 0x300400, 0x5670 },
	};

	ds(&p, D0(0), 2);
	ds(&p, D1(0), 0x000);
	ds(&p, D1(1), 0x100);
	ds(&p, D1(2), 0x200);
	ds(&p, D1(6), doutu(s, uk + 0x30));
	ds(&p, D1(7), 0xffffff);
	ds(&p, D1(8), 0x100);
	for (int i = 0; i < 3; i++) {
		ds(&p, D1(3 + i), doutu(s, uk + ev[i].entry));
		ds(&p, D1(9 + i), ev[i].mask);
	}
	movs(&p, DOUTA, IN(0), D1(0), IN(0), IN(0));
	movs(&p, DOUTA, IN(2), D1(1), IN(2), IN(2));
	movs(&p, DOUTA, IN(4), D1(2), IN(4), IN(4));
	alu(&p, CC_ALWAYS, AND, T0, IN(1), D1(7));
	alu(&p, CC_ALWAYS, AND, T1, T0, D1(8));
	tst(&p, CC_ALWAYS, T1);
	bra(&p, CC_P0, 8);
	bra(&p, CC_ALWAYS, 38);
	for (int i = 0; i < 3; i++) {
		unsigned next = p.pc + 10;	/* the next mask test */

		alu(&p, CC_ALWAYS, AND, T1, T0, D1(9 + i));
		tst(&p, CC_ALWAYS, T1);
		bra(&p, CC_P0, i < 2 ? next : 38);
		if (i == 0) {
			/* the first test keeps the order xor, tst, (p0) tst in0 */
			alu(&p, CC_ALWAYS, XOR, T1, T0, T1);
			tst(&p, CC_ALWAYS, T1);
			tst(&p, CC_P0, IN(0));
		} else {
			tst(&p, CC_ALWAYS, IN(0));
			alu(&p, CC_P0, XOR, T1, T0, T1);
			tst(&p, CC_P0, T1);
		}
		bra(&p, CC_P0, p.pc + 2);
		bra(&p, CC_ALWAYS, 38);
		movs(&p, DOUTU, D1(3 + i), D0(0), D0(1), D0(0));
		halt(&p);
	}
	movs(&p, DOUTU, D1(6), D0(0), D0(1), D0(0));
	halt(&p);
}

/* pds4: in0 and in4 to PA0 and PA2, then in0 & 0x4000f -> 0x73a0, in0 & 0x120080 -> 0x2ea0, else 0x5740. */
static void pds_events2(struct init *s)
{
	struct pds p = prog(s, B_PDS4, 12);
	uint32_t uk = va(s, B_UKERNEL);

	ds(&p, D0(0), doutu(s, uk + 0x73a0));
	ds(&p, D0(1), 2);
	ds(&p, D0(2), doutu(s, uk + 0x2ea0));
	ds(&p, D0(3), 2);
	ds(&p, D0(4), doutu(s, uk + 0x5740));
	ds(&p, D0(5), 2);
	ds(&p, D1(0), 0x000);
	ds(&p, D1(1), 0x200);
	ds(&p, D1(3), 0x4000f);
	ds(&p, D1(4), 0x120080);
	movs(&p, DOUTA, IN(0), D1(0), IN(0), IN(0));
	movs(&p, DOUTA, IN(4), D1(1), IN(4), IN(4));
	for (int i = 0; i < 2; i++) {
		alu(&p, CC_ALWAYS, AND, T0, IN(0), D1(3 + i));
		tst(&p, CC_ALWAYS, T0);
		bra(&p, CC_P0, p.pc + 3);
		movs(&p, DOUTU, D0(2 * i), D0(2 * i + 1), D1(2), D0(2 * i));
		halt(&p);
	}
	movs(&p, DOUTU, D0(4), D0(5), D1(2), D0(4));
	halt(&p);
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

	for (int i = 0; i < NBUF; i++)
		memset(s->b[i].cpu, 0, bufs[i].size);
	memcpy(w(s, B_UKERNEL), fw, UKERNEL_SIZE);

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

	/* The PDS programs that start the microkernel. */
	pds_entry(s, B_PDS0, 0xd60);	/* the event program (script register 0xa68) */
	pds_ta3d(s);
	pds_entry(s, B_PDS2, 0xe20);
	pds_events(s);
	pds_events2(s);

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
	uint8_t *d = malloc(UKERNEL_SIZE + 1);
	size_t n;

	if (!f || !d) {
		fprintf(stderr, "srvinit: %s: %s\n", path, strerror(errno));
		exit(2);
	}
	n = fread(d, 1, UKERNEL_SIZE + 1, f);
	fclose(f);
	if (n != UKERNEL_SIZE) {
		fprintf(stderr, "srvinit: %s: %zu bytes, want %u (run fetch-firmware.sh)\n", path, n, UKERNEL_SIZE);
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
	/*
	 * Drivers before the dispatch-table fix expect sizeof(PVRSRV_BRIDGE_RETURN)
	 * of OUT for DEVINITPART2 (16 bytes on x86_64), though the handler only
	 * writes the 8-byte PVRSRV_BRIDGE_OUT_SGXDEVINITPART2; i386 never noticed,
	 * both are 8 there. The size check runs before the handler, so a rejected
	 * call changes nothing: send 8, and 16 if the driver refuses it.
	 */
	union {
		PVRSRV_BRIDGE_OUT_SGXDEVINITPART2 o;
		PVRSRV_BRIDGE_RETURN r;
	} out2 = { 0 };
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

	err = pvr_call(&p, PVRSRV_BRIDGE_SGX_DEVINITPART2, &in2, sizeof in2, &out2, sizeof out2.o);
	if (err == -EINVAL)
		err = pvr_call(&p, PVRSRV_BRIDGE_SGX_DEVINITPART2, &in2, sizeof in2, &out2, sizeof out2.r);
	TRY("devinitpart2", err);
	if (out2.o.ui32KMBuildOptions != in2.sInitInfo.ui32ClientBuildOptions)
		fprintf(stderr, "srvinit: kernel build options 0x%x, microkernel's 0x%x\n",
			out2.o.ui32KMBuildOptions, in2.sInitInfo.ui32ClientBuildOptions);
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
