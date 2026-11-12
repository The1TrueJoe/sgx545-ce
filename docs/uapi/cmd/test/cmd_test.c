// SPDX-License-Identifier: MIT
/* Copyright © 2026 Joseph Telaak */

/*
 * Host test of the command copy and checks (docs/uapi/rev3-commands.md
 * sections 3 and 4): every rule must reject the command that breaks it, and a
 * good command must come through with its values. Built and run by
 * docs/uapi/check.sh.
 */

#include <stdio.h>
#include <string.h>

#include <linux/bits.h>
#include <linux/errno.h>

#include "pvrsgx_cmd_checks.h"

const void *shim_fault;
void (*shim_after_get_user)(void);
static int failures;

/* A second thread shrinking the command between the kernel's two reads. */
static struct drm_powervr_sgx_cmd_render *race_target;
static void race(void)
{
	race_target->head.size = 24;
}

#define EXPECT_EQ(got, want, what)                                                  \
	do {                                                                        \
		long long g_ = (got), w_ = (want);                                  \
		if (g_ != w_) {                                                     \
			fprintf(stderr, "FAIL %s:%d: %s: got %lld, want %lld\n",   \
				__FILE__, __LINE__, what, g_, w_);                 \
			failures++;                                                 \
		}                                                                   \
	} while (0)

#define UPTR(p) ((u64)(uintptr_t)(p))

/* One VM: PDS pixel heap [0x2e000000, +32 MiB), 0x2000 of it mapped; 4 MiB of general. */
static bool mapped(const void *priv, u64 addr, u64 size)
{
	(void)priv;
	return (addr >= 0x2e000000 && addr + size <= 0x2e002000) ||
	       (addr >= 0x3e000000 && addr + size <= 0x3e400000);
}

static struct pvrsgx_cmd_ctx ctx(void)
{
	struct pvrsgx_cmd_ctx c = {
		.heaps[PVRSGX_CMD_HEAP_PDS_PIXEL] = { 0x2e000000, 0x2000000 },
		.limits[PVRSGX_CMD_LIMIT_TILES_X] = 22,
		.limits[PVRSGX_CMD_LIMIT_TILES_Y] = 29,
		.rt_pad_width = 736,
		.rt_pad_height = 480,
		.mapped = mapped,
	};
	return c;
}

static const struct pvrsgx_cmd_core *core545 = &pvrsgx_cmd_core_sgx545;

static struct drm_powervr_sgx_cmd_render good_render(void)
{
	struct drm_powervr_sgx_cmd_render r = {
		.head.size = sizeof(r),
		.depth_addr = 0x3e100000,
		.pixel_event_pds_addr = 0x2e000100,
		.bg_pds_secondary_addr = 0x2e000200,
		.bg_pds_primary_addr = 0x2e000300,
		.pixel_event_pds_rows = 5,
		.bg_pds_secondary_rows = 1,
		.bg_pds_primary_rows = 2,
		.bg_depth_f32 = 0x3f800000,
		.depth_bias_units = -3,
	};
	return r;
}

/* copy, then every check, as the kernel's submit runs them */
static int submit_render(const struct pvrsgx_cmd_core *core, const struct pvrsgx_cmd_def *def,
			 const void *cmd, struct pvrsgx_cmd_render_all *all)
{
	struct pvrsgx_cmd_ctx c = ctx();
	u64 present;
	int err = pvrsgx_cmd_copy(def, UPTR(cmd), all, &present);

	if (!err)
		err = pvrsgx_cmd_check(core, def, &c, all, present);
	if (!err)
		err = pvrsgx_cmd_check_mapped(core, def, &c, all, present);
	if (!err)
		err = pvrsgx_check_render(core, all);
	return err;
}

static int render(const void *cmd)
{
	struct pvrsgx_cmd_render_all all;

	return submit_render(core545, core545->cmds[PVRSGX_CMD_RENDER], cmd, &all);
}

static void test_render_copy(void)
{
	const struct pvrsgx_cmd_def *def = core545->cmds[PVRSGX_CMD_RENDER];
	struct drm_powervr_sgx_cmd_render r = good_render();
	struct drm_powervr_sgx_ext_sgx545_render e = {
		.base.name = DRM_POWERVR_SGX_EXT_SGX545_RENDER,
		.zls_control = 0x00080001,	/* store, b19 */
		.flags_3d = 0x80,
	};
	struct pvrsgx_cmd_render_all all;
	struct { struct drm_powervr_sgx_cmd_render r; u8 tail[16]; } big = { .r = good_render() };
	u64 present;

	EXPECT_EQ(render(&r), 0, "a good render");

	r.head.extensions = UPTR(&e);
	r.stencil_addr = 0x3e200000;
	EXPECT_EQ(submit_render(core545, def, &r, &all), 0, "a render with the SGX545 extension");
	EXPECT_EQ(all.base.depth_bias_units, -3, "values come through");
	EXPECT_EQ(all.sgx545_render.zls_control, 0x00080001, "the extension comes through");
	r = good_render();

	/* sizes */
	r.head.size = 15;
	EXPECT_EQ(render(&r), -EINVAL, "size below the head");
	r.head.size = PVRSGX_CMD_MAX_SIZE + 8;
	EXPECT_EQ(render(&r), -EINVAL, "size above 4096");
	r.head.size = offsetof(struct drm_powervr_sgx_cmd_render, depth_bias_factor_f32);
	r.depth_bias_factor_f32 = 0x12345678;	/* past what this caller "knows" */
	EXPECT_EQ(pvrsgx_cmd_copy(def, UPTR(&r), &all, &present), 0, "an older, shorter struct");
	EXPECT_EQ(all.base.depth_bias_factor_f32, 0, "its missing tail reads as zero");
	r = good_render();

	big.r.head.size = sizeof(big);
	EXPECT_EQ(render(&big), 0, "a longer struct with a zero tail");
	big.tail[9] = 1;
	EXPECT_EQ(render(&big), -E2BIG, "a longer struct with a non-zero tail");

	r.head._padding_4 = 1;
	EXPECT_EQ(render(&r), -EINVAL, "head padding");
	r = good_render();

	EXPECT_EQ(render(NULL), -EINVAL, "no command");
	shim_fault = &r;
	EXPECT_EQ(render(&r), -EFAULT, "an unreadable command");
	shim_fault = NULL;

	race_target = &r;
	shim_after_get_user = race;
	EXPECT_EQ(render(&r), -EINVAL, "the size changed between the two reads");
	shim_after_get_user = NULL;
}

static void test_extensions(void)
{
	const struct pvrsgx_cmd_def *def = core545->cmds[PVRSGX_CMD_RENDER];
	struct drm_powervr_sgx_cmd_render r = good_render();
	struct drm_powervr_sgx_ext_sgx545_render e = { .base.name = DRM_POWERVR_SGX_EXT_SGX545_RENDER };
	struct drm_powervr_sgx_ext_sgx545_render e2 = e;
	struct drm_powervr_sgx_ext_sgx545_ta_kick k = { .base.name = DRM_POWERVR_SGX_EXT_SGX545_TA_KICK };
	struct drm_powervr_sgx_ext chain[PVRSGX_CMD_MAX_EXTS + 1];
	struct pvrsgx_cmd_def no_exts = *def;
	struct pvrsgx_cmd_render_all all;

	r.head.extensions = UPTR(&e);
	EXPECT_EQ(render(&r), 0, "one extension");

	e.base.next = UPTR(&e2);
	EXPECT_EQ(render(&r), -EINVAL, "a name twice");
	e.base.next = 0;

	e.base._padding_4 = 1;
	EXPECT_EQ(render(&r), -EINVAL, "extension head padding");
	e.base._padding_4 = 0;

	r.head.extensions = UPTR(&k);
	EXPECT_EQ(render(&r), -EINVAL, "another command's extension");

	chain[0] = (struct drm_powervr_sgx_ext){ .name = 63 };
	r.head.extensions = UPTR(&chain[0]);
	EXPECT_EQ(render(&r), -EINVAL, "an unknown name");
	chain[0].name = 0;
	EXPECT_EQ(render(&r), -EINVAL, "name 0");

	/* 17 links: the cap trips before the repeated name */
	for (unsigned i = 0; i <= PVRSGX_CMD_MAX_EXTS; i++)
		chain[i] = (struct drm_powervr_sgx_ext){ .name = 63, .next = UPTR(&chain[i + 1]) };
	chain[PVRSGX_CMD_MAX_EXTS].next = 0;
	EXPECT_EQ(render(&r), -EINVAL, "a chain past the cap");

	r.head.extensions = UPTR(&e);
	shim_fault = &e;
	EXPECT_EQ(render(&r), -EFAULT, "an unreadable extension");
	shim_fault = NULL;

	/* a core that has the render command but not this extension */
	no_exts.exts = NULL;
	no_exts.nr_exts = 0;
	EXPECT_EQ(submit_render(core545, &no_exts, &r, &all), -EOPNOTSUPP,
		  "an extension this core doesn't accept");

	EXPECT_EQ(pvrsgx_cmd_ext_mask(def), BIT_ULL(DRM_POWERVR_SGX_EXT_SGX545_RENDER),
		  "render's extension mask on the SGX545");
	EXPECT_EQ(pvrsgx_cmd_ext_mask(core545->cmds[PVRSGX_CMD_TRANSFER]), 0,
		  "transfer takes no extension");
}

static void test_render_fields(void)
{
	struct drm_powervr_sgx_cmd_render r = good_render();
	struct drm_powervr_sgx_ext_sgx545_render e = { .base.name = DRM_POWERVR_SGX_EXT_SGX545_RENDER };
	struct pvrsgx_cmd_core narrow = *core545;
	struct pvrsgx_cmd_render_all all;

	r.pixel_event_pds_rows = 0;
	EXPECT_EQ(render(&r), -EINVAL, "rows 0");
	r.pixel_event_pds_rows = 64;
	EXPECT_EQ(render(&r), -EINVAL, "rows 64");
	r = good_render();

	r.bg_pds_primary_addr = 0x2e000308;
	EXPECT_EQ(render(&r), -EINVAL, "a PDS address not 16-aligned");
	r.bg_pds_primary_addr = 0x3e000000;
	EXPECT_EQ(render(&r), -EINVAL, "a PDS address outside the PDS pixel heap");
	r.bg_pds_primary_addr = 0;
	EXPECT_EQ(render(&r), -EINVAL, "a required address left 0");
	r = good_render();

	r.pixel_event_pds_addr = 0x2e001ff0;	/* 5 rows of 16 run past the mapping */
	EXPECT_EQ(render(&r), -EINVAL, "a pixel event program not mapped for its rows");
	r = good_render();

	r.depth_addr = 0x1003e100000ull;
	EXPECT_EQ(render(&r), -EINVAL, "an address above 2^32");
	r = good_render();
	narrow.va_bits = 28;
	EXPECT_EQ(submit_render(&narrow, core545->cmds[PVRSGX_CMD_RENDER], &r, &all), -EINVAL,
		  "an address above 2^va_bits on a 28-bit core");

	r.head.extensions = UPTR(&e);
	e.zls_control = 0x8;	/* bit 3: the kernel's tile count */
	EXPECT_EQ(render(&r), -EINVAL, "a ZLS control bit the client may not set");
	e.zls_control = 0x1;	/* uses the stencil */
	EXPECT_EQ(render(&r), -EINVAL, "ZLS stencil bits without a stencil address");
	e.zls_control = 0;
	e.flags_3d = 0x100;
	EXPECT_EQ(render(&r), -EINVAL, "the 3D stencil flag without a stencil address");
	e.flags_3d = 0;
	r.depth_addr = 0;
	e.zls_control = 0x00080000;
	EXPECT_EQ(render(&r), -EINVAL, "ZLS without a depth address");
}

static void test_ta_kick(void)
{
	const struct pvrsgx_cmd_def *def = core545->cmds[PVRSGX_CMD_TA_KICK];
	struct drm_powervr_sgx_ext_sgx545_ta_kick e = {
		.base.name = DRM_POWERVR_SGX_EXT_SGX545_TA_KICK,
		.ta_flags2 = 0x02400000,
	};
	struct drm_powervr_sgx_cmd_ta_kick k = {
		.head = { .size = sizeof(k), .extensions = UPTR(&e) },
		.vdm_base = 0x3e000040,
	};
	struct pvrsgx_cmd_ta_kick_all all;
	struct pvrsgx_cmd_ctx c = ctx();
	u64 present;

	EXPECT_EQ(pvrsgx_cmd_take_elem(def, &k, sizeof(k), &all, &present), 0, "a kick element");
	EXPECT_EQ(pvrsgx_cmd_check(core545, def, &c, &all, present), 0, "a kick's checks");
	EXPECT_EQ(all.sgx545_ta_kick.ta_flags2, 0x02400000, "its extension comes through");
	EXPECT_EQ(pvrsgx_cmd_take_elem(def, &k, sizeof(k) + 8, &all, &present), -EINVAL,
		  "head.size differs from the stride");

	e._padding_14 = 1;
	pvrsgx_cmd_take_elem(def, &k, sizeof(k), &all, &present);
	EXPECT_EQ(pvrsgx_cmd_check(core545, def, &c, &all, present), -EINVAL, "extension padding");
	e._padding_14 = 0;
	e.ta_flags2 = 0x1;
	pvrsgx_cmd_take_elem(def, &k, sizeof(k), &all, &present);
	EXPECT_EQ(pvrsgx_cmd_check(core545, def, &c, &all, present), -EINVAL, "a TA flag not allowed");

	k.head.extensions = 0;
	k.vdm_base = 0x3e000042;
	pvrsgx_cmd_take_elem(def, &k, sizeof(k), &all, &present);
	EXPECT_EQ(pvrsgx_cmd_check(core545, def, &c, &all, present), -EINVAL, "a VDM base not 4-aligned");
}

static void test_transfer(void)
{
	const struct pvrsgx_cmd_def *def = core545->cmds[PVRSGX_CMD_TRANSFER];
	struct drm_powervr_sgx_cmd_transfer t = {
		.head.size = sizeof(t),
		.bg_block_addr = 0x3e000200,
		.pixel_event_pds_addr = 0x2e000100,
		.pixel_event_pds_rows = 5,
		.last_tile_x = 22,
		.last_tile_y = 29,
	};
	struct pvrsgx_cmd_transfer_all all;
	struct pvrsgx_cmd_ctx c = ctx();
	u64 present;

	EXPECT_EQ(pvrsgx_cmd_copy(def, 0, &all, &present), -EINVAL, "a transfer needs its command");
	EXPECT_EQ(pvrsgx_cmd_copy(def, UPTR(&t), &all, &present), 0, "a transfer");
	EXPECT_EQ(pvrsgx_cmd_check(core545, def, &c, &all, present), 0, "a transfer's checks");
	EXPECT_EQ(pvrsgx_cmd_check_mapped(core545, def, &c, &all, present), 0, "its extents");
	EXPECT_EQ(pvrsgx_check_transfer(&all), 0, "its tile range");

	t.last_tile_x = 23;
	pvrsgx_cmd_copy(def, UPTR(&t), &all, &present);
	EXPECT_EQ(pvrsgx_cmd_check(core545, def, &c, &all, present), -EINVAL, "a tile past the limit");
	t.last_tile_x = 22;
	t.first_tile_y = 29;
	t.last_tile_y = 3;
	pvrsgx_cmd_copy(def, UPTR(&t), &all, &present);
	EXPECT_EQ(pvrsgx_check_transfer(&all), -EINVAL, "first tile after last");
}

static void test_sgx530(void)
{
	const struct pvrsgx_cmd_core *c = &pvrsgx_cmd_core_sgx530;
	struct drm_powervr_sgx_cmd_render r = good_render();
	struct pvrsgx_cmd_render_all all;
	u64 present;

	EXPECT_EQ(c->va_bits, 28, "the SGX530's address bits");
	for (unsigned t = 0; t < PVRSGX_CMD_COUNT; t++)
		EXPECT_EQ(c->cmds[t]->supported, 0, "no SGX530 command until its values are known");
	EXPECT_EQ(pvrsgx_cmd_copy(c->cmds[PVRSGX_CMD_RENDER], UPTR(&r), &all, &present), -EOPNOTSUPP,
		  "an SGX530 render");
	EXPECT_EQ(pvrsgx_cmd_ext_mask(c->cmds[PVRSGX_CMD_RENDER]), 0, "no SGX530 extension");
}

int main(void)
{
	test_render_copy();
	test_extensions();
	test_render_fields();
	test_ta_kick();
	test_transfer();
	test_sgx530();
	if (failures) {
		fprintf(stderr, "cmd_test: %d failures\n", failures);
		return 1;
	}
	printf("cmd_test: all passed\n");
	return 0;
}
