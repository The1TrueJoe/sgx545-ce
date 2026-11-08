// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * Loading and starting the microkernel: what srvinit and the DDK services did
 * between them, now in the kernel. docs/SRVINIT.md is the reference for every
 * buffer, constant, PDS program and script value here, and docs/UKERNEL.md for
 * how the microkernel uses them.
 *
 * The host interface is the vendor's as of DDK 1.7 (decision D8). The open
 * microkernel's file header lists its entry points and patch sites
 * (docs/UKERNEL.md 4a).
 */

#include <drm/drm_print.h>

#include <linux/bitfield.h>
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/seq_buf.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <uapi/drm/powervr_sgx_drm.h>

#include "pvrsgx_ccb.h"
#include "pvrsgx_core.h"
#include "pvrsgx_device.h"
#include "pvrsgx_fw.h"
#include "pvrsgx_fwif.h"
#include "pvrsgx_kobj.h"
#include "pvrsgx_mmu.h"
#include "pvrsgx_pds.h"

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

/* SGXMKIF_MISCINFO: the request and answer flags in word 0. */
#define MISCINFO_READY			BIT(0)
#define MISCINFO_GET_STRUCT_SIZES	BIT(1)

/* SGXMKIF_CMD_POWER: data word 1. */
#define POWER_OFF			1

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
	[PVRSGX_FW_TA3DCTL] = { false, 0x3000, sizeof(struct pvrsgx_fw_ta3d_ctl) },
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

void pvrsgx_fw_hostctl_set(struct pvrsgx_device *sdev, unsigned int word, u32 v)
{
	WRITE_ONCE(fw_words(sdev, PVRSGX_FW_HOSTCTL)[word], v);
}

/*
 * Fill one of the microkernel's limm placeholders (word 0 0x?dbeef, the value
 * bits of word 1 clear): bits 0-20 to word 0, 21-25 to word 1 bits 4-8, 26-31
 * to word 1 bits 12-17.
 */
static int fw_limm(struct pvrsgx_device *sdev, u32 off, u32 v)
{
	u32 *i = (u32 *)((u8 *)sdev->fw.cpu[PVRSGX_FW_UKERNEL] + off);
	bool ok = (i[0] & 0x1fffff) == 0xdbeef && !(i[1] & 0x0003f1f0);

	if (off > sdev->fw.image_size - 8 || !ok) {
		drm_err(&sdev->base, "%s: no patch placeholder at +0x%x\n", sdev->fw.name, off);
		return -EINVAL;
	}
	i[0] = (i[0] & 0xffe00000) | (v & 0x1fffff);
	i[1] = (i[1] & 0xfffc0e0f) | ((v >> 17) & 0x1f0) | ((v >> 26) << 12);
	return 0;
}

/*
 * The microkernel's control block, rebuilt at every start: after a reset it
 * must not find the previous run's render in flight (UKERNEL.md 5a, "discard").
 */
static void ta3dctl_fill(struct pvrsgx_device *sdev)
{
	struct pvrsgx_fw_ta3d_ctl *t = sdev->fw.cpu[PVRSGX_FW_TA3DCTL];
	struct pvrsgx_fw *f = &sdev->fw;

	memset(t, 0, sizeof(*t));
	t->self = f->addr[PVRSGX_FW_TA3DCTL];
	t->host_ctl = f->addr[PVRSGX_FW_HOSTCTL];
	t->ccb_ctl = f->addr[PVRSGX_FW_CCBCTL];
	t->ccb = f->addr[PVRSGX_FW_CCB];
	t->kernel_pd = pvrsgx_mmu_pd_addr(sdev->kernel_mmu);
	t->hwperf = f->addr[PVRSGX_FW_HWPERF];
	t->ta_sig = f->addr[PVRSGX_FW_TASIG];
	t->sig_3d = f->addr[PVRSGX_FW_3DSIG];
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
		pvrsgx_pds_entry(sdev, PVRSGX_FW_PDS0 + p, entry[p]);

	for (u32 i = 0; i < f->n_patches && !err; i++)
		err = fw_limm(sdev, f->patches[i][0], f->addr[value_buf[f->patches[i][1]]]);
	if (err)
		return err;
	return 0;
}

/* Upload and patch the microkernel, assemble its entry programs, fill the signature lists. */
static int fw_build(struct pvrsgx_device *sdev, const u8 *image)
{
	struct pvrsgx_fw *f = &sdev->fw;
	u32 *t;

	memcpy(f->cpu[PVRSGX_FW_UKERNEL], image, f->image_size);

	t = fw_words(sdev, PVRSGX_FW_TASIG);
	t[0] = ARRAY_SIZE(ta_sig);
	memcpy(t + 2, ta_sig, sizeof(ta_sig));
	t = fw_words(sdev, PVRSGX_FW_3DSIG);
	t[0] = ARRAY_SIZE(sig_3d);
	memcpy(t + 2, sig_3d, sizeof(sig_3d));

	return fw_build_open(sdev);
}

/* Copy n pairs at off of the file, or NULL if they run past its end. */
static u32 (*ukfw_table(const u8 *data, size_t size, u32 off, u32 n))[2]
{
	const struct ukfw_pair *src = (const void *)(data + off);
	u32 (*t)[2];

	if (!n || n > 64 || off > size || n * sizeof(*src) > size - off || !IS_ALIGNED(off, 4))
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
 * its image and tables into f. -ENOENT if data is not one; on -EINVAL, *why
 * says what is wrong. f's tables and image are the caller's to free either way.
 */
static int ukfw_parse(const u8 *data, size_t size, struct pvrsgx_fw *f, const char **why)
{
	const struct ukfw_header *h = (const void *)data;
	u32 hsize, isize, iface;
	bool entry0 = false;

	if (size < UKFW_HEADER_MIN || le32_to_cpu(h->magic[0]) != UKFW_MAGIC0 ||
	    le32_to_cpu(h->magic[1]) != UKFW_MAGIC1)
		return -ENOENT;
	hsize = le32_to_cpu(h->header_size);
	isize = le32_to_cpu(h->image_size);
	iface = le32_to_cpu(h->interface);
	f->features = le32_to_cpu(h->features);
	*why = "bad sizes";
	if (hsize < UKFW_HEADER_MIN || !IS_ALIGNED(hsize, 8) || hsize > size ||
	    !isize || !IS_ALIGNED(isize, 8) || isize > size - hsize || isize > UKFW_MAX_IMAGE)
		return -EINVAL;
	*why = "interface or features not supported";
	if (iface >> 16 != UKFW_IF_MAJOR || (f->features & ~UKFW_FEATURES))
		return -EINVAL;
	*why = "image CRC mismatch";
	if (crc32_le(~0, data + hsize, isize) != ~le32_to_cpu(h->image_crc32))
		return -EINVAL;

	*why = "bad entry or patch table";
	f->n_entries = le32_to_cpu(h->n_entries);
	f->n_patches = le32_to_cpu(h->n_patches);
	f->entries = ukfw_table(data, size, le32_to_cpu(h->entries_off), f->n_entries);
	if (f->n_patches)
		f->patches = ukfw_table(data, size, le32_to_cpu(h->patches_off), f->n_patches);
	if (!f->entries || (f->n_patches && !f->patches))
		return -EINVAL;
	for (u32 i = 0; i < f->n_entries; i++) {
		if (f->entries[i][0] > 4 || f->entries[i][1] >= isize ||
		    !IS_ALIGNED(f->entries[i][1], 8))
			return -EINVAL;
		entry0 |= !f->entries[i][0];
	}
	for (u32 i = 0; i < f->n_patches; i++) {
		if (f->patches[i][0] > isize - 8 || !IS_ALIGNED(f->patches[i][0], 8) ||
		    f->patches[i][1] < UKFW_TA3DCTL || f->patches[i][1] > UKFW_MISC)
			return -EINVAL;
	}
	if (!entry0)
		return -EINVAL;

	f->image_size = isize;
	f->image = kmemdup(data + hsize, isize, GFP_KERNEL);
	return f->image ? 0 : -ENOMEM;
}

static int ukfw_load(struct pvrsgx_device *sdev, const struct firmware *fw)
{
	const struct ukfw_header *h = (const void *)fw->data;
	struct pvrsgx_fw *f = &sdev->fw;
	const char *why = "";
	u32 iface;
	int err;

	err = ukfw_parse(fw->data, fw->size, f, &why);
	if (err == -EINVAL)
		drm_err(&sdev->base, "%s: %s\n", f->name, why);
	if (err)
		return err;
	if (!(f->features & PVRSGX_UKFW_SECURE)) {
		drm_err(&sdev->base, "%s: not a secure-mode image; clients would reach the firmware\n",
			f->name);
		return -EINVAL;
	}
	iface = le32_to_cpu(h->interface);
	if (h->core_rev && le32_to_cpu(h->core_rev) != sdev->core_rev)
		drm_warn(&sdev->base, "%s: validated on core revision 0x%x, this is 0x%x\n",
			 f->name, le32_to_cpu(h->core_rev), sdev->core_rev);
	drm_info(&sdev->base, "%s: open microkernel %.32s, interface %u.%u, features 0x%x\n",
		 f->name, h->build_id, iface >> 16, iface & 0xffff, f->features);
	return 0;
}

int pvrsgx_fw_init(struct pvrsgx_device *sdev)
{
	struct pvrsgx_fw *f = &sdev->fw;
	const struct firmware *fw;
	u32 pds_base;
	int err;

	f->name = sdev->core->fw_name;
	err = request_firmware(&fw, f->name, sdev->base.dev);
	if (err) {
		drm_err(&sdev->base, "no microkernel: %s (%d)\n", f->name, err);
		return err;
	}
	err = ukfw_load(sdev, fw);
	if (err == -ENOENT)
		drm_err(&sdev->base, "%s: not a microkernel file\n", f->name);
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

/*
 * Init script part 2 (docs/SRVINIT.md, "Init scripts"), in its order. Most of
 * these registers have no name in the GPL header; the values are the vendor
 * script's.
 */
static const struct {
	u16 reg;
	u32 val;
} init_regs[] = {
	{ 0x818, 0x7ff }, { 0x810, 1 }, { 0x814, 0xffff },
	{ SGX_CR_EVENT_HOST_ENABLE, SGX_CR_EVENT_STATUS_SW_EVENT },
	{ SGX_CR_EVENT_HOST_ENABLE2, 0 },
	{ 0x13c, 0 }, { 0xa7c, 0 }, { 0xa80, 0 }, { 0xa74, 0x208200 }, { 0x4e8, 0x100 },
	{ 0xa78, 0x2c }, { 0xaac, 0x45 }, { 0xabc, 0xe }, { 0x800, 0x30 }, { 0x804, 0xffff },
	{ 0xa00, 0x7c000 },
};

/* USE_CODE_BASE: device address >> 7, and the window's mode bits. */
#define USE_CODE_BASE_ADDR_SHIFT	7
#define USE_CODE_BASE_PIXEL		0x2000000
#define USE_CODE_BASE_UKERNEL		0x6000000

static void init_script_part2(struct pvrsgx_device *sdev)
{
	struct pvrsgx_fw *f = &sdev->fw;

	for (u32 i = 0; i < ARRAY_SIZE(init_regs); i++)
		sgx_wr(sdev, init_regs[i].reg, init_regs[i].val);
	/* Code bases 0-9 the pixel shader heap, 10-13 the vertex shader heap, 15 ours. */
	for (u32 i = 0; i < 10; i++)
		sgx_wr(sdev, SGX_CR_USE_CODE_BASE(PVRSGX_USSE_PIXEL_BASE_INDEX + i),
		       ((PVRSGX_HEAP_USSE_PIXEL + (i << PVRSGX_USSE_WINDOW_LOG2)) >>
			USE_CODE_BASE_ADDR_SHIFT) | USE_CODE_BASE_PIXEL);
	for (u32 i = 0; i < 4; i++)
		sgx_wr(sdev, SGX_CR_USE_CODE_BASE(PVRSGX_USSE_VERTEX_BASE_INDEX + i),
		       (PVRSGX_HEAP_USSE_VERTEX + (i << PVRSGX_USSE_WINDOW_LOG2)) >>
		       USE_CODE_BASE_ADDR_SHIFT);
	sgx_wr(sdev, SGX_CR_USE_CODE_BASE(PVRSGX_USSE_UKERNEL_BASE_INDEX),
	       (PVRSGX_HEAP_KERNEL_CODE >> USE_CODE_BASE_ADDR_SHIFT) | USE_CODE_BASE_UKERNEL);
	sgx_wr(sdev, SGX_CR_PDS_EXEC_BASE, PVRSGX_HEAP_PDS_PIXEL);
	sgx_wr(sdev, 0xb30, 0x100);	/* unnamed, as the vendor script */
	sgx_wr(sdev, SGX_CR_EVENT_KICKER, f->addr[PVRSGX_FW_KICKER]);
	sgx_wr(sdev, SGX_CR_EVENT_PDS_BASE, f->addr[PVRSGX_FW_PDS0] & ~0xfu);
	sgx_wr(sdev, SGX_CR_EVENT_PDS_DATA0, 1);
	sgx_wr(sdev, SGX_CR_EVENT_PDS_DATA1, 6);
	sgx_wr(sdev, SGX_CR_EVENT_PDS_ENABLE, SGX_CR_EVENT_PDS_ENABLE_ALL);
}

static void deinit_script(struct pvrsgx_device *sdev)
{
	sgx_wr(sdev, SGX_CR_EVENT_PDS_ENABLE, 0);
	sgx_wr(sdev, SGX_CR_EVENT_PDS_BASE, 0);
	sgx_wr(sdev, SGX_CR_EVENT_PDS_DATA0, 0);
	sgx_wr(sdev, SGX_CR_EVENT_PDS_DATA1, 0);
	sgx_wr(sdev, SGX_CR_USE_CODE_BASE(PVRSGX_USSE_UKERNEL_BASE_INDEX), 0);
	sgx_wr(sdev, SGX_CR_EVENT_HOST_ENABLE, 0);
	sgx_wr(sdev, SGX_CR_EVENT_HOST_ENABLE2, 0);
	sgx_wr(sdev, SGX_CR_CLKGATECTL, 0);
	sgx_wr(sdev, SGX_CR_CLKGATECTL2, 0);
}

/* GETMISCINFO: the microkernel reports its identity and struct sizes. */
static int fw_check(struct pvrsgx_device *sdev)
{
	u32 *misc = fw_words(sdev, PVRSGX_FW_MISC);
	u32 data[6] = { 0, sdev->fw.addr[PVRSGX_FW_MISC] };
	int err;

	memset(misc, 0, layout[PVRSGX_FW_MISC].size);
	misc[0] = MISCINFO_GET_STRUCT_SIZES;
	err = pvrsgx_ccb_send(sdev, PVRSGX_CMD_GETMISCINFO, data, NULL);
	if (err)
		return err;
	err = pvrsgx_poll(sdev, &misc[0], MISCINFO_READY, MISCINFO_READY, 500000);
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
		if (was_powered) {
			/* What an unload and reload (which recovers) does in between. */
			deinit_script(sdev);
			sgx_wr(sdev, SGX_CR_SOFT_RESET,
			       SGX_CR_SOFT_RESET_BIF | SGX_CR_SOFT_RESET_CORES);
			sgx_rd(sdev, SGX_CR_SOFT_RESET);
			/* Behind gma500 bus mastering is the display's too: leave it. */
			if (sdev->pdev)
				pci_clear_master(sdev->pdev);
			fsleep(10000);
			if (sdev->pdev)
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
	hc[HOSTCTL_UK_TIMER_CLOCK] = sdev->core->clock_hz / UKERNEL_TIMER_HZ;
	hc[HOSTCTL_FLAGS] = HOSTCTL_HOST_BGBLOCK;
	hc[HOSTCTL_APM_SAMPLE_RATE] = 0;
	/* SGXPostPowerState clears the power status before every start. */
	hc[HOSTCTL_POWER_STATUS] = 0;
	pvrsgx_ccb_reset(sdev);
	ta3dctl_fill(sdev);
	/* The first command invalidates the directory cache. */
	sdev->fw.cache_control = PVRSGX_CC_INVAL_BIF_PD;

	/* Every unit's clock gated automatically, but the DPM's: it stays on. */
	sgx_wr(sdev, SGX_CR_CLKGATECTL,
	       (SGX_CLKG_ALL(SGX_CR_CLKGATECTL_UNITS, SGX_CLKG_AUTO) & ~SGX_CR_CLKGATECTL_DPM) |
	       FIELD_PREP(SGX_CR_CLKGATECTL_DPM, SGX_CLKG_ON));
	sgx_wr(sdev, SGX_CR_CLKGATECTL2, SGX_CLKG_ALL(SGX_CR_CLKGATECTL2_UNITS, SGX_CLKG_AUTO));
	sgx_wr(sdev, SGX_CR_EVENT_PDS_ENABLE, 0);
	sgx_wr(sdev, SGX_CR_EVENT_TIMER, 0);

	pvrsgx_reset(sdev, true);

	*(u32 *)sdev->fw.cpu[PVRSGX_FW_KICKER] = 0;
	init_script_part2(sdev);

	hc[HOSTCTL_HOST_CLOCK] = (u32)ktime_to_us(ktime_get());
	hc[HOSTCTL_INIT_STATUS] = 0;
	/* The image and the host control block are in memory before the kick. */
	wmb();
	sgx_wr(sdev, SGX_CR_EVENT_KICK2, SGX_CR_EVENT_KICK_NOW);

	err = pvrsgx_poll(sdev, &hc[HOSTCTL_INIT_STATUS], HOSTCTL_INIT_COMPLETE,
			  HOSTCTL_INIT_COMPLETE, 500000);
	if (err) {
		drm_err(&sdev->base,
			"microkernel did not start (init status 0x%x, event status 0x%x; BIF int 0x%x fault 0x%x req 0x%x)\n",
			hc[HOSTCTL_INIT_STATUS], sgx_rd(sdev, SGX_CR_EVENT_STATUS),
			sgx_rd(sdev, SGX_CR_BIF_INT_STAT), sgx_rd(sdev, SGX_CR_BIF_FAULT),
			sgx_rd(sdev, SGX_CR_BIF_MEM_REQ_STAT));
		return err;
	}
	sdev->fw_running = true;
	sdev->fw_started = true;
	if (first || sdev->reset_count != sdev->reset_logged) {
		drm_info(&sdev->base, "microkernel started%s\n", first ? "" : " again");
		sdev->reset_logged = sdev->reset_count;
	}

	err = fw_check(sdev);
	if (err)
		sdev->fw_running = false;
	return err;
}

/* SGXPrePowerState(OFF), then hold the cores in reset. */
void pvrsgx_fw_stop(struct pvrsgx_device *sdev)
{
	u32 *hc = fw_words(sdev, PVRSGX_FW_HOSTCTL);
	u32 data[6] = { 0, POWER_OFF };

	if (sdev->fw_running) {
		if (pvrsgx_ccb_send(sdev, PVRSGX_CMD_POWER, data, NULL) ||
		    pvrsgx_poll(sdev, &hc[HOSTCTL_POWER_STATUS], HOSTCTL_POWMAN_POWEROFF_COMPLETE,
				HOSTCTL_POWMAN_POWEROFF_COMPLETE, 500000))
			drm_warn(&sdev->base, "microkernel did not power off\n");
		/* Wait for the clocks to gate; the DPM's stays on. */
		for (int i = 0; i < 500; i++) {
			if (!(sgx_rd(sdev, SGX_CR_CLKGATESTATUS) & ~SGX_CR_CLKGATESTATUS_DPM))
				break;
			usleep_range(100, 200);
		}
		WRITE_ONCE(sdev->powered, false);
		synchronize_irq(sdev->irq);
		deinit_script(sdev);
		sdev->fw_running = false;
		sgx_wr(sdev, SGX_CR_SOFT_RESET, SGX_CR_SOFT_RESET_BIF | SGX_CR_SOFT_RESET_CORES);
		sgx_rd(sdev, SGX_CR_SOFT_RESET);
	} else if (READ_ONCE(sdev->powered)) {
		/* A failed start: the clocks are on, the microkernel isn't. */
		WRITE_ONCE(sdev->powered, false);
		synchronize_irq(sdev->irq);
		sgx_wr(sdev, SGX_CR_SOFT_RESET, SGX_CR_SOFT_RESET_BIF | SGX_CR_SOFT_RESET_CORES);
		sgx_rd(sdev, SGX_CR_SOFT_RESET);
	}
}

#if IS_ENABLED(CONFIG_DRM_POWERVR_SGX_KUNIT_TEST)
#include "tests/pvrsgx_fw_test.c"
#endif
