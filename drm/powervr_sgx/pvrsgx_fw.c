// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * Loading and starting the microkernel: what srvinit and the DDK services did
 * between them, now in the kernel. docs/SRVINIT.md is the reference for every
 * buffer, constant, PDS program and script value here, and docs/UKERNEL.md for
 * how the microkernel uses them.
 *
 * The host interface is SGXMKIF as of DDK 1.7 (decision D8). Two images speak
 * it: the DDK 1.7.862890 build, whose entry points and patch sites are fixed
 * here, and the open microkernel (ukernel/), whose file header lists its own
 * (docs/UKERNEL.md 4a).
 */

#include <drm/drm_print.h>

#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/seq_buf.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <uapi/drm/powervr_sgx_drm.h>

#include "pvrsgx_ccb.h"
#include "pvrsgx_device.h"
#include "pvrsgx_fw.h"
#include "pvrsgx_kobj.h"
#include "pvrsgx_mmu.h"
#include "pvrsgx_vm.h"

#define FW_OPEN		"powervr_sgx/sgx545-openhc-ukernel.bin"
#define FW_DDK		"powervr_sgx/sgx545-ukernel-1.7.862890.bin"

static char *firmware;
module_param(firmware, charp, 0444);
MODULE_PARM_DESC(firmware,
		 "Microkernel image under /lib/firmware (default: " FW_OPEN ", then " FW_DDK ")");
MODULE_FIRMWARE(FW_OPEN);
MODULE_FIRMWARE(FW_DDK);

static unsigned int core_clock = 400000000;
module_param(core_clock, uint, 0444);
MODULE_PARM_DESC(core_clock, "SGX core clock in Hz; sets the microkernel timer (default 400 MHz)");

/* -1: only with cleanup v2; on the DDK microkernel RT and PB cleanups never complete. */
static int cleanup = -1;
module_param(cleanup, int, 0644);
MODULE_PARM_DESC(cleanup, "Ask the microkernel to drop RT datasets, render contexts and PBs before freeing them (-1: when it speaks cleanup v2, 0 never, 1 always)");

static bool restart_quiesce = true;
module_param(restart_quiesce, bool, 0644);
MODULE_PARM_DESC(restart_quiesce, "Before a restart, run the deinit script, hold the cores in reset and drop bus mastering, as an unload does (default on)");

/* The DDK microkernel: libsrv_init's .rodata 0x2000..0xef68. */
#define UKERNEL_SIZE		0xcf68

/* The open microkernel's file (docs/UKERNEL.md 4a). */
#define UKFW_MAGIC0		0x35584753	/* "SGX5" */
#define UKFW_MAGIC1		0x4e524b55	/* "UKRN" */
#define UKFW_HEADER_MIN		0x60
#define UKFW_IF_MAJOR		1
#define UKFW_FEATURES		(PVRSGX_UKFW_CLEANUP_V2 | PVRSGX_UKFW_SECURE)
#define UKFW_MAX_IMAGE		(CODE_ARENA_SIZE - PDS_BYTES - 0x10)

struct ukfw_header {
	__le32 magic[2];
	__le32 header_size;
	__le32 interface;
	__le32 features;
	__le32 image_size;
	__le32 image_crc32;
	__le32 n_entries;
	__le32 entries_off;
	__le32 n_patches;
	__le32 patches_off;
	__le32 core_rev;
	__le32 reserved[4];
	char build_id[32];
};

struct ukfw_pair {
	__le32 a, b;
};

enum ukfw_patch_value {
	UKFW_TA3DCTL = 1,
	UKFW_HOSTCTL,
	UKFW_CCBCTL,
	UKFW_CCB,
	UKFW_MISC,
};

/* Microkernel timer at 1 kHz, its lockup check at 100 Hz. */
#define UKERNEL_TIMER_HZ	1000
#define HWR_CHECK_HZ		100

/* SGX_MISCINFO_STRUCT_SIZES this build was made with. */
static const u32 struct_sizes[] = {
	0x2b0,	/* CMDTA */
	0x1e0,	/* CMDTA_SHARED */
	0x4c8,	/* TRANSFERCMD */
	0xdc,	/* TRANSFERCMD_SHARED */
	0xd0,	/* 3DREGISTERS */
	0x3c,	/* HWPBDESC */
	0x44,	/* HWRENDERCONTEXT */
	0x1ec,	/* HWRENDERDETAILS */
	0x38,	/* HWRTDATA */
	0x50,	/* HWRTDATASET */
	0xb4,	/* HWTRANSFERCONTEXT */
	0x44,	/* HOST_CTL */
	0x20,	/* COMMAND */
};

/* Host-kick entries in instructions, per command type. */
static const u32 host_kick[PVRSGX_CMD_MAX] = {
	[PVRSGX_CMD_TA] = 0x11d,
	[PVRSGX_CMD_TRANSFER] = 0x103,
	[PVRSGX_CMD_POWER] = 0xd4,
	[PVRSGX_CMD_CONTEXTSUSPEND] = 0xeb,
	[PVRSGX_CMD_CLEANUP] = 0x13a,
	[PVRSGX_CMD_GETMISCINFO] = 0x173,
	[PVRSGX_CMD_PROCESS_QUEUES] = 0x156,
	[PVRSGX_CMD_SETHWPERFSTATUS] = 0x157,
};

/*
 * Where each buffer lives: the addresses the DDK's allocator gave on an EA3,
 * kept so traces compare one for one with a DDK boot.
 */
static const struct {
	bool code;
	u32 offset, size;
} layout[PVRSGX_FW_NBUF] = {
	[PVRSGX_FW_CCB] = { false, 0x0000, 0x2000 },
	[PVRSGX_FW_CCBCTL] = { false, 0x2000, 0x8 },
	[PVRSGX_FW_KICKER] = { false, 0x2010, 0x4 },
	[PVRSGX_FW_TA3DCTL] = { false, 0x3000, 0xec },
	[PVRSGX_FW_HOSTCTL] = { false, 0x3100, 0x44 },
	[PVRSGX_FW_MISC] = { false, 0x3160, 0x54 },
	[PVRSGX_FW_HWPERF] = { false, 0x4000, 0x640c },
	[PVRSGX_FW_TASIG] = { false, 0xa420, 0x620 },
	[PVRSGX_FW_3DSIG] = { false, 0xb000, 0x2088 },
	/* The image at 0, then the PDS programs from its end (0xcf70 for the DDK's). */
	[PVRSGX_FW_UKERNEL] = { true, 0x0000, 0 },
	[PVRSGX_FW_PDS0] = { true, 0x000, 0x18 },
	[PVRSGX_FW_PDS1] = { true, 0x020, 0x38 },
	[PVRSGX_FW_PDS2] = { true, 0x060, 0x18 },
	[PVRSGX_FW_PDS3] = { true, 0x090, 0x100 },
	[PVRSGX_FW_PDS4] = { true, 0x190, 0x68 },
};

#define PDS_BYTES	0x200

#define DATA_ARENA_SIZE	0xe000
#define CODE_ARENA_SIZE	0xe000

/* Registers the microkernel samples to tell a hung core from a busy one. */
static const u32 ta_sig[] = { 0x304, 0x26c, 0x270, 0x2a0, 0x2a4, 0x378 };
static const u32 sig_3d[] = {
	0x438, 0x4cc, 0x4d0, 0x4d4, 0x4d8, 0x510, 0x514, 0x518,
	0x51c, 0x524, 0x528, 0x52c, 0x530, 0x534, 0x538, 0x53c,
	0x540, 0x544, 0x550, 0x554, 0x558, 0x55c, 0x84c, 0x850,
	0x854, 0x858, 0x560, 0x564, 0x568, 0x56c, 0x308, 0x30c,
};

static u32 *fw_words(struct pvrsgx_device *sdev, enum pvrsgx_fw_buf b)
{
	return sdev->fw.cpu[b];
}

/* The TA3D control block as the microkernel left it (in-flight slot, PB state). */
void pvrsgx_fw_dump(struct seq_buf *sb, struct pvrsgx_device *sdev, unsigned int b,
		    const char *name)
{
	const u32 *w = fw_words(sdev, b);

	seq_buf_printf(sb, "%s @ 0x%08x:", name, sdev->fw.addr[b]);
	for (u32 i = 0; i < layout[b].size / 4; i++)
		seq_buf_printf(sb, "%s%08x", i % 8 ? " " : "\n  ", READ_ONCE(w[i]));
	seq_buf_puts(sb, "\n");
}

u32 pvrsgx_fw_hostctl(struct pvrsgx_device *sdev, unsigned int word)
{
	return READ_ONCE(fw_words(sdev, PVRSGX_FW_HOSTCTL)[word]);
}

/*
 * DOUTU word 0 for microkernel code at device address a: code base 15 plus the
 * 512 KiB segment, then the offset in the segment in 8-byte instructions, << 4.
 * Bit 27 is set on every microkernel entry.
 */
static u32 doutu(u32 a)
{
	u32 off = a - PVRSGX_HEAP_KERNEL_CODE, seg = off >> 19;

	return ((seg + 15) & 15) | 0x8000000 | (((off - (seg << 19)) * 2) & 0x3ffff0);
}

/*
 * Fill one of the microkernel's limm placeholders (word 0 0x?dbeef, the value
 * bits of word 1 clear; 0xfc237150 in the DDK's): bits 0-20 to word 0, 21-25 to
 * word 1 bits 4-8, 26-31 to word 1 bits 12-17.
 */
static int limm(struct pvrsgx_device *sdev, u32 off, u32 v)
{
	u32 *i = (u32 *)((u8 *)sdev->fw.cpu[PVRSGX_FW_UKERNEL] + off);
	bool ok = (i[0] & 0x1fffff) == 0xdbeef &&
		  (sdev->fw.ddk ? i[1] == 0xfc237150 : !(i[1] & 0x0003f1f0));

	if (off > sdev->fw.image_size - 8 || !ok) {
		drm_err(&sdev->base, "%s: no patch placeholder at +0x%x\n", sdev->fw.name, off);
		return -EINVAL;
	}
	i[0] = (i[0] & 0xffe00000) | (v & 0x1fffff);
	i[1] = (i[1] & 0xfffc0e0f) | ((v >> 17) & 0x1f0) | ((v >> 26) << 12);
	return 0;
}

/*
 * PDS assembly (docs/PDS.md). A program is its data segment in 16-byte rows of
 * DS0 DS0 DS1 DS1, then code. Operands are unified register numbers: DS0 words
 * 0x00-0x3f, DS1 words 0x40-0x7f, inputs from 0x80 (DS0 words 0x40 on).
 */
#define D0(n)		(n)
#define D1(n)		(0x40 + (n))
#define IN(n)		(0x80 + (n))
#define T0		D0(0x30)
#define T1		D1(0x30)
#define CC_P0		0
#define CC_ALWAYS	7
#define DOUTD		3
#define DOUTU		5
#define DOUTA		6
#define ALU_AND		0xc8
#define ALU_XOR		0xd0

struct pds {
	u32 *w;
	unsigned int data;	/* data segment words */
	unsigned int pc;
};

static void pds_ds(struct pds *p, unsigned int r, u32 v)
{
	unsigned int k = r & 0x3f;

	p->w[(k >> 1) * 4 + (r >= 0x40 ? 2 : 0) + (k & 1)] = v;
}

static void pds_emit(struct pds *p, u32 i)
{
	p->w[p->data + p->pc++] = i;
}

static unsigned int ds0w(unsigned int r)
{
	return r >= 0x80 ? r - 0x40 : r;
}

/* MOVS: four words from one DS0 pair and one DS1 pair to a DOUT destination. */
static void pds_movs(struct pds *p, unsigned int dest, unsigned int a, unsigned int b,
		     unsigned int c, unsigned int d)
{
	unsigned int src[4] = { a, b, c, d }, r1 = 0, r2 = 0, sel = 0;

	for (int i = 0; i < 4; i++) {
		unsigned int s;

		if (src[i] >= 0x40 && src[i] < 0x80) {
			r2 = (src[i] - 0x40) >> 1;
			s = 2 + (src[i] & 1);
		} else {
			r1 = ds0w(src[i]) >> 1;
			s = ds0w(src[i]) & 1;
		}
		sel |= s << (11 - 2 * i);
	}
	pds_emit(p, (u32)CC_ALWAYS << 24 | r1 << 18 | r2 << 13 | sel | dest);
}

/* d = a (DS0 side) op b (DS1 side); sources in 16-bit units. */
static void pds_alu(struct pds *p, unsigned int cc, unsigned int op, unsigned int d,
		    unsigned int a, unsigned int b)
{
	pds_emit(p, op << 24 | cc << 24 | ds0w(a) * 2 << 16 | (b - 0x40) * 2 << 9 | d);
}

/* p0 = (r == 0) */
static void pds_tst(struct pds *p, unsigned int cc, unsigned int r)
{
	if (r >= 0x40 && r < 0x80)
		pds_emit(p, 0x80000000u | cc << 24 | (r - 0x40) * 2 << 9 | 1 << 8);
	else
		pds_emit(p, 0x80000000u | cc << 24 | ds0w(r) * 2 << 16);
}

static void pds_bra(struct pds *p, unsigned int cc, unsigned int target)
{
	pds_emit(p, 0x90000000u | cc << 24 | target);
}

static void pds_halt(struct pds *p)
{
	pds_emit(p, 0xaf000000u);
}

static struct pds pds_prog(struct pvrsgx_device *sdev, enum pvrsgx_fw_buf b, unsigned int data)
{
	return (struct pds){ fw_words(sdev, b), data, 0 };
}

/* Start the microkernel at offset e: DOUTU with word 1 = 2. */
static void pds_entry(struct pvrsgx_device *sdev, enum pvrsgx_fw_buf b, u32 e)
{
	struct pds p = pds_prog(sdev, b, 4);

	pds_ds(&p, D0(0), doutu(sdev->fw.addr[PVRSGX_FW_UKERNEL] + e));
	pds_ds(&p, D0(1), 2);
	pds_movs(&p, DOUTU, D0(0), D0(1), D1(0), D0(0));
	pds_halt(&p);
}

/* pds1: if in0 == 0, DMA the control block into sa first (DOUTD 0x3c0003f); then enter at 0xdf0. */
static void pds_ta3d(struct pvrsgx_device *sdev)
{
	struct pds p = pds_prog(sdev, PVRSGX_FW_PDS1, 8);
	u32 off = sdev->fw.addr[PVRSGX_FW_UKERNEL] + 0xdf0 - PVRSGX_HEAP_KERNEL_CODE;

	pds_ds(&p, D0(0), sdev->fw.addr[PVRSGX_FW_TA3DCTL]);
	pds_ds(&p, D0(1), 0x3c0003f);
	/* No bit 27 and no segment subtracted here, unlike doutu(). */
	pds_ds(&p, D0(2), ((off * 2) & 0xffff0) | (((off >> 19) + 15) & 15));
	pds_ds(&p, D0(3), 2);
	pds_tst(&p, CC_ALWAYS, IN(0));
	pds_bra(&p, CC_P0, 3);
	pds_bra(&p, CC_ALWAYS, 4);
	pds_movs(&p, DOUTD, D0(0), D0(1), D0(0), D0(0));
	pds_movs(&p, DOUTU, D0(2), D0(3), D1(0), D0(2));
	pds_halt(&p);
}

/*
 * pds3, the event dispatcher: in0, in2, in4 to PA0-PA2; event bits in in1 pick
 * the handler. Any of 0x100 -> 0x30. Otherwise the first of 0x400090, 0x200,
 * 0x300400 with a bit set selects 0x7050, 0x2cd0 or 0x5670, if no other bit is
 * set and in0 is 0. Anything else -> 0x30.
 */
static void pds_events(struct pvrsgx_device *sdev)
{
	static const struct { u32 mask, entry; } ev[3] = {
		{ 0x400090, 0x7050 }, { 0x200, 0x2cd0 }, { 0x300400, 0x5670 },
	};
	struct pds p = pds_prog(sdev, PVRSGX_FW_PDS3, 24);
	u32 uk = sdev->fw.addr[PVRSGX_FW_UKERNEL];

	pds_ds(&p, D0(0), 2);
	pds_ds(&p, D1(0), 0x000);
	pds_ds(&p, D1(1), 0x100);
	pds_ds(&p, D1(2), 0x200);
	pds_ds(&p, D1(6), doutu(uk + 0x30));
	pds_ds(&p, D1(7), 0xffffff);
	pds_ds(&p, D1(8), 0x100);
	for (int i = 0; i < 3; i++) {
		pds_ds(&p, D1(3 + i), doutu(uk + ev[i].entry));
		pds_ds(&p, D1(9 + i), ev[i].mask);
	}
	pds_movs(&p, DOUTA, IN(0), D1(0), IN(0), IN(0));
	pds_movs(&p, DOUTA, IN(2), D1(1), IN(2), IN(2));
	pds_movs(&p, DOUTA, IN(4), D1(2), IN(4), IN(4));
	pds_alu(&p, CC_ALWAYS, ALU_AND, T0, IN(1), D1(7));
	pds_alu(&p, CC_ALWAYS, ALU_AND, T1, T0, D1(8));
	pds_tst(&p, CC_ALWAYS, T1);
	pds_bra(&p, CC_P0, 8);
	pds_bra(&p, CC_ALWAYS, 38);
	for (int i = 0; i < 3; i++) {
		unsigned int next = p.pc + 10;

		pds_alu(&p, CC_ALWAYS, ALU_AND, T1, T0, D1(9 + i));
		pds_tst(&p, CC_ALWAYS, T1);
		pds_bra(&p, CC_P0, i < 2 ? next : 38);
		if (i == 0) {
			pds_alu(&p, CC_ALWAYS, ALU_XOR, T1, T0, T1);
			pds_tst(&p, CC_ALWAYS, T1);
			pds_tst(&p, CC_P0, IN(0));
		} else {
			pds_tst(&p, CC_ALWAYS, IN(0));
			pds_alu(&p, CC_P0, ALU_XOR, T1, T0, T1);
			pds_tst(&p, CC_P0, T1);
		}
		pds_bra(&p, CC_P0, p.pc + 2);
		pds_bra(&p, CC_ALWAYS, 38);
		pds_movs(&p, DOUTU, D1(3 + i), D0(0), D0(1), D0(0));
		pds_halt(&p);
	}
	pds_movs(&p, DOUTU, D1(6), D0(0), D0(1), D0(0));
	pds_halt(&p);
}

/* pds4: in0, in4 to PA0, PA2; in0 & 0x4000f -> 0x73a0, in0 & 0x120080 -> 0x2ea0, else 0x5740. */
static void pds_events2(struct pvrsgx_device *sdev)
{
	struct pds p = pds_prog(sdev, PVRSGX_FW_PDS4, 12);
	u32 uk = sdev->fw.addr[PVRSGX_FW_UKERNEL];

	pds_ds(&p, D0(0), doutu(uk + 0x73a0));
	pds_ds(&p, D0(1), 2);
	pds_ds(&p, D0(2), doutu(uk + 0x2ea0));
	pds_ds(&p, D0(3), 2);
	pds_ds(&p, D0(4), doutu(uk + 0x5740));
	pds_ds(&p, D0(5), 2);
	pds_ds(&p, D1(0), 0x000);
	pds_ds(&p, D1(1), 0x200);
	pds_ds(&p, D1(3), 0x4000f);
	pds_ds(&p, D1(4), 0x120080);
	pds_movs(&p, DOUTA, IN(0), D1(0), IN(0), IN(0));
	pds_movs(&p, DOUTA, IN(4), D1(1), IN(4), IN(4));
	for (int i = 0; i < 2; i++) {
		pds_alu(&p, CC_ALWAYS, ALU_AND, T0, IN(0), D1(3 + i));
		pds_tst(&p, CC_ALWAYS, T0);
		pds_bra(&p, CC_P0, p.pc + 3);
		pds_movs(&p, DOUTU, D0(2 * i), D0(2 * i + 1), D1(2), D0(2 * i));
		pds_halt(&p);
	}
	pds_movs(&p, DOUTU, D0(4), D0(5), D1(2), D0(4));
	pds_halt(&p);
}

/*
 * The microkernel's control block, rebuilt at every start: after a reset it
 * must not find the previous run's render in flight (UKERNEL.md 5a, "discard").
 */
static void ta3dctl_fill(struct pvrsgx_device *sdev)
{
	struct pvrsgx_fw *f = &sdev->fw;
	u32 *t = fw_words(sdev, PVRSGX_FW_TA3DCTL);

	memset(t, 0, layout[PVRSGX_FW_TA3DCTL].size);
	t[0x00] = f->addr[PVRSGX_FW_TA3DCTL];
	t[0x01] = f->addr[PVRSGX_FW_HOSTCTL];
	t[0x02] = f->addr[PVRSGX_FW_CCBCTL];
	t[0x13] = 0;	/* HWPerf flags */
	t[0x16] = f->addr[PVRSGX_FW_CCB];
	t[0x17] = pvrsgx_mmu_pd_addr(sdev->kernel_mmu);
	t[0x2b] = f->addr[PVRSGX_FW_HWPERF];
	t[0x2c] = f->addr[PVRSGX_FW_TASIG];
	t[0x2d] = f->addr[PVRSGX_FW_3DSIG];
}

/*
 * The open microkernel's entry programs and patches, from its file header.
 * A PDS program it doesn't list enters where program 0 does: its event entry
 * dispatches every event itself.
 */
static int fw_build_open(struct pvrsgx_device *sdev)
{
	static const enum pvrsgx_fw_buf value_buf[] = {
		[UKFW_TA3DCTL] = PVRSGX_FW_TA3DCTL,
		[UKFW_HOSTCTL] = PVRSGX_FW_HOSTCTL,
		[UKFW_CCBCTL] = PVRSGX_FW_CCBCTL,
		[UKFW_CCB] = PVRSGX_FW_CCB,
		[UKFW_MISC] = PVRSGX_FW_MISC,
	};
	struct pvrsgx_fw *f = &sdev->fw;
	u32 entry[5];
	int err = 0;

	/* The header check guarantees program 0 is listed. */
	for (u32 i = 0; i < f->n_entries; i++)
		if (!f->entries[i][0])
			memset32(entry, f->entries[i][1], ARRAY_SIZE(entry));
	for (u32 i = 0; i < f->n_entries; i++)
		entry[f->entries[i][0]] = f->entries[i][1];
	for (u32 p = 0; p < ARRAY_SIZE(entry); p++)
		pds_entry(sdev, PVRSGX_FW_PDS0 + p, entry[p]);

	for (u32 i = 0; i < f->n_patches && !err; i++)
		err = limm(sdev, f->patches[i][0], f->addr[value_buf[f->patches[i][1]]]);
	if (err)
		return err;

	for (u32 t = 0; t < PVRSGX_CMD_MAX; t++)
		f->host_kick[t] = t;
	return 0;
}

/* Upload and patch the microkernel, assemble its entry programs, fill the signature lists. */
static int fw_build(struct pvrsgx_device *sdev, const u8 *image)
{
	struct pvrsgx_fw *f = &sdev->fw;
	u32 *t;
	int err;

	memcpy(f->cpu[PVRSGX_FW_UKERNEL], image, f->image_size);

	t = fw_words(sdev, PVRSGX_FW_TASIG);
	t[0] = ARRAY_SIZE(ta_sig);
	memcpy(t + 2, ta_sig, sizeof(ta_sig));
	t = fw_words(sdev, PVRSGX_FW_3DSIG);
	t[0] = ARRAY_SIZE(sig_3d);
	memcpy(t + 2, sig_3d, sizeof(sig_3d));

	if (!f->ddk)
		return fw_build_open(sdev);

	pds_entry(sdev, PVRSGX_FW_PDS0, 0xd60);	/* the event program */
	pds_ta3d(sdev);
	pds_entry(sdev, PVRSGX_FW_PDS2, 0xe20);
	pds_events(sdev);
	pds_events2(sdev);

	err = limm(sdev, 0xbaf8, 0x1800100);
	err = err ?: limm(sdev, 0xbb10, f->addr[PVRSGX_FW_PDS4] >> 4);
	err = err ?: limm(sdev, 0x0d60, 0x1001000);
	err = err ?: limm(sdev, 0x0d70, f->addr[PVRSGX_FW_PDS1] >> 4);
	err = err ?: limm(sdev, 0x0d78, f->addr[PVRSGX_FW_TA3DCTL]);
	err = err ?: limm(sdev, 0x0db0, 0x800100);
	err = err ?: limm(sdev, 0x0dc0, f->addr[PVRSGX_FW_PDS2] >> 4);
	err = err ?: limm(sdev, 0x0e40, f->addr[PVRSGX_FW_PDS3] & ~0xfu);
	err = err ?: limm(sdev, 0x0e50, 6);
	if (err)
		return err;

	memcpy(f->host_kick, host_kick, sizeof(host_kick));
	return 0;
}

/* One cleanup command and its outcome (UKERNEL.md 3, cleanup v2). */
static int cleanup_v2(struct pvrsgx_device *sdev, u32 type, u32 addr)
{
	u32 *hc = fw_words(sdev, PVRSGX_FW_HOSTCTL);
	ktime_t give_up = ktime_add_ms(ktime_get(), 1000);
	u32 data[6] = { type, addr };
	u32 v;
	int err;

	do {
		data[2] = ++sdev->cleanup_seq ?: ++sdev->cleanup_seq;
		WRITE_ONCE(hc[HOSTCTL_CLEANUP_STATUS], 0);
		err = pvrsgx_ccb_send(sdev, PVRSGX_CMD_CLEANUP, data, NULL);
		if (err)
			return err;
		err = read_poll_timeout(READ_ONCE, v,
					(v & HOSTCTL_CLEANUP_COMPLETE) &&
					READ_ONCE(hc[HOSTCTL_CLEANUP_SEQ]) == data[2],
					1000, 5 * USEC_PER_SEC, false, hc[HOSTCTL_CLEANUP_STATUS]);
		if (err)
			return err;
		if (v & HOSTCTL_CLEANUP_DONE)
			return 0;
		fsleep(1000);
	} while (ktime_before(ktime_get(), give_up));
	return -EBUSY;
}

/*
 * Tell the microkernel to forget a resource (UKERNEL.md 3, cleanup). It
 * completes only once nothing in flight uses it; the caller has waited for
 * its own jobs. On a timeout the memory is kept anyway (graveyard).
 */
int pvrsgx_fw_cleanup(struct pvrsgx_device *sdev, u32 type, u32 addr)
{
	bool v2 = sdev->fw.features & PVRSGX_UKFW_CLEANUP_V2;
	u32 data[6] = { type, addr };
	u32 *status;
	u32 v;
	int err, pm;

	if (!(cleanup < 0 ? v2 : cleanup) || !addr)
		return 0;
	/* A suspended microkernel holds nothing; the next start is a fresh one. */
	pm = pm_runtime_get_if_active(&sdev->pdev->dev);
	if (pm == 0)
		return 0;
	mutex_lock(&sdev->cleanup_lock);
	if (!sdev->fw_running || sdev->lost) {
		err = 0;
		goto out;
	}
	if (v2) {
		err = cleanup_v2(sdev, type, addr);
		goto done;
	}
	status = &fw_words(sdev, PVRSGX_FW_HOSTCTL)[HOSTCTL_CLEANUP_STATUS];
	err = pvrsgx_ccb_send(sdev, PVRSGX_CMD_CLEANUP, data, NULL);
	if (err)
		goto out;
	err = read_poll_timeout(READ_ONCE, v, v & HOSTCTL_CLEANUP_COMPLETE, 1000, 5 * USEC_PER_SEC,
				false, *status);
	WRITE_ONCE(*status, *status & ~HOSTCTL_CLEANUP_COMPLETE);
done:
	if (err)
		drm_err(&sdev->base, "cleanup %u of 0x%08x did not complete (%d)\n",
			type, addr, err);
	sdev->fw.cache_control |= PVRSGX_CC_INVAL_DATA;
out:
	mutex_unlock(&sdev->cleanup_lock);
	if (pm > 0)
		pm_runtime_put_autosuspend(&sdev->pdev->dev);
	return err;
}

/* Copy n pairs at off of the file, or NULL if they run past its end. */
static u32 (*ukfw_table(const struct firmware *fw, u32 off, u32 n))[2]
{
	const struct ukfw_pair *src = (const void *)(fw->data + off);
	u32 (*t)[2];

	if (!n || n > 64 || off > fw->size || n * sizeof(*src) > fw->size - off ||
	    !IS_ALIGNED(off, 4))
		return NULL;
	t = kmalloc_array(n, sizeof(*t), GFP_KERNEL);
	if (!t)
		return NULL;
	for (u32 i = 0; i < n; i++) {
		t[i][0] = le32_to_cpu(src[i].a);
		t[i][1] = le32_to_cpu(src[i].b);
	}
	return t;
}

/*
 * The open microkernel's file: check the header (docs/UKERNEL.md 4a) and take
 * its image and tables. -ENOENT if fw is not one.
 */
static int ukfw_load(struct pvrsgx_device *sdev, const struct firmware *fw)
{
	const struct ukfw_header *h = (const void *)fw->data;
	struct pvrsgx_fw *f = &sdev->fw;
	u32 hsize, isize, iface;
	bool entry0 = false;

	if (fw->size < UKFW_HEADER_MIN || le32_to_cpu(h->magic[0]) != UKFW_MAGIC0 ||
	    le32_to_cpu(h->magic[1]) != UKFW_MAGIC1)
		return -ENOENT;
	hsize = le32_to_cpu(h->header_size);
	isize = le32_to_cpu(h->image_size);
	iface = le32_to_cpu(h->interface);
	f->features = le32_to_cpu(h->features);
	if (hsize < UKFW_HEADER_MIN || !IS_ALIGNED(hsize, 8) || hsize > fw->size ||
	    !isize || !IS_ALIGNED(isize, 8) || isize > fw->size - hsize || isize > UKFW_MAX_IMAGE) {
		drm_err(&sdev->base, "%s: bad sizes (header %u, image %u, file %zu)\n", f->name,
			hsize, isize, fw->size);
		return -EINVAL;
	}
	if (iface >> 16 != UKFW_IF_MAJOR || (f->features & ~UKFW_FEATURES)) {
		drm_err(&sdev->base, "%s: interface %u.%u, features 0x%x not supported\n", f->name,
			iface >> 16, iface & 0xffff, f->features);
		return -EINVAL;
	}
	if (crc32_le(~0, fw->data + hsize, isize) != ~le32_to_cpu(h->image_crc32)) {
		drm_err(&sdev->base, "%s: image CRC mismatch\n", f->name);
		return -EINVAL;
	}
	if (h->core_rev && le32_to_cpu(h->core_rev) != sdev->core_rev)
		drm_warn(&sdev->base, "%s: validated on core revision 0x%x, this is 0x%x\n",
			 f->name, le32_to_cpu(h->core_rev), sdev->core_rev);

	f->n_entries = le32_to_cpu(h->n_entries);
	f->n_patches = le32_to_cpu(h->n_patches);
	f->entries = ukfw_table(fw, le32_to_cpu(h->entries_off), f->n_entries);
	if (f->n_patches)
		f->patches = ukfw_table(fw, le32_to_cpu(h->patches_off), f->n_patches);
	if (!f->entries || (f->n_patches && !f->patches))
		goto bad_table;
	for (u32 i = 0; i < f->n_entries; i++) {
		if (f->entries[i][0] > 4 || f->entries[i][1] >= isize ||
		    !IS_ALIGNED(f->entries[i][1], 8))
			goto bad_table;
		entry0 |= !f->entries[i][0];
	}
	for (u32 i = 0; i < f->n_patches; i++) {
		if (f->patches[i][0] > isize - 8 || !IS_ALIGNED(f->patches[i][0], 8) ||
		    f->patches[i][1] < UKFW_TA3DCTL || f->patches[i][1] > UKFW_MISC)
			goto bad_table;
	}
	if (!entry0)
		goto bad_table;

	f->image_size = isize;
	f->image = kmemdup(fw->data + hsize, isize, GFP_KERNEL);
	if (!f->image)
		return -ENOMEM;
	drm_info(&sdev->base, "%s: open microkernel %.32s, interface %u.%u, features 0x%x\n",
		 f->name, h->build_id, iface >> 16, iface & 0xffff, f->features);
	return 0;

bad_table:
	drm_err(&sdev->base, "%s: bad entry or patch table\n", f->name);
	return -EINVAL;
}

static int ddk_load(struct pvrsgx_device *sdev, const struct firmware *fw)
{
	struct pvrsgx_fw *f = &sdev->fw;

	if (fw->size != UKERNEL_SIZE) {
		drm_err(&sdev->base, "%s: %zu bytes, expected %u\n", f->name, fw->size,
			UKERNEL_SIZE);
		return -EINVAL;
	}
	f->image_size = UKERNEL_SIZE;
	f->image = kmemdup(fw->data, UKERNEL_SIZE, GFP_KERNEL);
	if (!f->image)
		return -ENOMEM;
	f->ddk = true;		/* the only other image this driver knows */
	return 0;
}

static int fw_request(struct pvrsgx_device *sdev, const struct firmware **fw)
{
	static const char *const names[] = { FW_OPEN, FW_DDK };
	int err = -ENOENT;

	if (firmware) {
		sdev->fw.name = firmware;
		return request_firmware(fw, firmware, sdev->base.dev);
	}
	for (u32 i = 0; i < ARRAY_SIZE(names) && err; i++) {
		sdev->fw.name = names[i];
		err = firmware_request_nowarn(fw, names[i], sdev->base.dev);
	}
	return err;
}

int pvrsgx_fw_init(struct pvrsgx_device *sdev)
{
	struct pvrsgx_fw *f = &sdev->fw;
	const struct firmware *fw;
	u32 pds_base;
	int err;

	err = fw_request(sdev, &fw);
	if (err) {
		drm_err(&sdev->base, "no microkernel: %s (%d)\n", firmware ?: FW_OPEN " or " FW_DDK,
			err);
		return err;
	}
	err = ukfw_load(sdev, fw);
	if (err == -ENOENT)
		err = ddk_load(sdev, fw);
	release_firmware(fw);
	if (err)
		return err;
	f->interface = DRM_POWERVR_SGX_FW_IF_SGXMKIF_1_7;

	/* PTE flags as the DDK maps the same buffers: data read-write, code read-only. */
	err = pvrsgx_kmem_alloc(sdev, &f->data, PVRSGX_HEAP_KERNEL_DATA, DATA_ARENA_SIZE,
				SGX_MMU_PTE_CACHECONSISTENT | SGX_MMU_PTE_EDMPROTECT);
	if (err)
		return err;
	err = pvrsgx_kmem_alloc(sdev, &f->code, PVRSGX_HEAP_KERNEL_CODE, CODE_ARENA_SIZE,
				SGX_MMU_PTE_READONLY);
	if (err)
		return err;

	pds_base = ALIGN(f->image_size, 0x10);
	for (int b = 0; b < PVRSGX_FW_NBUF; b++) {
		struct pvrsgx_kmem *km = layout[b].code ? &f->code : &f->data;
		u32 off = layout[b].offset;

		if (b >= PVRSGX_FW_PDS0 && b <= PVRSGX_FW_PDS4)
			off += pds_base;
		f->addr[b] = km->dev_addr + off;
		f->cpu[b] = (u8 *)km->cpu + off;
	}
	return fw_build(sdev, f->image);
}

void pvrsgx_fw_fini(struct pvrsgx_device *sdev)
{
	kfree(sdev->fw.entries);
	kfree(sdev->fw.patches);
	sdev->fw.entries = NULL;
	sdev->fw.patches = NULL;
	kfree(sdev->fw.image);
	sdev->fw.image = NULL;
	pvrsgx_kmem_free(sdev, &sdev->fw.code);
	pvrsgx_kmem_free(sdev, &sdev->fw.data);
}

static void wr(struct pvrsgx_device *sdev, u32 reg, u32 val)
{
	sgx_wr(sdev, reg, val);
}

/* Init script part 2 (docs/SRVINIT.md, "Init scripts"). */
static void init_script_part2(struct pvrsgx_device *sdev)
{
	struct pvrsgx_fw *f = &sdev->fw;

	wr(sdev, 0x818, 0x7ff);
	wr(sdev, 0x810, 1);
	wr(sdev, 0x814, 0xffff);
	wr(sdev, SGX_CR_EVENT_HOST_ENABLE, SGX_CR_EVENT_STATUS_SW_EVENT);
	wr(sdev, SGX_CR_EVENT_HOST_ENABLE2, 0);
	wr(sdev, 0x13c, 0);
	wr(sdev, 0xa7c, 0);
	wr(sdev, 0xa80, 0);
	wr(sdev, 0xa74, 0x208200);
	wr(sdev, 0x4e8, 0x100);
	wr(sdev, 0xa78, 0x2c);
	wr(sdev, 0xaac, 0x45);
	wr(sdev, 0xabc, 0xe);
	wr(sdev, 0x800, 0x30);
	wr(sdev, 0x804, 0xffff);
	wr(sdev, 0xa00, 0x7c000);
	/* Code bases 0-9 the pixel shader heap, 10-13 the vertex shader heap, 15 ours. */
	for (u32 i = 0; i < 10; i++)
		wr(sdev, SGX_CR_USE_CODE_BASE(PVRSGX_USSE_PIXEL_BASE_INDEX + i),
		   ((PVRSGX_HEAP_USSE_PIXEL + (i << PVRSGX_USSE_WINDOW_LOG2)) >> 7) | 0x2000000);
	for (u32 i = 0; i < 4; i++)
		wr(sdev, SGX_CR_USE_CODE_BASE(PVRSGX_USSE_VERTEX_BASE_INDEX + i),
		   (PVRSGX_HEAP_USSE_VERTEX + (i << PVRSGX_USSE_WINDOW_LOG2)) >> 7);
	wr(sdev, SGX_CR_USE_CODE_BASE(15), (PVRSGX_HEAP_KERNEL_CODE >> 7) | 0x6000000);
	wr(sdev, SGX_CR_PDS_EXEC_BASE, PVRSGX_HEAP_PDS_PIXEL);
	wr(sdev, 0xb30, 0x100);
	wr(sdev, SGX_CR_EVENT_KICKER, f->addr[PVRSGX_FW_KICKER]);
	/* The event PDS program and its two control words. */
	wr(sdev, 0xa68, f->addr[PVRSGX_FW_PDS0] & ~0xfu);
	wr(sdev, 0xa6c, 1);
	wr(sdev, 0xa70, 6);
	wr(sdev, 0xa58, 0x2204b00f);
}

static void deinit_script(struct pvrsgx_device *sdev)
{
	wr(sdev, 0xa58, 0);
	wr(sdev, 0xa68, 0);
	wr(sdev, 0xa6c, 0);
	wr(sdev, 0xa70, 0);
	wr(sdev, SGX_CR_USE_CODE_BASE(15), 0);
	wr(sdev, SGX_CR_EVENT_HOST_ENABLE, 0);
	wr(sdev, SGX_CR_EVENT_HOST_ENABLE2, 0);
	wr(sdev, SGX_CR_CLKGATECTL, 0);
	wr(sdev, SGX_CR_CLKGATECTL2, 0);
}

/* GETMISCINFO: the microkernel reports its identity and struct sizes. */
static int fw_check(struct pvrsgx_device *sdev)
{
	u32 *misc = fw_words(sdev, PVRSGX_FW_MISC);
	u32 data[6] = { 0, sdev->fw.addr[PVRSGX_FW_MISC] };
	int err;

	memset(misc, 0, layout[PVRSGX_FW_MISC].size);
	misc[0] = 0x2;	/* GET_STRUCT_SIZES */
	err = pvrsgx_ccb_send(sdev, PVRSGX_CMD_GETMISCINFO, data, NULL);
	if (err)
		return err;
	err = pvrsgx_poll(sdev, &misc[0], 0x1, 0x1, 500000);	/* READY */
	if (err) {
		drm_err(&sdev->base, "GETMISCINFO: no answer\n");
		return err;
	}

	sdev->fw.ddk_version = misc[3];
	sdev->fw.ddk_build = misc[4];
	drm_info(&sdev->base,
		 "microkernel: core rev 0x%x id 0x%x, DDK 0x%x build %u, build options 0x%x\n",
		 misc[1], misc[2], misc[3], misc[4], misc[7]);
	for (int i = 0; i < ARRAY_SIZE(struct_sizes); i++) {
		if (misc[8 + i] != struct_sizes[i]) {
			drm_err(&sdev->base, "microkernel struct %d is 0x%x, expected 0x%x\n",
				i, misc[8 + i], struct_sizes[i]);
			return -EPROTO;
		}
	}
	return 0;
}

/*
 * SGXInitialise: clocks, part 1, reset, part 2, then the first event kick,
 * and the microkernel's init entry sets INIT_COMPLETE.
 */
int pvrsgx_fw_start(struct pvrsgx_device *sdev)
{
	u32 *hc = fw_words(sdev, PVRSGX_FW_HOSTCTL);
	bool first = !sdev->fw_started, was_powered = READ_ONCE(sdev->powered);
	int err;

	WRITE_ONCE(sdev->powered, true);

	/*
	 * A restart starts from a fresh image and empty arenas: the microkernel
	 * changes both as it runs, and a module reload (which recovers) is
	 * exactly this.
	 */
	if (!first) {
		sdev->fw_gen++;
		/*
		 * After a hang the clocks are on and the cores may be busy. After a
		 * clean stop (runtime suspend) they are gated and quiet already, and
		 * a register read there froze the EA3 (log 2026-11-03-0630).
		 */
		if (restart_quiesce && was_powered) {
			/* What an unload and reload (which recovers) does in between. */
			deinit_script(sdev);
			wr(sdev, SGX_CR_SOFT_RESET,
			   SGX_CR_SOFT_RESET_BIF | SGX_CR_SOFT_RESET_CORES);
			sgx_rd(sdev, SGX_CR_SOFT_RESET);
			pci_clear_master(sdev->pdev);
			fsleep(10000);
			pci_set_master(sdev->pdev);
		}
		memset(sdev->fw.data.cpu, 0, sdev->fw.data.size);
		memset(sdev->fw.code.cpu, 0, sdev->fw.code.size);
		err = fw_build(sdev, sdev->fw.image);
		if (err)
			return err;
		/* Nothing the old run pointed at matters any more. */
		pvrsgx_kobj_reap(sdev, U64_MAX);
	}
	memset(hc, 0, layout[PVRSGX_FW_HOSTCTL].size);
	hc[HOSTCTL_HWR_SAMPLE_RATE] = UKERNEL_TIMER_HZ / HWR_CHECK_HZ;
	hc[HOSTCTL_UK_TIMER_CLOCK] = core_clock / UKERNEL_TIMER_HZ;
	hc[HOSTCTL_APM_SAMPLE_RATE] = 0;
	/* SGXPostPowerState clears the power status before every start. */
	hc[HOSTCTL_POWER_STATUS] = 0;
	pvrsgx_ccb_reset(sdev);
	ta3dctl_fill(sdev);
	/* The first command invalidates the directory cache. */
	sdev->fw.cache_control = PVRSGX_CC_INVAL_BIF_PD;

	wr(sdev, SGX_CR_CLKGATECTL, 0xa6aa);
	wr(sdev, SGX_CR_CLKGATECTL2, 0xaaaaaa);
	wr(sdev, 0xa58, 0);
	wr(sdev, SGX_CR_EVENT_TIMER, 0);

	pvrsgx_reset(sdev, true);

	*(u32 *)sdev->fw.cpu[PVRSGX_FW_KICKER] = 0;
	init_script_part2(sdev);

	hc[HOSTCTL_HOST_CLOCK] = (u32)ktime_to_us(ktime_get());
	hc[HOSTCTL_INIT_STATUS] = 0;
	/* The image and the host control block are in memory before the kick. */
	wmb();
	wr(sdev, SGX_CR_EVENT_KICK2, SGX_CR_EVENT_KICK_NOW);

	err = pvrsgx_poll(sdev, &hc[HOSTCTL_INIT_STATUS], HOSTCTL_INIT_COMPLETE,
			  HOSTCTL_INIT_COMPLETE, 500000);
	if (err) {
		drm_err(&sdev->base, "microkernel did not start (init status 0x%x, event status 0x%x)\n",
			hc[HOSTCTL_INIT_STATUS], sgx_rd(sdev, SGX_CR_EVENT_STATUS));
		return err;
	}
	sdev->fw_running = true;
	sdev->fw_started = true;
	if (first || sdev->reset_count != sdev->reset_logged) {
		drm_info(&sdev->base, "microkernel started%s\n", first ? "" : " again");
		sdev->reset_logged = sdev->reset_count;
	}
	if (first && pvrsgx_vm_shares_kernel_heaps(sdev))
		drm_warn(&sdev->base,
			 "every VM maps the firmware's heaps: clients are not isolated from the firmware\n");

	err = fw_check(sdev);
	if (err)
		sdev->fw_running = false;
	return err;
}

/* SGXPrePowerState(OFF), then hold the cores in reset. */
void pvrsgx_fw_stop(struct pvrsgx_device *sdev)
{
	u32 *hc = fw_words(sdev, PVRSGX_FW_HOSTCTL);
	u32 data[6] = { 0, 1 };	/* POWEROFF */

	if (sdev->fw_running) {
		if (pvrsgx_ccb_send(sdev, PVRSGX_CMD_POWER, data, NULL) ||
		    pvrsgx_poll(sdev, &hc[HOSTCTL_POWER_STATUS], HOSTCTL_POWMAN_POWEROFF_COMPLETE,
				HOSTCTL_POWMAN_POWEROFF_COMPLETE, 500000))
			drm_warn(&sdev->base, "microkernel did not power off\n");
		for (int i = 0; i < 500 && (sgx_rd(sdev, SGX_CR_CLKGATESTATUS) & 0xffffffdf); i++)
			usleep_range(100, 200);
		WRITE_ONCE(sdev->powered, false);
		synchronize_irq(sdev->irq);
		deinit_script(sdev);
		sdev->fw_running = false;
		wr(sdev, SGX_CR_SOFT_RESET, SGX_CR_SOFT_RESET_BIF | SGX_CR_SOFT_RESET_CORES);
		sgx_rd(sdev, SGX_CR_SOFT_RESET);
	} else if (READ_ONCE(sdev->powered)) {
		/* A failed start: the clocks are on, the microkernel isn't. */
		WRITE_ONCE(sdev->powered, false);
		synchronize_irq(sdev->irq);
		wr(sdev, SGX_CR_SOFT_RESET, SGX_CR_SOFT_RESET_BIF | SGX_CR_SOFT_RESET_CORES);
		sgx_rd(sdev, SGX_CR_SOFT_RESET);
	}
}
