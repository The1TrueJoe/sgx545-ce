/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */

/*
 * Stream decoder of the powervr_sgx uAPI: the reference for the kernel driver.
 * Same framing as the upstream PowerVR driver's pvr_stream.c, stricter (no
 * unchecked bytes), plus a validator per field. The tables are generated from
 * the stream definitions (powervr_sgx_kmd_stream.xml).
 */

#ifndef PVRSGX_STREAM_H
#define PVRSGX_STREAM_H

#include <linux/bits.h>
#include <linux/types.h>

#define PVRSGX_STREAM_HDR_SIZE 8U
#define PVRSGX_STREAM_MAX_SIZE 4096U

#define PVRSGX_STREAM_EXTHDR_TYPE_SHIFT 29U
#define PVRSGX_STREAM_EXTHDR_TYPE_MAX 8U
#define PVRSGX_STREAM_EXTHDR_CONTINUATION BIT(28)
#define PVRSGX_STREAM_EXTHDR_DATA_MASK 0x0fffffffU

enum pvrsgx_stream_check {
	PVRSGX_STREAM_CHECK_NONE = 0,
	PVRSGX_STREAM_CHECK_RANGE,
	PVRSGX_STREAM_CHECK_MASK,
	PVRSGX_STREAM_CHECK_ADDR,
};

/* CLIENT has no range: VM_BIND only maps client heaps, and the mapped check covers it. */
enum pvrsgx_stream_heap {
	PVRSGX_STREAM_HEAP_CLIENT = 0,
	PVRSGX_STREAM_HEAP_PDS_PIXEL,
	PVRSGX_STREAM_HEAP_MAX,
};

enum pvrsgx_stream_limit {
	PVRSGX_STREAM_LIMIT_NONE = 0,
	PVRSGX_STREAM_LIMIT_TILES_X,
	PVRSGX_STREAM_LIMIT_TILES_Y,
	PVRSGX_STREAM_LIMIT_MAX,
};

enum pvrsgx_stream_extent {
	PVRSGX_STREAM_EXTENT_NONE = 0,
	PVRSGX_STREAM_EXTENT_BYTES,
	PVRSGX_STREAM_EXTENT_RT_DEPTH,
	PVRSGX_STREAM_EXTENT_RT_STENCIL,
	PVRSGX_STREAM_EXTENT_ROWS16,
};

struct pvrsgx_stream_def {
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

struct pvrsgx_stream_ext_def {
	const struct pvrsgx_stream_def *fields;
	u32 nr_fields;
	u32 header_mask;
	u32 quirk; /* 0: none */
};

struct pvrsgx_stream_ext_header {
	const struct pvrsgx_stream_ext_def *ext_streams;
	u32 nr_ext_streams;
	u32 valid_mask;
};

struct pvrsgx_stream_cmd_defs {
	const char *name;
	const struct pvrsgx_stream_def *main;
	u32 nr_main;
	u32 main_len; /* bytes, header included */
	const struct pvrsgx_stream_ext_header *ext_headers;
	u32 nr_ext_headers;
	size_t dest_size;
};

struct pvrsgx_stream_ctx {
	/* Heap ranges, by enum pvrsgx_stream_heap. CLIENT's is never read; a
	 * heap left at size 0 refuses every address in it. */
	struct {
		u64 base, size;
	} heaps[PVRSGX_STREAM_HEAP_MAX];
	u32 limits[PVRSGX_STREAM_LIMIT_MAX];

	/* Per extension header type, the bits this device requires, or NULL. */
	const u32 *musthave;
	bool (*has_quirk)(const void *priv, u32 quirk);

	/* For pvrsgx_stream_check_mapped(). 0 for a transfer. */
	u32 rt_pad_width, rt_pad_height;
	bool (*mapped)(const void *priv, u64 addr, u64 size);

	const void *priv;
};

int pvrsgx_stream_process(const struct pvrsgx_stream_cmd_defs *defs,
			  const struct pvrsgx_stream_ctx *ctx,
			  const void *stream, u32 stream_size, void *dest);
int pvrsgx_stream_check_mapped(const struct pvrsgx_stream_cmd_defs *defs,
			       const struct pvrsgx_stream_ctx *ctx,
			       const void *dest);

/* Cross-field rules, which the XML can't express (pvrsgx_cmd_checks.c). */
struct pvrsgx_stream_render;
struct pvrsgx_stream_transfer;
int pvrsgx_check_render(const struct pvrsgx_stream_render *r);
int pvrsgx_check_transfer(const struct pvrsgx_stream_transfer *t);

#endif /* PVRSGX_STREAM_H */
