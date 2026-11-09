// SPDX-License-Identifier: MIT
/* Copyright © 2026 Joseph Telaak */

/*
 * Host test of the stream decoder: streams written with the generated userspace
 * writer must decode to the same values, and every rule of docs/UAPI.md section
 * 3 must reject the stream that breaks it. Built and run by docs/uapi/check.sh.
 */

#include <stdio.h>
#include <stdlib.h>

#include "pvrsgx_stream_defs.h"
#include "fixture_stream_defs.h"
#include "sgx_kmd_stream_pack.h"

#include <linux/array_size.h>

static int failures;

#define EXPECT(cond, what)                                                  \
	do {                                                                \
		if (!(cond)) {                                              \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, what); \
			failures++;                                         \
		}                                                           \
	} while (0)

/* One VM: [0x2e000000, +0x2000) PDS pixel; 4 MiB at 0x3e000000 and 0x7e000000, general. */
static bool mapped(const void *priv, u64 addr, u64 size)
{
	static const u64 maps[][2] = {
		{ 0x2e000000, 0x2000 }, { 0x3e000000, 0x400000 }, { 0x7e000000, 0x400000 },
	};

	(void)priv;
	for (size_t i = 0; i < ARRAY_SIZE(maps); i++) {
		if (addr >= maps[i][0] && addr + size <= maps[i][0] + maps[i][1])
			return true;
	}
	return false;
}

static bool has_quirk(const void *priv, u32 quirk)
{
	return priv && quirk == 1234;
}

static struct pvrsgx_stream_ctx ctx(void)
{
	struct pvrsgx_stream_ctx c = {
		.heaps[PVRSGX_STREAM_HEAP_PDS_PIXEL] = { 0x2e000000, 0x2000000 },
		.limits[PVRSGX_STREAM_LIMIT_TILES_X] = 63,
		.limits[PVRSGX_STREAM_LIMIT_TILES_Y] = 127,
		.rt_pad_width = 736,
		.rt_pad_height = 480,
		.mapped = mapped,
		.has_quirk = has_quirk,
	};
	return c;
}

static const struct SGX_KMD_STREAM_CMD_RENDER good_render = {
	.depth_addr = 0x3e100000,
	.stencil_addr = 0x3e300000,
	.pixel_event_pds_addr = 0x2e000020,
	.bg_pds_secondary_addr = 0x2e000000,
	.bg_pds_primary_addr = 0x2e000060,
	.pixel_event_pds_rows = 5,
	.bg_pds_secondary_rows = 1,
	.bg_pds_primary_rows = 3,
	.zls_control = 0x00080001,
	.flags_3d = 0x180,
	.bg_depth = 0x3f800000,
	.depth_bias_units = -2,
	.depth_bias_factor = 0x3f000000,
};

static int render(const struct SGX_KMD_STREAM_CMD_RENDER *v, const struct pvrsgx_stream_ctx *c,
		  struct pvrsgx_stream_render *out)
{
	u8 buf[SGX_KMD_STREAM_CMD_RENDER_length];
	u32 len = SGX_KMD_STREAM_CMD_RENDER_pack(buf, v);
	int err = pvrsgx_stream_process(&pvrsgx_stream_render_defs, c, buf, len, out);

	if (!err)
		err = pvrsgx_stream_check_mapped(&pvrsgx_stream_render_defs, c, out);
	if (!err)
		err = pvrsgx_check_render(out);
	return err;
}

static void test_render(void)
{
	struct pvrsgx_stream_ctx c = ctx();
	struct pvrsgx_stream_render out;
	struct SGX_KMD_STREAM_CMD_RENDER v;
	u8 buf[SGX_KMD_STREAM_CMD_RENDER_length + 16] = { 0 };
	u32 len;

	EXPECT(render(&good_render, &c, &out) == 0, "a good render stream decodes");
	EXPECT(out.depth_addr == good_render.depth_addr && out.stencil_addr == good_render.stencil_addr &&
	       out.pixel_event_pds_addr == good_render.pixel_event_pds_addr &&
	       out.pixel_event_pds_rows == 5 && out.zls_control == 0x00080001 &&
	       out.flags_3d == 0x180 && out.bg_depth == 0x3f800000 &&
	       (s32)out.depth_bias_units == -2 && out.depth_bias_factor == 0x3f000000,
	       "render fields round-trip");

	/* Framing. */
	len = SGX_KMD_STREAM_CMD_RENDER_pack(buf, &good_render);
	EXPECT(len == pvrsgx_stream_render_defs.main_len, "writer and decoder agree on the length");
	buf[4] = 1;
	EXPECT(pvrsgx_stream_process(&pvrsgx_stream_render_defs, &c, buf, len, &out),
	       "header word 1 must be zero");
	buf[4] = 0;
	buf[0] = len + 4;
	EXPECT(pvrsgx_stream_process(&pvrsgx_stream_render_defs, &c, buf, len + 4, &out),
	       "a longer main stream is refused (it is frozen)");
	buf[0] = len;
	EXPECT(pvrsgx_stream_process(&pvrsgx_stream_render_defs, &c, buf, len - 4, &out),
	       "a truncated stream is refused");
	EXPECT(pvrsgx_stream_process(&pvrsgx_stream_render_defs, &c, buf, len + 2, &out),
	       "trailing bytes that are not an extension header are refused");
	EXPECT(pvrsgx_stream_process(&pvrsgx_stream_render_defs, &c, buf, len + 4, &out) == 0,
	       "an empty extension header of a known type is accepted");
	EXPECT(pvrsgx_stream_process(&pvrsgx_stream_render_defs, &c, buf, len + 8, &out),
	       "bytes after the last extension stream are refused");
	buf[len + 3] = 1 << 5; /* type 1 */
	EXPECT(pvrsgx_stream_process(&pvrsgx_stream_render_defs, &c, buf, len + 4, &out),
	       "an unknown extension header type is refused");
	buf[len + 3] = 0;
	buf[len] = 1;
	EXPECT(pvrsgx_stream_process(&pvrsgx_stream_render_defs, &c, buf, len + 4, &out),
	       "an extension bit outside the valid mask is refused");
	buf[len] = 0;
	buf[len + 3] = 1 << 4; /* continuation */
	EXPECT(pvrsgx_stream_process(&pvrsgx_stream_render_defs, &c, buf, len + 4, &out),
	       "a continuation with no next header is refused");
	EXPECT(pvrsgx_stream_process(&pvrsgx_stream_render_defs, &c, buf, len + 8, &out),
	       "the same extension header type twice is refused");
	EXPECT(pvrsgx_stream_process(&pvrsgx_stream_render_defs, &c, buf, 4097, &out),
	       "a stream over 4 KiB is refused");
	EXPECT(pvrsgx_stream_process(&pvrsgx_stream_render_defs, &c, buf, 0, &out),
	       "an empty stream is refused");

	/* Per field. */
	v = good_render;
	v.pixel_event_pds_rows = 0;
	EXPECT(render(&v, &c, &out), "rows 0");
	v.pixel_event_pds_rows = 64;
	EXPECT(render(&v, &c, &out), "rows 64");
	v = good_render;
	v.bg_pds_primary_rows = 63;
	EXPECT(render(&v, &c, &out) == 0, "rows 63");
	v = good_render;
	v.bg_pds_primary_addr = 0x30000000;
	EXPECT(render(&v, &c, &out), "PDS program outside PDS_PIXEL");
	v.bg_pds_primary_addr = 0x2e000068;
	EXPECT(render(&v, &c, &out), "PDS program not 16-byte aligned");
	v.bg_pds_primary_addr = 0;
	EXPECT(render(&v, &c, &out), "PDS program 0");
	v.bg_pds_primary_addr = 0x12e000060ULL;
	EXPECT(render(&v, &c, &out), "address above 4 GiB");
	v = good_render;
	v.zls_control |= 1 << 3;
	EXPECT(render(&v, &c, &out), "zls_control tile-count bits belong to the kernel");
	v = good_render;
	v.flags_3d = 0x580;
	EXPECT(render(&v, &c, &out), "flags_3d bit 10 belongs to the kernel");

	/* Mapped extents. */
	v = good_render;
	v.pixel_event_pds_addr = 0x2e001fc0;
	EXPECT(render(&v, &c, &out), "pixel event rows run past the mapping");
	v = good_render;
	v.depth_addr = 0x3e300000;
	v.stencil_addr = 0x3e100000;
	EXPECT(render(&v, &c, &out), "depth buffer runs past the mapping");

	/* Cross-field. */
	v = good_render;
	v.depth_addr = 0;
	EXPECT(render(&v, &c, &out), "zls_control set with no depth buffer");
	v = good_render;
	v.stencil_addr = 0;
	v.zls_control = 0x00080000;
	EXPECT(render(&v, &c, &out), "flags_3d bit 8 set with no stencil buffer");
	v.flags_3d = 0x80;
	EXPECT(render(&v, &c, &out) == 0, "depth only");
	v = good_render;
	v.stencil_addr = 0x7e100000;
	EXPECT(render(&v, &c, &out), "depth and stencil 1 GiB or more apart");
	v = good_render;
	v.zls_control = 0;
	v.flags_3d = 0;
	v.depth_addr = v.stencil_addr = 0;
	EXPECT(render(&v, &c, &out) == 0, "no depth or stencil");
}

static void test_ta_kick(void)
{
	struct pvrsgx_stream_ctx c = ctx();
	struct pvrsgx_stream_ta_kick out;
	struct SGX_KMD_STREAM_CMD_TA_KICK v = { .vdm_base = 0x3e018000, .ta_flags2 = 0x2c80000 };
	u8 buf[SGX_KMD_STREAM_CMD_TA_KICK_length];
	u32 len = SGX_KMD_STREAM_CMD_TA_KICK_pack(buf, &v);

	EXPECT(len == 20, "TA kick stream is 20 bytes");
	EXPECT(!pvrsgx_stream_process(&pvrsgx_stream_ta_kick_defs, &c, buf, len, &out) &&
	       !pvrsgx_stream_check_mapped(&pvrsgx_stream_ta_kick_defs, &c, &out) &&
	       out.vdm_base == 0x3e018000 && out.ta_flags2 == 0x2c80000, "a good TA kick decodes");
	v.ta_flags2 = 1;
	SGX_KMD_STREAM_CMD_TA_KICK_pack(buf, &v);
	EXPECT(pvrsgx_stream_process(&pvrsgx_stream_ta_kick_defs, &c, buf, len, &out),
	       "ta_flags2 bit 0");
	v.ta_flags2 = 0;
	v.vdm_base = 0x3e018002;
	SGX_KMD_STREAM_CMD_TA_KICK_pack(buf, &v);
	EXPECT(pvrsgx_stream_process(&pvrsgx_stream_ta_kick_defs, &c, buf, len, &out),
	       "vdm_base not 4-byte aligned");
	v.vdm_base = 0x13e018000ULL;
	SGX_KMD_STREAM_CMD_TA_KICK_pack(buf, &v);
	EXPECT(pvrsgx_stream_process(&pvrsgx_stream_ta_kick_defs, &c, buf, len, &out),
	       "vdm_base above 4 GiB");
	v.vdm_base = 0x3f000000;
	SGX_KMD_STREAM_CMD_TA_KICK_pack(buf, &v);
	EXPECT(!pvrsgx_stream_process(&pvrsgx_stream_ta_kick_defs, &c, buf, len, &out) &&
	       pvrsgx_stream_check_mapped(&pvrsgx_stream_ta_kick_defs, &c, &out),
	       "vdm_base not mapped");
}

static void test_transfer(void)
{
	struct pvrsgx_stream_ctx c = ctx();
	struct pvrsgx_stream_transfer out;
	struct SGX_KMD_STREAM_CMD_TRANSFER v = {
		.bg_block_addr = 0x3e000200,
		.pixel_event_pds_addr = 0x2e000020,
		.pixel_event_pds_rows = 5,
		.first_tile_x = 0, .first_tile_y = 0,
		.last_tile_x = 22, .last_tile_y = 29,
		.bg_depth = 0x3f800000,
	};
	u8 buf[SGX_KMD_STREAM_CMD_TRANSFER_length];
	u32 len;

	c.rt_pad_width = c.rt_pad_height = 0;
	len = SGX_KMD_STREAM_CMD_TRANSFER_pack(buf, &v);
	EXPECT(!pvrsgx_stream_process(&pvrsgx_stream_transfer_defs, &c, buf, len, &out) &&
	       !pvrsgx_stream_check_mapped(&pvrsgx_stream_transfer_defs, &c, &out) &&
	       !pvrsgx_check_transfer(&out) && out.last_tile_y == 29, "a good transfer decodes");
	v.first_tile_x = 23;
	SGX_KMD_STREAM_CMD_TRANSFER_pack(buf, &v);
	EXPECT(!pvrsgx_stream_process(&pvrsgx_stream_transfer_defs, &c, buf, len, &out) &&
	       pvrsgx_check_transfer(&out), "first tile after last");
	v.first_tile_x = 0;
	v.last_tile_x = 64;
	SGX_KMD_STREAM_CMD_TRANSFER_pack(buf, &v);
	EXPECT(pvrsgx_stream_process(&pvrsgx_stream_transfer_defs, &c, buf, len, &out),
	       "tile past the device limit");
	v.last_tile_x = 22;
	v.bg_block_addr = 0x3e3fff00;
	SGX_KMD_STREAM_CMD_TRANSFER_pack(buf, &v);
	EXPECT(!pvrsgx_stream_process(&pvrsgx_stream_transfer_defs, &c, buf, len, &out) &&
	       pvrsgx_stream_check_mapped(&pvrsgx_stream_transfer_defs, &c, &out),
	       "background block runs past the mapping");
}

/* Writes a little-endian word. */
static void put32(u8 *p, u32 v)
{
	memcpy(p, &v, 4);
}

static void test_extensions(void)
{
	struct pvrsgx_stream_ctx c = ctx();
	struct fixture_stream_demo out;
	const struct pvrsgx_stream_cmd_defs *d = &fixture_stream_demo_defs;
	u32 musthave[2] = { FIXTURE_STREAM_EXTHDR_DEMO0_EXTRA, 0 };
	u8 s[64] = { 0 };
	u64 addr = 0x3e000040;

	put32(s, 12);
	put32(s + 8, 2);
	EXPECT(d->main_len == 12, "fixture main stream is 12 bytes");
	EXPECT(!pvrsgx_stream_process(d, &c, s, 12, &out) && out.extra_addr == 0 &&
	       !pvrsgx_stream_check_mapped(d, &c, &out), "no extension: its fields are zero");

	/* ext0 EXTRA: header at 12, extra_bits at 16, a zero gap at 20, extra_addr at 24. */
	put32(s + 12, FIXTURE_STREAM_EXTHDR_DEMO0_EXTRA);
	put32(s + 16, 0x30);
	memcpy(s + 24, &addr, 8);
	EXPECT(!pvrsgx_stream_process(d, &c, s, 32, &out) && out.extra_bits == 0x30 &&
	       out.extra_addr == addr && !pvrsgx_stream_check_mapped(d, &c, &out),
	       "extension stream decodes after an aligned gap");
	s[21] = 1;
	EXPECT(pvrsgx_stream_process(d, &c, s, 32, &out), "non-zero gap byte");
	s[21] = 0;
	addr = 0x3f000000;
	memcpy(s + 24, &addr, 8);
	EXPECT(!pvrsgx_stream_process(d, &c, s, 32, &out) &&
	       pvrsgx_stream_check_mapped(d, &c, &out), "extension address not mapped");
	addr = 0x3e000040;
	memcpy(s + 24, &addr, 8);

	/* must-have: the device requires EXTRA. */
	c.musthave = musthave;
	EXPECT(pvrsgx_stream_process(d, &c, s, 12, &out), "a must-have extension is missing");
	EXPECT(!pvrsgx_stream_process(d, &c, s, 32, &out), "a must-have extension is present");
	put32(s + 12, 0);
	EXPECT(pvrsgx_stream_process(d, &c, s, 16, &out),
	       "an extension header without the must-have bit");
	put32(s + 12, FIXTURE_STREAM_EXTHDR_DEMO0_EXTRA);
	c.musthave = NULL;

	/* ext0 QUIRKY needs quirk 1234. */
	put32(s + 12, FIXTURE_STREAM_EXTHDR_DEMO0_QUIRKY);
	put32(s + 16, 7);
	EXPECT(pvrsgx_stream_process(d, &c, s, 20, &out), "quirk extension without the quirk");
	c.priv = &c;
	EXPECT(!pvrsgx_stream_process(d, &c, s, 20, &out) && out.quirky == 7,
	       "quirk extension with the quirk");

	/* ext0 then ext1 MORE, through a continuation. */
	put32(s + 12, FIXTURE_STREAM_EXTHDR_DEMO0_EXTRA | PVRSGX_STREAM_EXTHDR_CONTINUATION);
	put32(s + 16, 0x30);
	put32(s + 32, (1U << 29) | FIXTURE_STREAM_EXTHDR_DEMO1_MORE);
	put32(s + 36, 63);
	EXPECT(!pvrsgx_stream_process(d, &c, s, 40, &out) && out.more == 63, "two extension headers");
	put32(s + 36, 64);
	EXPECT(pvrsgx_stream_process(d, &c, s, 40, &out), "extension field past its limit");
	put32(s + 32, FIXTURE_STREAM_EXTHDR_DEMO0_EXTRA);
	EXPECT(pvrsgx_stream_process(d, &c, s, 40, &out), "extension header type 0 twice");
}

int main(void)
{
	test_render();
	test_ta_kick();
	test_transfer();
	test_extensions();
	if (failures) {
		fprintf(stderr, "%d stream test(s) failed\n", failures);
		return 1;
	}
	return 0;
}
