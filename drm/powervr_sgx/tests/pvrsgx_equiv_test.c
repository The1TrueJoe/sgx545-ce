// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * The SGX545 path, recorded: every register write, in order, and the bytes
 * of every structure the kernel builds for the microkernel, through a start,
 * a command, a recovery, a stop, a resume, a render and a transfer context
 * and the heap query. Included from pvrsgx_fw.c so it reaches the loader's
 * static functions.
 *
 * Registers are a memory buffer, and a stand-in for the microkernel answers
 * the init kick, takes every command and confirms the power-off. Physical
 * addresses, which differ between runs, are recorded as the directory they
 * belong to (PD0, PD1, ...) or as page flags only.
 *
 * Each phase's record is checked against pvrsgx_equiv_golden.h: its line
 * count and CRC32. A change meant to leave the SGX545 alone must leave every
 * record as it is. On a mismatch the whole record is printed, one "EQ|" line
 * per entry, for a diff against tests/pvrsgx_equiv_545.txt.
 */

#include <kunit/test.h>

#include <drm/drm_kunit_helpers.h>

#include <linux/mman.h>
#include <linux/uaccess.h>

#include "../pvrsgx_job.h"
#include "../pvrsgx_render.h"
#include "../pvrsgx_transfer.h"
#include "../pvrsgx_vm.h"

#include "pvrsgx_equiv_golden.h"

#define EQ_REGS_BYTES	0x10000
#define EQ_LINE		64
#define EQ_MAX_LINES	4096
#define EQ_MAX_PDS	8

struct eq_fx {
	struct kunit *test;
	struct pvrsgx_device *sdev;
	u32 *regs;
	/* Physical directory addresses, recorded as their index. */
	u32 pd[EQ_MAX_PDS];
	unsigned int npd;
	/* The current phase's record. */
	char (*line)[EQ_LINE];
	unsigned int n;
	bool overflow;
	/* BIF faults left for the next recovery to find. */
	unsigned int faults;
};

/* The fixture of the test that is running, for the write hook. */
static struct eq_fx *eq_cur;

static __printf(2, 3) void eq_add(struct eq_fx *fx, const char *fmt, ...)
{
	va_list ap;

	if (fx->n == EQ_MAX_LINES) {
		fx->overflow = true;
		return;
	}
	va_start(ap, fmt);
	vsnprintf(fx->line[fx->n++], EQ_LINE, fmt, ap);
	va_end(ap);
}

static void eq_pd(struct eq_fx *fx, struct pvrsgx_mmu *mmu)
{
	if (fx->npd < EQ_MAX_PDS)
		fx->pd[fx->npd++] = pvrsgx_mmu_pd_addr(mmu);
}

/* A value as recorded: a directory's index, or the value. */
static const char *eq_val(struct eq_fx *fx, u32 v, char buf[16])
{
	for (unsigned int i = 0; i < fx->npd; i++) {
		if (v == fx->pd[i]) {
			snprintf(buf, 16, "PD%u", i);
			return buf;
		}
	}
	snprintf(buf, 16, "%08x", v);
	return buf;
}

/* The microkernel's part, answered at once. */
static void eq_wr(struct pvrsgx_device *sdev, u32 reg, u32 val)
{
	struct eq_fx *fx = eq_cur;
	u32 *hc = sdev->fw.cpu[PVRSGX_FW_HOSTCTL];
	char b[16];

	eq_add(fx, "wr %04x %s", reg, eq_val(fx, val, b));

	if (reg == sdev->cr->event_kick2 && val == sdev->cr->event_kick2_now)
		hc[HOSTCTL_INIT_STATUS] |= HOSTCTL_INIT_COMPLETE;
	if (reg == SGX_CR_EVENT_KICK && val == SGX_CR_EVENT_KICK_NOW) {
		u32 *ctl = sdev->fw.cpu[PVRSGX_FW_CCBCTL];
		u32 last = (ctl[0] - 1) & (PVRSGX_CCB_SLOTS - 1);
		u32 *cmd = (u32 *)sdev->fw.cpu[PVRSGX_FW_CCB] + last * PVRSGX_CCB_CMD_WORDS;

		if (cmd[0] == PVRSGX_KCMD_POWER)
			hc[HOSTCTL_POWER_STATUS] |= HOSTCTL_POWMAN_POWEROFF_COMPLETE;
		ctl[1] = ctl[0];
	}
	/* The recovery loop clears the events once per fault it handles. */
	if (reg == SGX_CR_EVENT_HOST_CLEAR && fx->faults && !--fx->faults)
		fx->regs[SGX_CR_BIF_INT_STAT / 4] = 0;
}

/* Nonzero words of n bytes at p; word pid_word (if not -1) as "PID". */
static void eq_words(struct eq_fx *fx, const char *name, const void *p, u32 n, int pid_word)
{
	const u32 *w = p;
	char b[16];

	for (u32 i = 0; i < n / 4; i++) {
		u32 v = READ_ONCE(w[i]);

		if (!v)
			continue;
		if ((int)i == pid_word)
			eq_add(fx, "%s +%04x PID", name, i * 4);
		else
			eq_add(fx, "%s +%04x %s", name, i * 4, eq_val(fx, v, b));
	}
}

/* Every valid PDE's flags, and the PTE flags in runs of equal flags. */
static void eq_mmu(struct eq_fx *fx, const char *name, struct pvrsgx_mmu *mmu)
{
	for (u32 d = 0; d < SGX_MMU_PD_ENTRIES; d++) {
		u32 base = d * SGX_MMU_PDE_SPAN, pde, pte, run = 0, flags = 0, start = 0;

		KUNIT_ASSERT_TRUE(fx->test, pvrsgx_mmu_lookup(mmu, base, &pde, &pte));
		if (!(pde & SGX_MMU_PDE_VALID))
			continue;
		eq_add(fx, "%s pde %03x %x", name, d, (u32)(pde & ~SGX_MMU_ADDR_MASK));
		for (u32 i = 0; i <= SGX_MMU_PT_ENTRIES; i++) {
			u32 a = base + i * SGX_MMU_PAGE_SIZE, f = 0;

			if (i < SGX_MMU_PT_ENTRIES) {
				KUNIT_ASSERT_TRUE(fx->test, pvrsgx_mmu_lookup(mmu, a, &pde, &pte));
				f = pte & ~SGX_MMU_ADDR_MASK;
			}
			if (run && (i == SGX_MMU_PT_ENTRIES || f != flags)) {
				if (flags)
					eq_add(fx, "%s pte %08x+%x %x", name, start, run, flags);
				run = 0;
			}
			if (i < SGX_MMU_PT_ENTRIES && !run) {
				start = a;
				flags = f;
			}
			run++;
		}
	}
}

static void eq_arenas(struct eq_fx *fx)
{
	struct pvrsgx_fw *f = &fx->sdev->fw;

	eq_add(fx, "data %08x %x", f->data.dev_addr, f->data.size);
	eq_words(fx, "data", f->data.cpu, f->data.size, -1);
	eq_add(fx, "code %08x %x", f->code.dev_addr, f->code.size);
	eq_words(fx, "code", f->code.cpu, f->code.size, -1);
	for (int b = 0; b < PVRSGX_FW_NBUF; b++)
		eq_add(fx, "buf %d %08x", b, f->addr[b]);
}

/* Close a phase: compare its record with the golden one, print it if they differ. */
static void eq_check(struct eq_fx *fx, const char *phase)
{
	const struct pvrsgx_equiv_golden *g = NULL;
	u32 crc = ~0u;

	KUNIT_EXPECT_FALSE_MSG(fx->test, fx->overflow, "%s: record overflow", phase);
	for (unsigned int i = 0; i < fx->n; i++)
		crc = crc32_le(crc, fx->line[i], strlen(fx->line[i]) + 1);
	crc = ~crc;
	for (unsigned int i = 0; i < ARRAY_SIZE(pvrsgx_equiv_golden_545); i++)
		if (pvrsgx_equiv_golden_545[i].phase &&
		    !strcmp(pvrsgx_equiv_golden_545[i].phase, phase))
			g = &pvrsgx_equiv_golden_545[i];

	if (!g || g->lines != fx->n || g->crc != crc) {
		for (unsigned int i = 0; i < fx->n; i++)
			kunit_info(fx->test, "EQ| %s %s\n", phase, fx->line[i]);
		kunit_info(fx->test, "EQ= { \"%s\", %u, 0x%08x },\n", phase, fx->n, crc);
		KUNIT_FAIL(fx->test, "%s: %u lines, crc 0x%08x; golden %u, 0x%08x", phase, fx->n,
			   crc, g ? g->lines : 0, g ? g->crc : 0);
	}
	fx->n = 0;
	fx->overflow = false;
}

/* A microkernel file: three entries and a patch of each kind. */
#define EQ_IMAGE	0x100

struct eq_file {
	struct ukfw_header h;
	struct ukfw_pair entries[3];
	struct ukfw_pair patches[5];
	u8 image[EQ_IMAGE];
};

static struct eq_file *eq_file(struct kunit *test)
{
	static const u32 patch[][2] = {
		{ 0x10, UKFW_TA3DCTL }, { 0x20, UKFW_HOSTCTL }, { 0x30, UKFW_CCBCTL },
		{ 0x48, UKFW_CCB }, { 0x58, UKFW_MISC },
	};
	struct eq_file *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	u32 *w;

	KUNIT_ASSERT_NOT_NULL(test, t);
	w = (u32 *)t->image;
	for (u32 i = 0; i < EQ_IMAGE / 4; i++)
		w[i] = 0x01010101u * (i + 1);
	for (u32 i = 0; i < ARRAY_SIZE(patch); i++) {
		w[patch[i][0] / 4] = 0xf80dbeef;
		w[patch[i][0] / 4 + 1] = 0xfc000000;
		t->patches[i] = (struct ukfw_pair){ cpu_to_le32(patch[i][0]),
						    cpu_to_le32(patch[i][1]) };
	}
	t->h.magic[0] = cpu_to_le32(UKFW_MAGIC0);
	t->h.magic[1] = cpu_to_le32(UKFW_MAGIC1);
	t->h.header_size = cpu_to_le32(offsetof(struct eq_file, image));
	t->h.interface = cpu_to_le32(1 << 16);
	t->h.features = cpu_to_le32(PVRSGX_UKFW_CLEANUP_V2 | PVRSGX_UKFW_SECURE |
				    PVRSGX_UKFW_DONE_SEQ);
	t->h.image_size = cpu_to_le32(EQ_IMAGE);
	t->h.image_crc32 = cpu_to_le32(~crc32_le(~0, t->image, EQ_IMAGE));
	t->h.n_entries = cpu_to_le32(ARRAY_SIZE(t->entries));
	t->h.entries_off = cpu_to_le32(offsetof(struct eq_file, entries));
	t->h.n_patches = cpu_to_le32(ARRAY_SIZE(t->patches));
	t->h.patches_off = cpu_to_le32(offsetof(struct eq_file, patches));
	t->entries[0] = (struct ukfw_pair){ cpu_to_le32(0), cpu_to_le32(0) };
	t->entries[1] = (struct ukfw_pair){ cpu_to_le32(2), cpu_to_le32(0x40) };
	t->entries[2] = (struct ukfw_pair){ cpu_to_le32(4), cpu_to_le32(0x80) };
	return t;
}

static void eq_noop_work(struct work_struct *work)
{
}

/* pvrsgx_device_init() up to the start, with the firmware from eq_file(). */
static struct eq_fx *eq_device(struct kunit *test, unsigned int core, u32 core_rev)
{
	struct eq_fx *fx = kunit_kzalloc(test, sizeof(*fx), GFP_KERNEL);
	struct pvrsgx_device *sdev;
	struct firmware fw = {};
	struct eq_file *file;
	struct device *dev;

	KUNIT_ASSERT_NOT_NULL(test, fx);
	fx->test = test;
	fx->line = kunit_kcalloc(test, EQ_MAX_LINES, EQ_LINE, GFP_KERNEL);
	fx->regs = kunit_kzalloc(test, EQ_REGS_BYTES, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, fx->line);
	KUNIT_ASSERT_NOT_NULL(test, fx->regs);

	dev = drm_kunit_helper_alloc_device(test);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	sdev = drm_kunit_helper_alloc_drm_device(test, dev, struct pvrsgx_device, base, 0);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sdev);
	fx->sdev = sdev;

	mutex_init(&sdev->ccb_lock);
	mutex_init(&sdev->done_lock);
	spin_lock_init(&sdev->ccb_spin);
	spin_lock_init(&sdev->inval_lock);
	INIT_LIST_HEAD(&sdev->inval_waits);
	INIT_WORK(&sdev->reap_work, eq_noop_work);
	/* The restart path queues the reap; the real driver's own workqueue isn't set up here. */
	sdev->wq = system_dfl_wq;
	sdev->irq = -1;
	sdev->regs = (void __iomem __force *)fx->regs;

	sdev->family = pvrsgx_core_family_get(core);
	KUNIT_ASSERT_NOT_NULL(test, sdev->family);
	sdev->cr = sdev->family->regs;
	sdev->core_id = sdev->family->core_id;
	sdev->core_rev = core_rev;
	sdev->core = pvrsgx_core_lookup(sdev->family, sdev->core_id, sdev->core_rev);
	KUNIT_ASSERT_NOT_NULL(test, sdev->core);

	sdev->kernel_mmu = pvrsgx_mmu_create(sdev);
	sdev->idle_mmu = pvrsgx_mmu_create(sdev);
	sdev->reset_mmu = pvrsgx_mmu_create(sdev);
	sdev->reset_page = alloc_page(GFP_KERNEL | __GFP_ZERO | __GFP_DMA32);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sdev->kernel_mmu);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sdev->idle_mmu);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, sdev->reset_mmu);
	KUNIT_ASSERT_NOT_NULL(test, sdev->reset_page);
	eq_pd(fx, sdev->kernel_mmu);
	eq_pd(fx, sdev->idle_mmu);
	eq_pd(fx, sdev->reset_mmu);

	pvrsgx_kobj_init(sdev);
	mutex_init(&sdev->pb_lock);

	file = eq_file(test);
	fw.data = (const u8 *)file;
	fw.size = sizeof(*file);
	sdev->fw.name = "equiv";
	KUNIT_ASSERT_EQ(test, ukfw_load(sdev, &fw), 0);
	KUNIT_ASSERT_EQ(test, fw_arenas(sdev), 0);

	eq_cur = fx;
	sdev->test_wr = eq_wr;
	return fx;
}

static void eq_fini(struct eq_fx *fx)
{
	struct pvrsgx_device *sdev = fx->sdev;

	sdev->test_wr = NULL;
	eq_cur = NULL;
	cancel_work_sync(&sdev->reap_work);
	pvrsgx_ccb_inval_reap(sdev, true);
	pvrsgx_pb_put(sdev->pb);
	sdev->pb = NULL;
	pvrsgx_kobj_fini(sdev);
	pvrsgx_fw_fini(sdev);
	__free_page(sdev->reset_page);
	pvrsgx_mmu_destroy(sdev->reset_mmu);
	pvrsgx_mmu_destroy(sdev->idle_mmu);
	pvrsgx_mmu_destroy(sdev->kernel_mmu);
	mutex_destroy(&sdev->pb_lock);
	mutex_destroy(&sdev->done_lock);
	mutex_destroy(&sdev->ccb_lock);
}

/* DEV_QUERY_HEAP_INFO_GET, through user memory as a client sees it. */
static void eq_heaps(struct eq_fx *fx)
{
	struct drm_powervr_sgx_dev_query_heap_info info = {};
	struct drm_powervr_sgx_heap heaps[16] = {};
	unsigned long user;
	u32 size = sizeof(info);

	user = kunit_vm_mmap(fx->test, NULL, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
			     MAP_ANONYMOUS | MAP_PRIVATE, 0);
	KUNIT_ASSERT_NE_MSG(fx->test, user, 0, "no user memory");
	info.heaps.stride = sizeof(heaps[0]);
	info.heaps.count = ARRAY_SIZE(heaps);
	info.heaps.array = user + 64;
	KUNIT_ASSERT_EQ(fx->test, copy_to_user((void __user *)user, &info, sizeof(info)), 0);
	KUNIT_ASSERT_EQ(fx->test, pvrsgx_query_heap_info(fx->sdev, (void __user *)user, &size), 0);
	KUNIT_ASSERT_EQ(fx->test, copy_from_user(&info, (void __user *)user, sizeof(info)), 0);
	KUNIT_ASSERT_EQ(fx->test, copy_from_user(heaps, (void __user *)(user + 64),
						 sizeof(heaps)), 0);
	eq_add(fx, "heaps size %u stride %u count %u", size, info.heaps.stride, info.heaps.count);
	for (u32 i = 0; i < min_t(u32, info.heaps.count, ARRAY_SIZE(heaps)); i++) {
		char name[16];

		snprintf(name, sizeof(name), "heap%u", i);
		eq_words(fx, name, &heaps[i], sizeof(heaps[i]), -1);
	}
}

static void equiv_test_sgx545(struct kunit *test)
{
	struct eq_fx *fx = eq_device(test, 545, 0x0001000e);
	struct pvrsgx_device *sdev = fx->sdev;
	struct pvrsgx_context rctx = {}, xctx = {};
	u32 data[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
	struct pvrsgx_vm *vm;

	/* The image built, the arenas mapped. */
	eq_arenas(fx);
	eq_mmu(fx, "kmmu", sdev->kernel_mmu);
	eq_check(fx, "init");

	/* The first start. */
	KUNIT_ASSERT_EQ(test, pvrsgx_fw_start(sdev), 0);
	eq_words(fx, "data", sdev->fw.data.cpu, sdev->fw.data.size, -1);
	eq_check(fx, "start");

	/* A command that asks for every invalidate. */
	sdev->fw.cache_control |= PVRSGX_CC_INVAL_BIF_PT | PVRSGX_CC_INVAL_BIF_PD |
				  PVRSGX_CC_INVAL_DATA;
	KUNIT_ASSERT_EQ(test, pvrsgx_ccb_send(sdev, PVRSGX_KCMD_PROCESS_QUEUES, data, NULL), 0);
	eq_words(fx, "data", sdev->fw.data.cpu, 0x3200, -1);
	eq_check(fx, "send");

	/* A hang: the restart finds a BIF fault left over. */
	fx->regs[SGX_CR_BIF_INT_STAT / 4] = sdev->cr->bif_int_stat_fault_req;
	fx->regs[SGX_CR_BIF_FAULT / 4] = 0x3e001000;
	fx->faults = 1;
	sdev->fw_running = false;
	KUNIT_ASSERT_EQ(test, pvrsgx_fw_start(sdev), 0);
	eq_arenas(fx);
	eq_mmu(fx, "rmmu", sdev->reset_mmu);
	eq_check(fx, "recover");

	/* Runtime suspend. */
	pvrsgx_fw_stop(sdev);
	eq_words(fx, "data", sdev->fw.data.cpu, 0x3200, -1);
	eq_check(fx, "stop");

	/* Runtime resume, from gated clocks. */
	KUNIT_ASSERT_EQ(test, pvrsgx_fw_start(sdev), 0);
	eq_words(fx, "data", sdev->fw.data.cpu, sdev->fw.data.size, -1);
	eq_check(fx, "resume");

	/* A VM with a render and a transfer context. */
	vm = pvrsgx_vm_create(sdev);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, vm);
	eq_pd(fx, vm->mmu);
	rctx.sdev = sdev;
	rctx.vm = vm;
	xctx.sdev = sdev;
	xctx.vm = vm;
	KUNIT_ASSERT_EQ(test, pvrsgx_render_ctx_init(sdev, &rctx, 0x40), 0);
	KUNIT_ASSERT_EQ(test, pvrsgx_transfer_ctx_init(sdev, &xctx), 0);
	eq_add(fx, "rctx hw %08x ccb %08x ccbctl %08x", kobj_va(rctx.render->hwctx),
	       kobj_va(rctx.render->ccb), kobj_va(rctx.render->ccbctl));
	eq_add(fx, "rctx scratch %08x sync %08x", kobj_va(rctx.render->scratch),
	       kobj_va(rctx.render->sync3d));
	eq_words(fx, "rhw", kobj_cpu(rctx.render->hwctx), sizeof(struct pvrsgx_fw_hw_render_ctx),
		 offsetof(struct pvrsgx_fw_hw_render_ctx, pid) / 4);
	eq_add(fx, "pb pages %x base %08x buf %08x fl %08x", rctx.render->pb->pages,
	       rctx.render->pb->base, kobj_va(rctx.render->pb->buf),
	       kobj_va(rctx.render->pb->freelist));
	eq_add(fx, "pb desc %08x blocks %08x", kobj_va(rctx.render->pb->desc),
	       kobj_va(rctx.render->pb->blocks));
	eq_add(fx, "xctx hw %08x", kobj_va(xctx.transfer->hwctx));
	eq_words(fx, "xhw", kobj_cpu(xctx.transfer->hwctx),
		 sizeof(struct pvrsgx_fw_hw_transfer_ctx),
		 offsetof(struct pvrsgx_fw_hw_transfer_ctx, pid) / 4);
	eq_mmu(fx, "vmmu", vm->mmu);
	eq_mmu(fx, "kmmu", sdev->kernel_mmu);
	eq_check(fx, "ctx");

	eq_heaps(fx);
	eq_check(fx, "heaps");

	pvrsgx_transfer_ctx_fini(sdev, &xctx);
	pvrsgx_render_ctx_fini(sdev, &rctx);
	pvrsgx_vm_put(vm);
	eq_check(fx, "teardown");
	eq_fini(fx);
}

static struct kunit_case pvrsgx_equiv_test_cases[] = {
	KUNIT_CASE(equiv_test_sgx545),
	{}
};

static struct kunit_suite pvrsgx_equiv_test_suite = {
	.name = "powervr_sgx_equiv",
	.test_cases = pvrsgx_equiv_test_cases,
};

kunit_test_suite(pvrsgx_equiv_test_suite);
