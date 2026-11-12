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
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/seq_buf.h>
#include <linux/sizes.h>
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
#define UKFW_FEATURES		(PVRSGX_UKFW_CLEANUP_V2 | PVRSGX_UKFW_SECURE | \
				 PVRSGX_UKFW_SINGLE_CONTEXT)
/* Bits 16-31: the microkernel's own build variants, nothing the host must know. */
#define UKFW_FEATURES_PRIVATE	GENMASK(31, 16)
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
	UKFW_PDTABLE,	/* single-context cores only */
};

/* The POWER command: data word 1. */
#define POWER_OFF			1

/* The microkernel's timer runs at 1 kHz. */
#define UKERNEL_TIMER_HZ	1000

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
	/* Right after the control block, which fills 0x3000-0x30ff. */
	[PVRSGX_FW_HOSTCTL] = { false, 0x3100, 0x44 },
	[PVRSGX_FW_MISC] = { false, 0x3160, 0x54 },
	/* The image at 0, then the PDS programs from its end (0xcf70 for the DDK's). */
	[PVRSGX_FW_UKERNEL] = { true, 0x0000, 0 },
	[PVRSGX_FW_PDS0] = { true, 0x000, 0x18 },
	[PVRSGX_FW_PDS1] = { true, 0x020, 0x38 },
	[PVRSGX_FW_PDS2] = { true, 0x060, 0x18 },
	[PVRSGX_FW_PDS3] = { true, 0x090, 0x100 },
	[PVRSGX_FW_PDS4] = { true, 0x190, 0x68 },
};

static_assert(0x3000 + sizeof(struct pvrsgx_fw_ta3d_ctl) <= 0x3100);

#define PDS_BYTES	0x200

#define DATA_ARENA_SIZE	0xe000
#define CODE_ARENA_SIZE	0xe000

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

/*
 * A core still being brought up: the microkernel's version, last timed-out
 * wait, wait count and heartbeat (open microkernel control-block words 0x18,
 * 0x28, 0x29 and 0x2a), and the host control block's init status.
 */
void pvrsgx_fw_dump_status_words(struct pvrsgx_device *sdev)
{
	const u32 *t = fw_words(sdev, PVRSGX_FW_TA3DCTL);

	drm_info(&sdev->base,
		 "microkernel: init 0x%x version 0x%08x wait 0x%x timeouts %u heartbeat %u\n",
		 pvrsgx_fw_hostctl(sdev, HOSTCTL_INIT_STATUS), READ_ONCE(t[0x18]),
		 READ_ONCE(t[0x28]), READ_ONCE(t[0x29]), READ_ONCE(t[0x2a]));
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
	sdev->fw.event_entry = entry[0];

	for (u32 i = 0; i < f->n_patches && !err; i++) {
		u32 v = f->patches[i][1];

		if (v == UKFW_PDTABLE && !f->pdtable.pages) {
			drm_err(&sdev->base, "%s: a PD table patch on a multi-context core\n",
				f->name);
			return -EINVAL;
		}
		err = fw_limm(sdev, f->patches[i][0],
			      v == UKFW_PDTABLE ? f->pdtable.dev_addr : f->addr[value_buf[v]]);
	}
	if (err)
		return err;
	return 0;
}

/* Upload and patch the microkernel and assemble its entry programs. */
static int fw_build(struct pvrsgx_device *sdev, const u8 *image)
{
	struct pvrsgx_fw *f = &sdev->fw;

	memcpy(f->cpu[PVRSGX_FW_UKERNEL], image, f->image_size);

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
	if (iface >> 16 != UKFW_IF_MAJOR ||
	    (f->features & ~(UKFW_FEATURES | UKFW_FEATURES_PRIVATE)))
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
		    f->patches[i][1] < UKFW_TA3DCTL || f->patches[i][1] > UKFW_PDTABLE)
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
	/*
	 * Secure mode keeps the firmware out of every client directory, which a
	 * single-context core can't do: there the image must not claim it.
	 */
	if (pvrsgx_core_single_context(sdev->family)) {
		if ((f->features & (PVRSGX_UKFW_SECURE | PVRSGX_UKFW_SINGLE_CONTEXT)) !=
		    PVRSGX_UKFW_SINGLE_CONTEXT) {
			drm_err(&sdev->base, "%s: not an image for a single-context core\n",
				f->name);
			return -EINVAL;
		}
	} else if (f->features & PVRSGX_UKFW_SINGLE_CONTEXT) {
		drm_err(&sdev->base, "%s: an image for a single-context core\n", f->name);
		return -EINVAL;
	} else if (!(f->features & PVRSGX_UKFW_SECURE)) {
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

/*
 * Single-context cores: the microkernel loads list 0 from this table, by the
 * slot a HW context names, so no directory address it loads is GPU-writable.
 * Read-only on the GPU, between the code arena and the kernel code objects.
 * Every slot no VM holds names the firmware's directory.
 */
#define PDTABLE_OFFSET	0xf000
#define PDTABLE_SLOTS	(PAGE_SIZE / sizeof(u32))

/* The PD table sits past the code arena, inside the kernel code heap's first 64 KiB. */
static_assert(PDTABLE_OFFSET >= CODE_ARENA_SIZE);
static_assert(SZ_64K - PAGE_SIZE >= PDTABLE_OFFSET);

static int pdtable_init(struct pvrsgx_device *sdev)
{
	struct pvrsgx_fw *f = &sdev->fw;
	int err;

	if (!pvrsgx_core_single_context(sdev->family))
		return 0;
	err = pvrsgx_kmem_alloc(sdev, &f->pdtable,
				pvrsgx_heaps(sdev)->kernel_code.base + PDTABLE_OFFSET, PAGE_SIZE,
				SGX_MMU_PTE_READONLY);
	if (err)
		return err;
	memset32(f->pdtable.cpu, pvrsgx_mmu_pd_addr(sdev->kernel_mmu), PDTABLE_SLOTS);
	return 0;
}

/* A slot naming directory pd, for a new VM; its HW contexts carry the slot. */
int pvrsgx_fw_pd_slot_get(struct pvrsgx_device *sdev, u32 pd)
{
	int slot = ida_alloc_range(&sdev->fw.pd_slots, 1, PDTABLE_SLOTS - 1, GFP_KERNEL);

	if (slot < 0)
		return slot;
	WRITE_ONCE(((u32 *)sdev->fw.pdtable.cpu)[slot], pd);
	/* In memory before any command names the slot. */
	wmb();
	return slot;
}

/* The VM is gone: its slot names the firmware's directory again. */
void pvrsgx_fw_pd_slot_put(struct pvrsgx_device *sdev, int slot)
{
	WRITE_ONCE(((u32 *)sdev->fw.pdtable.cpu)[slot], pvrsgx_mmu_pd_addr(sdev->kernel_mmu));
	/* The slot names the firmware's directory before another VM can take it. */
	wmb();
	ida_free(&sdev->fw.pd_slots, slot);
}

int pvrsgx_fw_init(struct pvrsgx_device *sdev)
{
	struct pvrsgx_fw *f = &sdev->fw;
	const struct firmware *fw;
	u32 pds_base;
	int err;

	ida_init(&f->pd_slots);
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
	err = pvrsgx_kmem_alloc(sdev, &f->data, pvrsgx_heaps(sdev)->kernel_data.base,
				DATA_ARENA_SIZE,
				SGX_MMU_PTE_CACHECONSISTENT | SGX_MMU_PTE_EDMPROTECT);
	if (err)
		return err;
	err = pvrsgx_kmem_alloc(sdev, &f->code, pvrsgx_heaps(sdev)->kernel_code.base,
				CODE_ARENA_SIZE, SGX_MMU_PTE_READONLY);
	if (err)
		return err;
	err = pdtable_init(sdev);
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
	pvrsgx_kmem_free(sdev, &sdev->fw.pdtable);
	ida_destroy(&sdev->fw.pd_slots);
}

/* USE_CODE_BASE: device address >> 7, and the window's data master (1 pixel, 3 the EDM). */
#define USE_CODE_BASE_ADDR_SHIFT	7
#define USE_CODE_BASE_DM_PIXEL		1
#define USE_CODE_BASE_DM_UKERNEL	3

static u32 use_code_base(struct pvrsgx_device *sdev, u32 n)
{
	return sdev->cr->use_code_base0 + 4 * n;
}

/*
 * Where the event PDS registers are a guess, the program address must read
 * back before anything kicks the microkernel; if not, no kick.
 */
/*
 * The event program's address as the event PDS base register takes it. A core
 * without EDM_VERTEX_PDSADDR_FULL_RANGE holds 26 bits there (stage 1, log
 * 2026-11-11-2144), the 64 MiB the GPL DDK lets PDS code lie above
 * PDS_EXEC_BASE: taken as an offset from it (G).
 */
static u32 event_pds_addr(struct pvrsgx_device *sdev)
{
	u32 a = sdev->fw.addr[PVRSGX_FW_PDS0] & ~0xfu;

	if (!(sdev->family->features & PVRSGX_FEATURE_EDM_VERTEX_PDSADDR_FULL_RANGE))
		a -= pvrsgx_heaps(sdev)->pds_pixel.base;
	return a;
}

static int check_event_pds(struct pvrsgx_device *sdev)
{
	const struct pvrsgx_core_regs *cr = sdev->cr;
	u32 want = event_pds_addr(sdev), got;

	if (!sdev->family->event_pds_guessed)
		return 0;
	got = sgx_rd(sdev, cr->event_pds_base);
	drm_info(&sdev->base, "event PDS base 0x%04x: wrote 0x%08x, read 0x%08x\n",
		 cr->event_pds_base, want, got);
	if (got != want) {
		drm_err(&sdev->base, "event PDS base doesn't read back; not starting the microkernel\n");
		return -EIO;
	}
	return 0;
}

/*
 * Bring-up only, off by default: point code bases 0-14 at the landing pads of
 * the SGX530 bases image (pad n at code offset 0x4380 + 0x80 n writes
 * EVENT_TIMER = 0xc00 + n), so a task that goes through another base than 15
 * says which. The usual data masters (1 for 0-9, 0 for 10-14), or all 3.
 */
static bool code_base_pads;
module_param(code_base_pads, bool, 0444);
MODULE_PARM_DESC(code_base_pads, "Point code bases 0-14 at the SGX530 bases image's landing pads");

#define PAD0_OFFSET	0x4380
#define PAD_STRIDE	0x80

static bool pads_wanted(struct pvrsgx_device *sdev)
{
	return code_base_pads && sdev->family->event_pds_guessed;
}

static void pads_set(struct pvrsgx_device *sdev)
{
	const struct pvrsgx_core_regs *cr = sdev->cr;
	u32 base = (u32)pvrsgx_heaps(sdev)->kernel_code.base + PAD0_OFFSET;

	for (u32 n = 0; n < 15; n++) {
		u32 dm = sdev->sweep_flags & PVRSGX_SWEEP_PADS_DM3 ? 3 :
			 n < 10 ? USE_CODE_BASE_DM_PIXEL : 0;

		sgx_wr(sdev, use_code_base(sdev, n),
		       (base + n * PAD_STRIDE) >> USE_CODE_BASE_ADDR_SHIFT |
		       dm << cr->use_code_base_dm_shift);
	}
}

/* Init script part 2 (docs/SRVINIT.md, "Init scripts"): the core's script, then the bases. */
static int init_script_part2(struct pvrsgx_device *sdev)
{
	const struct pvrsgx_core_family *fam = sdev->family;
	const struct pvrsgx_heap_layout *h = pvrsgx_heaps(sdev);
	const struct pvrsgx_core_regs *cr = sdev->cr;
	struct pvrsgx_fw *f = &sdev->fw;
	u32 dm = cr->use_code_base_dm_shift;
	/*
	 * Event data 0 is where the event program's code starts. The SGX530 counts
	 * dwords (stage 1, log 2026-11-12-0002: the PDS starts at program + 0x0a6c),
	 * so it is the 4-dword data segment; the SGX545's 1 is that in 16-byte rows (G).
	 */
	u32 data0 = fam->features & PVRSGX_FEATURE_PDS_DATA_INTERLEAVE_2DWORDS ? 1 : 4;
	u32 data1 = 6, uk_dm = USE_CODE_BASE_DM_UKERNEL;
	int err;

	/* The bring-up sweep's changes (sweep[] below); none in a normal start. */
	if (sdev->sweep_flags & PVRSGX_SWEEP_DATA0_2)
		data0 = 2;
	if (sdev->sweep_data0)
		data0 = sdev->sweep_data0 - 1;
	if (sdev->sweep_data1)
		data1 = sdev->sweep_data1 - 1;
	if (sdev->sweep_flags & PVRSGX_SWEEP_DM2)
		uk_dm = 2;

	for (u32 i = 0; i < fam->num_init_regs; i++)
		sgx_wr(sdev, fam->init_regs[i].reg, fam->init_regs[i].val);
	/* Code bases 0-9 the pixel shader heap, 10-13 the vertex shader heap, 15 ours. */
	if (pads_wanted(sdev)) {
		pads_set(sdev);
	} else {
		for (u32 i = 0; i < 10; i++)
			sgx_wr(sdev, use_code_base(sdev, PVRSGX_USSE_PIXEL_BASE_INDEX + i),
			       ((u32)(h->usse_pixel.base + (i << PVRSGX_USSE_WINDOW_LOG2)) >>
				USE_CODE_BASE_ADDR_SHIFT) | USE_CODE_BASE_DM_PIXEL << dm);
		for (u32 i = 0; i < 4; i++)
			sgx_wr(sdev, use_code_base(sdev, PVRSGX_USSE_VERTEX_BASE_INDEX + i),
			       (u32)(h->usse_vertex.base + (i << PVRSGX_USSE_WINDOW_LOG2)) >>
			       USE_CODE_BASE_ADDR_SHIFT);
	}
	sgx_wr(sdev, use_code_base(sdev, PVRSGX_USSE_UKERNEL_BASE_INDEX),
	       ((u32)h->kernel_code.base >> USE_CODE_BASE_ADDR_SHIFT) | uk_dm << dm);
	sgx_wr(sdev, cr->pds_exec_base, h->pds_pixel.base);
	sgx_wr_opt(sdev, cr->vendor_b30, 0x100);
	sgx_wr(sdev, SGX_CR_EVENT_KICKER, f->addr[PVRSGX_FW_KICKER]);
	sgx_wr(sdev, cr->event_pds_base, event_pds_addr(sdev));
	sgx_wr(sdev, cr->event_pds_data0, data0);
	sgx_wr(sdev, cr->event_pds_data1, data1);
	/* Before the events are enabled, so a wrong guess starts nothing. */
	err = check_event_pds(sdev);
	if (err)
		return err;
	sgx_wr(sdev, cr->event_pds_enable, sdev->sweep_flags & PVRSGX_SWEEP_ENABLE ?
	       sdev->sweep_enable : cr->event_pds_enable_all);
	return 0;
}

static void deinit_script(struct pvrsgx_device *sdev)
{
	const struct pvrsgx_core_regs *cr = sdev->cr;

	sgx_wr(sdev, cr->event_pds_enable, 0);
	sgx_wr(sdev, cr->event_pds_base, 0);
	sgx_wr(sdev, cr->event_pds_data0, 0);
	sgx_wr(sdev, cr->event_pds_data1, 0);
	sgx_wr(sdev, use_code_base(sdev, PVRSGX_USSE_UKERNEL_BASE_INDEX), 0);
	sgx_wr(sdev, SGX_CR_EVENT_HOST_ENABLE, 0);
	sgx_wr(sdev, SGX_CR_EVENT_HOST_ENABLE2, 0);
	sgx_wr(sdev, SGX_CR_CLKGATECTL, 0);
	sgx_wr_opt(sdev, cr->clkgatectl2, 0);
}

/*
 * Bring-up logs for a core whose event registers are a guess: named registers
 * only, and the OMAP OCP wrapper's (SGX_OCP_*) where it is mapped.
 */
static void bringup_diag(struct pvrsgx_device *sdev, const char *when)
{
	const struct pvrsgx_core_regs *cr = sdev->cr;

	drm_info(&sdev->base,
		 "%s: clock status 0x%08x event status 0x%08x BIF req 0x%x int 0x%x timer 0x%08x\n",
		 when, sgx_rd(sdev, cr->clkgatestatus), sgx_rd(sdev, SGX_CR_EVENT_STATUS),
		 sgx_rd(sdev, SGX_CR_BIF_MEM_REQ_STAT), sgx_rd(sdev, SGX_CR_BIF_INT_STAT),
		 sgx_rd(sdev, SGX_CR_EVENT_TIMER));
	if (cr->pds_pc_base)
		drm_info(&sdev->base, "%s: PDS PC base 0x%08x PDS 0x%08x\n", when,
			 sgx_rd(sdev, cr->pds_pc_base), sgx_rd(sdev, cr->pds_ctrl));
	if (sdev->ocp)
		drm_info(&sdev->base,
			 "%s: OCP revision 0x%08x sysconfig 0x%08x debug config 0x%08x status 0x%08x\n",
			 when, readl(sdev->ocp + SGX_OCP_REVISION),
			 readl(sdev->ocp + SGX_OCP_SYSCONFIG),
			 readl(sdev->ocp + SGX_OCP_DEBUG_CONFIG),
			 readl(sdev->ocp + SGX_OCP_DEBUG_STATUS));
}

/*
 * Bring-up only: for the first start of a core with guessed event registers,
 * map code-arena pages where code base 15 would start under other units of
 * USE_CODE_BASE's address field (2^k bytes; ours is 128). Each page holds a
 * probe of the SGX530 probe image that writes EVENT_TIMER = 0xb00 + k.
 */
static bool code_base_probes;
module_param(code_base_probes, bool, 0444);
MODULE_PARM_DESC(code_base_probes, "Map code-base unit probes on a guessed core's first start");

/* The image's probe for a unit of 2^k bytes, at offset off in the code arena. */
static const struct {
	u32 off, k;
} code_base_probe[] = {
	{ 0x5000, 0 }, { 0x6000, 1 }, { 0x7000, 2 }, { 0x8000, 3 }, { 0x9000, 4 },
	{ 0xa000, 5 }, { 0xb000, 6 }, { 0xc000, 8 }, { 0xd000, 9 },
};

static bool probes_wanted(struct pvrsgx_device *sdev)
{
	return code_base_probes && sdev->family->event_pds_guessed;
}

/*
 * Where code base 15 starts if its unit is 2^k bytes, or 0 if that address
 * is past the core's space or in a kernel heap mapped already.
 */
static u64 probe_va(struct pvrsgx_device *sdev, unsigned int i)
{
	const struct pvrsgx_heap_layout *h = pvrsgx_heaps(sdev);
	u64 va = (h->kernel_code.base >> USE_CODE_BASE_ADDR_SHIFT) << code_base_probe[i].k;
	const struct pvrsgx_va_range *kh[] = { &h->kernel_code, &h->kernel_data, &h->sync };

	if (va & (PAGE_SIZE - 1) || va + PAGE_SIZE > BIT_ULL(sdev->family->va_bits))
		return 0;
	for (unsigned int j = 0; j < ARRAY_SIZE(kh); j++)
		if (va < kh[j]->end && va + PAGE_SIZE > kh[j]->base)
			return 0;
	return va;
}

static void probes_unmap(struct pvrsgx_device *sdev, unsigned int n)
{
	for (unsigned int i = 0; i < n; i++)
		if (probe_va(sdev, i))
			pvrsgx_mmu_unmap(sdev->kernel_mmu, probe_va(sdev, i), PAGE_SIZE);
}

static int probes_map(struct pvrsgx_device *sdev)
{
	struct pvrsgx_kmem *code = &sdev->fw.code;

	for (unsigned int i = 0; i < ARRAY_SIZE(code_base_probe); i++) {
		u64 va = probe_va(sdev, i);
		int err;

		if (!va)
			continue;
		err = pvrsgx_mmu_map_pages(sdev->kernel_mmu, va,
					   &code->pages[code_base_probe[i].off >> PAGE_SHIFT], 1,
					   SGX_MMU_PTE_READONLY);
		if (err) {
			probes_unmap(sdev, i);
			return err;
		}
		drm_info(&sdev->base, "code-base unit probe %u B at 0x%08llx\n",
			 1u << code_base_probe[i].k, va);
	}
	return 0;
}

/* Wait for init; on a guessed core, log every 100 ms that it doesn't come. */
static int wait_for_init(struct pvrsgx_device *sdev, const u32 *status)
{
	char when[32];
	int err = -ETIMEDOUT;

	if (!sdev->family->event_pds_guessed)
		return pvrsgx_poll(sdev, status, HOSTCTL_INIT_COMPLETE, HOSTCTL_INIT_COMPLETE,
				   500000);
	for (int i = 1; i <= 5; i++) {
		err = pvrsgx_poll(sdev, status, HOSTCTL_INIT_COMPLETE, HOSTCTL_INIT_COMPLETE,
				  100000);
		if (!err)
			return 0;
		snprintf(when, sizeof(when), "%d ms after the kick", i * 100);
		bringup_diag(sdev, when);
	}
	return err;
}

/* Every unit's clock gated automatically, but the DPM's: it stays on. */
static void clocks_on(struct pvrsgx_device *sdev)
{
	const struct pvrsgx_core_regs *cr = sdev->cr;

	sgx_wr(sdev, SGX_CR_CLKGATECTL,
	       (SGX_CLKG_ALL(cr->clkgatectl_units, SGX_CLKG_AUTO) & ~cr->clkgatectl_dpm) |
	       field_prep(cr->clkgatectl_dpm, SGX_CLKG_ON));
	sgx_wr_opt(sdev, cr->clkgatectl2, SGX_CLKG_ALL(cr->clkgatectl2_units, SGX_CLKG_AUTO));
}

/* The init kick; a core with one kick register (BRN_22934) starts on EVENT_KICK. */
/*
 * The init kick. With one kick register, the event-kicker word goes up by one
 * first, as for every CCB kick: the GPL DDK's SGXInitialise does exactly this
 * without MULTI_EVENT_KICK.
 */
static void init_kick(struct pvrsgx_device *sdev)
{
	const struct pvrsgx_core_regs *cr = sdev->cr;
	u32 *kicker = sdev->fw.cpu[PVRSGX_FW_KICKER];

	if (cr->event_kick2) {
		sgx_wr(sdev, cr->event_kick2, cr->event_kick2_now);
		return;
	}
	*kicker = (*kicker + 1) & 0xff;
	/* The kicker word is in memory before the kick. */
	wmb();
	sgx_wr(sdev, SGX_CR_EVENT_KICK, SGX_CR_EVENT_KICK_NOW);
}

/*
 * Bring-up only, off by default: on a guessed core's first start, if the
 * diagnostic entry's ladder doesn't run within 100 ms, reset and retry with
 * the next combination of changes to the event program's hand-off to the
 * USSE (docs/porting/cores/sgx530-stage1.md, "After the eighth attempt").
 */
static bool stage1_sweep;
module_param(stage1_sweep, bool, 0444);
MODULE_PARM_DESC(stage1_sweep, "Try variants of the event program setup on a guessed core's first start");

/*
 * Each variant one change from the normal start, + 1 where 0 means none:
 * the event program's MOVS destination and condition, the event data words
 * 0xa6c and 0xa70; or flags.
 */
struct sweep_variant {
	u32 flags, enable;
	u8 dest, cc, data0, data1;
};

#define D1(b)		{ .data1 = (b) + 1 }
#define F(f)		{ .flags = (f) }

static const struct sweep_variant sweep[] = {
	{ 0 },
	D1(0), D1(1), D1(2), D1(3), D1(4), D1(8), D1(16),
	{ .dest = 4 + 1 }, { .dest = 6 + 1 }, { .dest = 7 + 1 }, { .dest = 1 + 1 },
	{ .dest = 0 + 1 }, { .dest = 2 + 1 }, { .cc = 3 + 1 }, { .cc = 0 + 1 },
	F(PVRSGX_SWEEP_DOUTU_W1_0), F(PVRSGX_SWEEP_INTERLEAVE), F(PVRSGX_SWEEP_ALL_SLOTS),
	F(PVRSGX_SWEEP_TEMPS4), F(PVRSGX_SWEEP_TEMPS0), F(PVRSGX_SWEEP_DM2),
	F(PVRSGX_SWEEP_DOUTU_W1_0 | PVRSGX_SWEEP_DM2),
	F(PVRSGX_SWEEP_ALL_SLOTS | PVRSGX_SWEEP_TEMPS4),
};

#undef D1
#undef F

#undef DATA

static void sweep_name(const struct sweep_variant *sv, char *buf, size_t len)
{
	if (sv->data0)
		snprintf(buf, len, "event data %d, %d", sv->data0 - 1, sv->data1 - 1);
	else if (sv->data1)
		snprintf(buf, len, "event data 1 = %d", sv->data1 - 1);
	else if (sv->flags & PVRSGX_SWEEP_ENABLE)
		snprintf(buf, len, "event PDS enable 0x%08x", sv->enable);
	else if (sv->dest || sv->cc)
		snprintf(buf, len, "MOVS dest %d cc %d", sv->dest - 1, sv->cc - 1);
	else if (sv->flags)
		snprintf(buf, len, "flags 0x%x", sv->flags);
	else
		snprintf(buf, len, "baseline");
}

/*
 * Whether the diagnostic ladder ran: its EVENT_TIMER write (0xa5a, timer
 * off), a code-base probe's (0xb00 + k), its wait-word marker, or the
 * heartbeat. An enabled timer counts down by itself, so a changed
 * EVENT_TIMER alone proves nothing.
 */
#define LADDER_TIMER		0xa5a
#define LADDER_MARKER		0xd1a60001

static bool ladder_seen(struct pvrsgx_device *sdev)
{
	const u32 *t = fw_words(sdev, PVRSGX_FW_TA3DCTL);
	u32 timer = sgx_rd(sdev, SGX_CR_EVENT_TIMER);

	return timer == LADDER_TIMER || (timer >= 0xb00 && timer <= 0xb09) ||
	       (timer >= 0xc00 && timer <= 0xc0e) ||
	       READ_ONCE(t[0x28]) == LADDER_MARKER || READ_ONCE(t[0x2a]);
}

/* Poll for 100 ms, logging PDS_PC_BASE every 10 ms: where the PDS is. */
static bool ladder_ran(struct pvrsgx_device *sdev, const char *name)
{
	const struct pvrsgx_core_regs *cr = sdev->cr;
	u32 pc[10] = {};
	bool seen = false;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pc) && !seen; i++) {
		usleep_range(10000, 11000);
		if (cr->pds_pc_base)
			pc[i] = sgx_rd(sdev, cr->pds_pc_base);
		seen = ladder_seen(sdev);
	}
	drm_info(&sdev->base,
		 "sweep %s: PDS PC %x %x %x %x %x %x %x %x %x %x, timer 0x%08x\n",
		 name, pc[0], pc[1], pc[2], pc[3], pc[4], pc[5], pc[6], pc[7], pc[8], pc[9],
		 sgx_rd(sdev, SGX_CR_EVENT_TIMER));
	return seen;
}

/*
 * The sweep's last step, if nothing ran: does this core report a BIF fault
 * at all? Point EVENT_KICKER at the hole below the kernel code heap, where
 * nothing is mapped, kick, and log BIF_INT_STAT, BIF_FAULT and
 * BIF_MEM_REQ_STAT every 10 ms; then the recovery reset, which completes
 * faulting requests on its scratch page.
 */
static int fault_check(struct pvrsgx_device *sdev, u32 *hc)
{
	const struct pvrsgx_heap_layout *h = pvrsgx_heaps(sdev);
	u32 hole = h->pds_vertex.end, st[10], fa[10], rq[10];
	int err;

	if (hole >= h->kernel_code.base)
		return 0;
	sgx_wr(sdev, sdev->cr->event_pds_enable, 0);
	sdev->sweep_flags = 0;
	sdev->sweep_dest = 0;
	sdev->sweep_cc = 0;
	sdev->sweep_data0 = 0;
	sdev->sweep_data1 = 0;
	pvrsgx_pds_entry(sdev, PVRSGX_FW_PDS0, sdev->fw.event_entry);
	pvrsgx_reset(sdev, true);
	err = init_script_part2(sdev);
	if (err)
		return err;
	sgx_wr(sdev, SGX_CR_EVENT_KICKER, hole);
	hc[HOSTCTL_INIT_STATUS] = 0;
	/* The host control word is in memory before the kick. */
	wmb();
	init_kick(sdev);
	for (unsigned int i = 0; i < ARRAY_SIZE(st); i++) {
		usleep_range(10000, 11000);
		st[i] = sgx_rd(sdev, SGX_CR_BIF_INT_STAT);
		fa[i] = sgx_rd(sdev, SGX_CR_BIF_FAULT);
		rq[i] = sgx_rd(sdev, SGX_CR_BIF_MEM_REQ_STAT);
	}
	drm_info(&sdev->base,
		 "fault check, EVENT_KICKER 0x%08x: BIF int %x %x %x %x %x %x %x %x %x %x\n",
		 hole, st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7], st[8], st[9]);
	drm_info(&sdev->base,
		 "fault check: BIF fault %x %x %x %x %x %x %x %x %x %x req %x %x %x %x %x %x %x %x %x %x\n",
		 fa[0], fa[1], fa[2], fa[3], fa[4], fa[5], fa[6], fa[7], fa[8], fa[9],
		 rq[0], rq[1], rq[2], rq[3], rq[4], rq[5], rq[6], rq[7], rq[8], rq[9]);
	/* Recovery: the kicker back, no kick, the reset that completes faults. */
	sgx_wr(sdev, SGX_CR_EVENT_KICKER, sdev->fw.addr[PVRSGX_FW_KICKER]);
	pvrsgx_reset(sdev, true);
	st[0] = sgx_rd(sdev, SGX_CR_BIF_INT_STAT);
	if (st[0] & sdev->cr->bif_int_stat_fault_req) {
		/* Still faulting: hold the cores in reset and touch nothing more. */
		drm_err(&sdev->base, "fault check: BIF int 0x%x after recovery; cores held in reset\n",
			st[0]);
		sgx_wr(sdev, SGX_CR_SOFT_RESET,
		       sdev->cr->soft_reset_bif | sdev->cr->soft_reset_cores);
		sdev->lost = true;
		return -EIO;
	}
	drm_info(&sdev->base, "fault check: recovered, BIF int 0x%x\n", st[0]);
	return 0;
}

static int start_sweep(struct pvrsgx_device *sdev, u32 *hc, u32 clk_idle)
{
	const struct pvrsgx_core_regs *cr = sdev->cr;
	char when[64];
	u32 clk;
	int err;

	for (unsigned int v = 0; v < ARRAY_SIZE(sweep); v++) {
		const struct sweep_variant *sv = &sweep[v];
		char name[40];

		if ((sv->flags & PVRSGX_SWEEP_PADS_DM3) && !pads_wanted(sdev))
			continue;
		sweep_name(sv, name, sizeof(name));
		if (v) {
			sgx_wr(sdev, cr->event_pds_enable, 0);
			sgx_wr(sdev, SGX_CR_EVENT_TIMER, 0);
			sdev->sweep_flags = sv->flags;
			sdev->sweep_dest = sv->dest;
			sdev->sweep_cc = sv->cc;
			sdev->sweep_data0 = sv->data0;
			sdev->sweep_data1 = sv->data1;
			sdev->sweep_enable = sv->enable;
			pvrsgx_pds_entry(sdev, PVRSGX_FW_PDS0, sdev->fw.event_entry);
			pvrsgx_reset(sdev, true);
			/*
			 * The SGX530's SOFT_RESET has no PDS bit: a unit still busy
			 * here makes this variant depend on the last one.
			 */
			clk = sgx_rd(sdev, cr->clkgatestatus);
			if (clk != clk_idle)
				drm_warn(&sdev->base,
					 "sweep variant %u: clock status 0x%08x after the reset, 0x%08x before the first kick: not independent of variant %u\n",
					 v, clk, clk_idle, v - 1);
			*(u32 *)sdev->fw.cpu[PVRSGX_FW_KICKER] = 0;
			err = init_script_part2(sdev);
			if (err)
				return err;
			hc[HOSTCTL_INIT_STATUS] = 0;
			/* The rebuilt program is in memory before the kick. */
			wmb();
			init_kick(sdev);
		}
		snprintf(when, sizeof(when), "variant %u (%s)", v, name);
		if (ladder_ran(sdev, when)) {
			drm_info(&sdev->base, "sweep: %s runs the diagnostic ladder\n", when);
			return 0;
		}
		bringup_diag(sdev, when);
	}
	drm_info(&sdev->base, "sweep: no variant runs the diagnostic ladder\n");
	return fault_check(sdev, hc);
}

/*
 * Start: clocks, init script part 1, reset, part 2, then the first event kick,
 * and the microkernel's init entry sets INIT_COMPLETE.
 */
int pvrsgx_fw_start(struct pvrsgx_device *sdev)
{
	const struct pvrsgx_core_regs *cr = sdev->cr;
	u32 *hc = fw_words(sdev, PVRSGX_FW_HOSTCTL);
	bool first = !sdev->fw_started, was_powered = READ_ONCE(sdev->powered);
	u32 clk_idle = 0;
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
			sgx_wr(sdev, SGX_CR_SOFT_RESET, cr->soft_reset_bif | cr->soft_reset_cores);
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
	hc[HOSTCTL_UK_TIMER_CLOCK] = sdev->core->clock_hz / UKERNEL_TIMER_HZ;
	hc[HOSTCTL_FLAGS] = HOSTCTL_HOST_BGBLOCK;
	/* The power status starts clear at every start. */
	hc[HOSTCTL_POWER_STATUS] = 0;
	pvrsgx_ccb_reset(sdev);
	ta3dctl_fill(sdev);
	/* The first command invalidates the directory cache. */
	sdev->fw.cache_control = PVRSGX_CC_INVAL_BIF_PD;

	clocks_on(sdev);
	if (sdev->family->event_pds_guessed && first)
		bringup_diag(sdev, "clocks on");
	if (sdev->family->event_pds_guessed && first)
		drm_info(&sdev->base,
			 "event PDS registers before any write: enable 0x%08x base 0x%08x data 0x%08x 0x%08x\n",
			 sgx_rd(sdev, cr->event_pds_enable), sgx_rd(sdev, cr->event_pds_base),
			 sgx_rd(sdev, cr->event_pds_data0), sgx_rd(sdev, cr->event_pds_data1));
	sgx_wr(sdev, cr->event_pds_enable, 0);
	sgx_wr(sdev, SGX_CR_EVENT_TIMER, 0);

	/* Before the reset: it invalidates the directory cache. */
	if (first && probes_wanted(sdev)) {
		err = probes_map(sdev);
		if (err)
			return err;
	}
	pvrsgx_reset(sdev, true);

	*(u32 *)sdev->fw.cpu[PVRSGX_FW_KICKER] = 0;
	err = init_script_part2(sdev);
	if (err)
		return err;

	hc[HOSTCTL_INIT_STATUS] = 0;
	if (sdev->family->event_pds_guessed) {
		clk_idle = sgx_rd(sdev, cr->clkgatestatus);
		bringup_diag(sdev, "before the kick");
	}
	/* The image and the host control block are in memory before the kick. */
	wmb();
	init_kick(sdev);

	err = 0;
	if (first && stage1_sweep && sdev->family->event_pds_guessed)
		err = start_sweep(sdev, hc, clk_idle);
	if (!err)
		err = wait_for_init(sdev, &hc[HOSTCTL_INIT_STATUS]);
	if (first && probes_wanted(sdev))
		probes_unmap(sdev, ARRAY_SIZE(code_base_probe));
	if (err && sdev->lost)
		return err;
	if (err) {
		drm_err(&sdev->base,
			"microkernel did not start (init status 0x%x, event status 0x%x; BIF int 0x%x fault 0x%x req 0x%x)\n",
			hc[HOSTCTL_INIT_STATUS], sgx_rd(sdev, SGX_CR_EVENT_STATUS),
			sgx_rd(sdev, SGX_CR_BIF_INT_STAT), sgx_rd(sdev, SGX_CR_BIF_FAULT),
			sgx_rd(sdev, SGX_CR_BIF_MEM_REQ_STAT));
		/* Memory only: a heartbeat of 0 means the event program never ran. */
		if (sdev->family->event_pds_guessed)
			pvrsgx_fw_dump_status_words(sdev);
		return err;
	}
	if (sdev->family->event_pds_guessed)
		pvrsgx_fw_dump_status_words(sdev);
	sdev->fw_running = true;
	sdev->fw_started = true;
	if (first || sdev->reset_count != sdev->reset_logged) {
		drm_info(&sdev->base, "microkernel started%s\n", first ? "" : " again");
		sdev->reset_logged = sdev->reset_count;
	}
	return 0;
}

/* Ask the microkernel to power off, then hold the cores in reset. */
void pvrsgx_fw_stop(struct pvrsgx_device *sdev)
{
	const struct pvrsgx_core_regs *cr = sdev->cr;
	u32 *hc = fw_words(sdev, PVRSGX_FW_HOSTCTL);
	u32 data[6] = { 0, POWER_OFF };

	if (sdev->fw_running) {
		if (pvrsgx_ccb_send(sdev, PVRSGX_CMD_POWER, data, NULL) ||
		    pvrsgx_poll(sdev, &hc[HOSTCTL_POWER_STATUS], HOSTCTL_POWMAN_POWEROFF_COMPLETE,
				HOSTCTL_POWMAN_POWEROFF_COMPLETE, 500000))
			drm_warn(&sdev->base, "microkernel did not power off\n");
		/* Wait for the clocks to gate; the DPM's stays on. */
		for (int i = 0; i < 500; i++) {
			if (!(sgx_rd(sdev, cr->clkgatestatus) & ~cr->clkgatestatus_dpm))
				break;
			usleep_range(100, 200);
		}
		WRITE_ONCE(sdev->powered, false);
		synchronize_irq(sdev->irq);
		deinit_script(sdev);
		sdev->fw_running = false;
		sgx_wr(sdev, SGX_CR_SOFT_RESET, cr->soft_reset_bif | cr->soft_reset_cores);
		sgx_rd(sdev, SGX_CR_SOFT_RESET);
	} else if (READ_ONCE(sdev->powered)) {
		/* A failed start: the clocks are on, the microkernel isn't. */
		WRITE_ONCE(sdev->powered, false);
		synchronize_irq(sdev->irq);
		/* A start that left the BIF faulting holds the cores in reset already. */
		if (sdev->lost)
			return;
		sgx_wr(sdev, SGX_CR_SOFT_RESET, cr->soft_reset_bif | cr->soft_reset_cores);
		sgx_rd(sdev, SGX_CR_SOFT_RESET);
	}
}

#if IS_ENABLED(CONFIG_DRM_POWERVR_SGX_KUNIT_TEST)
#include "tests/pvrsgx_fw_test.c"
#include "tests/pvrsgx_regs_test.c"
#endif
