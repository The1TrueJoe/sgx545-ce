/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */

/*
 * Stream decoder of the powervr_sgx uAPI: the reference for the kernel driver.
 * Same framing as the upstream PowerVR driver's pvr_stream.c, stricter (no
 * unchecked bytes), plus a validator per field. docs/UAPI.md section 3 is the
 * specification; the tables come from gen_stream.py.
 */

#ifndef POWERVR_SGX_STREAM_H
#define POWERVR_SGX_STREAM_H

#include <linux/bits.h>
#include <linux/types.h>

#define POWERVR_SGX_STREAM_HDR_SIZE 8U
#define POWERVR_SGX_STREAM_MAX_SIZE 4096U

#define POWERVR_SGX_STREAM_EXTHDR_TYPE_SHIFT 29U
#define POWERVR_SGX_STREAM_EXTHDR_TYPE_MAX 8U
#define POWERVR_SGX_STREAM_EXTHDR_CONTINUATION BIT(28)
#define POWERVR_SGX_STREAM_EXTHDR_DATA_MASK 0x0fffffffU

enum powervr_sgx_stream_check {
	POWERVR_SGX_STREAM_CHECK_NONE = 0,
	POWERVR_SGX_STREAM_CHECK_RANGE,
	POWERVR_SGX_STREAM_CHECK_MASK,
	POWERVR_SGX_STREAM_CHECK_ADDR,
};

/* CLIENT has no range: VM_MAP only maps client heaps, and the mapped check covers it. */
enum powervr_sgx_stream_heap {
	POWERVR_SGX_STREAM_HEAP_CLIENT = 0,
	POWERVR_SGX_STREAM_HEAP_PDS_PIXEL,
	POWERVR_SGX_STREAM_HEAP_MAX,
};

enum powervr_sgx_stream_limit {
	POWERVR_SGX_STREAM_LIMIT_NONE = 0,
	POWERVR_SGX_STREAM_LIMIT_TILES_X,
	POWERVR_SGX_STREAM_LIMIT_TILES_Y,
	POWERVR_SGX_STREAM_LIMIT_MAX,
};

enum powervr_sgx_stream_extent {
	POWERVR_SGX_STREAM_EXTENT_NONE = 0,
	POWERVR_SGX_STREAM_EXTENT_BYTES,
	POWERVR_SGX_STREAM_EXTENT_RT_DEPTH,
	POWERVR_SGX_STREAM_EXTENT_RT_STENCIL,
	POWERVR_SGX_STREAM_EXTENT_ROWS16,
};

struct powervr_sgx_stream_def {
	const char *name;
	u16 offset; /* in the decoded struct */
	u8 size; /* 4 or 8 */
	u8 check;
	u8 limit; /* RANGE: replaces max when set */
	u8 heap; /* ADDR */
	u8 extent; /* ADDR */
	bool optional; /* ADDR: 0 means none */
	u16 extent_field; /* ROWS16: decoded offset of the u32 row count */
	u32 align; /* ADDR */
	u32 extent_bytes; /* BYTES */
	u64 min, max; /* RANGE */
	u64 mask; /* MASK */
};

struct powervr_sgx_stream_ext_def {
	const struct powervr_sgx_stream_def *fields;
	u32 nr_fields;
	u32 header_mask;
	u32 quirk; /* 0: none */
};

struct powervr_sgx_stream_ext_header {
	const struct powervr_sgx_stream_ext_def *ext_streams;
	u32 nr_ext_streams;
	u32 valid_mask;
};

struct powervr_sgx_stream_cmd_defs {
	const char *name;
	const struct powervr_sgx_stream_def *main;
	u32 nr_main;
	u32 main_len; /* bytes, header included */
	const struct powervr_sgx_stream_ext_header *ext_headers;
	u32 nr_ext_headers;
	size_t dest_size;
};

struct powervr_sgx_stream_ctx {
	/* Heap ranges; size 0 skips the range check. */
	struct {
		u64 base, size;
	} heaps[POWERVR_SGX_STREAM_HEAP_MAX];
	u32 limits[POWERVR_SGX_STREAM_LIMIT_MAX];

	/* Per extension header type, the bits this device requires, or NULL. */
	const u32 *musthave;
	bool (*has_quirk)(const void *priv, u32 quirk);

	/* For powervr_sgx_stream_check_mapped(). 0 for a transfer. */
	u32 rt_pad_width, rt_pad_height;
	bool (*mapped)(const void *priv, u64 addr, u64 size);

	const void *priv;
};

int powervr_sgx_stream_process(const struct powervr_sgx_stream_cmd_defs *defs,
			       const struct powervr_sgx_stream_ctx *ctx,
			       const void *stream, u32 stream_size, void *dest);
int powervr_sgx_stream_check_mapped(const struct powervr_sgx_stream_cmd_defs *defs,
				    const struct powervr_sgx_stream_ctx *ctx,
				    const void *dest);

/* Cross-field rules, which the XML can't express (powervr_sgx_cmd_checks.c). */
struct powervr_sgx_stream_render;
struct powervr_sgx_stream_transfer;
int powervr_sgx_check_render(const struct powervr_sgx_stream_render *r);
int powervr_sgx_check_transfer(const struct powervr_sgx_stream_transfer *t);

#endif /* POWERVR_SGX_STREAM_H */
