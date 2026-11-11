// SPDX-License-Identifier: MIT
/* Copyright © 2026 Joseph Telaak */

#include <linux/align.h>
#include <linux/errno.h>
#include <linux/string.h>

#include "pvrsgx_stream.h"

/*
 * The stream has already been copied into kernel memory (at most
 * PVRSGX_STREAM_MAX_SIZE bytes), so nothing here races with userspace.
 */

/* Aligns *off to @align; the skipped bytes must be zero. */
static int stream_align(const u8 *stream, u32 *off, u32 end, u32 align)
{
	u32 to = ALIGN(*off, align);

	if (to > end)
		return -EINVAL;
	for (; *off < to; (*off)++) {
		if (stream[*off])
			return -EINVAL;
	}
	return 0;
}

static int stream_get(const u8 *stream, u32 *off, u32 end, u32 size, void *dest)
{
	int err = stream_align(stream, off, end, size);

	if (err)
		return err;
	if (end - *off < size)
		return -EINVAL;
	memcpy(dest, stream + *off, size);
	*off += size;
	return 0;
}

static int check_field(const struct pvrsgx_stream_def *def,
		       const struct pvrsgx_stream_ctx *ctx, u64 v)
{
	u64 max;

	switch (def->check) {
	case PVRSGX_STREAM_CHECK_NONE:
		return 0;

	case PVRSGX_STREAM_CHECK_RANGE:
		max = def->limit ? ctx->limits[def->limit] : def->max;
		return v < def->min || v > max ? -EINVAL : 0;

	case PVRSGX_STREAM_CHECK_MASK:
		return v & ~def->mask ? -EINVAL : 0;

	case PVRSGX_STREAM_CHECK_ADDR:
		if (!v)
			return def->optional ? 0 : -EINVAL;
		/* The GPU's address words are 32 bits. */
		if (v >> 32 || v & (def->align - 1))
			return -EINVAL;
		if (ctx->heaps[def->heap].size &&
		    (v < ctx->heaps[def->heap].base ||
		     v - ctx->heaps[def->heap].base >= ctx->heaps[def->heap].size))
			return -EINVAL;
		return 0;
	}
	return -EINVAL;
}

static int process_fields(const struct pvrsgx_stream_def *defs, u32 nr,
			  const struct pvrsgx_stream_ctx *ctx,
			  const u8 *stream, u32 *off, u32 end, u8 *dest, size_t dest_size)
{
	for (u32 i = 0; i < nr; i++) {
		const struct pvrsgx_stream_def *def = &defs[i];
		u64 v = 0;
		int err;

		if (def->offset + def->size > dest_size)
			return -EINVAL;
		err = stream_get(stream, off, end, def->size, &v);
		if (err)
			return err;
		err = check_field(def, ctx, v);
		if (err)
			return err;
		memcpy(dest + def->offset, &v, def->size);
	}
	return 0;
}

static int process_ext(const struct pvrsgx_stream_cmd_defs *defs,
		       const struct pvrsgx_stream_ctx *ctx,
		       const u8 *stream, u32 off, u32 size, u8 *dest)
{
	u32 musthave[PVRSGX_STREAM_EXTHDR_TYPE_MAX] = { 0 };
	u32 seen = 0;
	u32 hdr;

	if (ctx->musthave)
		memcpy(musthave, ctx->musthave, defs->nr_ext_headers * sizeof(u32));

	do {
		const struct pvrsgx_stream_ext_header *eh;
		u32 type, data;
		int err;

		err = stream_get(stream, &off, size, sizeof(hdr), &hdr);
		if (err)
			return err;

		type = hdr >> PVRSGX_STREAM_EXTHDR_TYPE_SHIFT;
		data = hdr & PVRSGX_STREAM_EXTHDR_DATA_MASK;
		if (type >= defs->nr_ext_headers || seen & BIT(type))
			return -EINVAL;
		seen |= BIT(type);

		eh = &defs->ext_headers[type];
		if (data & ~eh->valid_mask)
			return -EINVAL;
		musthave[type] &= ~data;

		for (u32 i = 0; i < eh->nr_ext_streams; i++) {
			const struct pvrsgx_stream_ext_def *ed = &eh->ext_streams[i];

			if (!(data & ed->header_mask))
				continue;
			if (ed->quirk && !(ctx->has_quirk && ctx->has_quirk(ctx->priv, ed->quirk)))
				return -EINVAL;
			err = process_fields(ed->fields, ed->nr_fields, ctx, stream, &off, size,
					     dest, defs->dest_size);
			if (err)
				return err;
		}
	} while (hdr & PVRSGX_STREAM_EXTHDR_CONTINUATION);

	/* Nothing may follow the last extension stream. */
	if (off != size)
		return -EINVAL;

	for (u32 i = 0; i < defs->nr_ext_headers; i++) {
		if (musthave[i])
			return -EINVAL;
	}
	return 0;
}

/**
 * pvrsgx_stream_process() - Decode and check a client command stream
 * @defs: Stream definition, from the generated tables.
 * @ctx: Heaps, limits and quirks of the device.
 * @stream: The stream, already copied from userspace.
 * @stream_size: Its size in bytes.
 * @dest: Decoded struct of @defs->dest_size bytes. Fields not sent are zero.
 *
 * Checks the framing and every field on its own. The caller still runs
 * pvrsgx_stream_check_mapped() and the command's cross-field checks.
 *
 * Return: 0, or -EINVAL for a malformed stream or a field out of range.
 */
int pvrsgx_stream_process(const struct pvrsgx_stream_cmd_defs *defs,
			  const struct pvrsgx_stream_ctx *ctx,
			  const void *stream, u32 stream_size, void *dest)
{
	const u8 *s = stream;
	u32 off = 0;
	u32 main_len, padding;
	int err;

	memset(dest, 0, defs->dest_size);

	if (!stream || stream_size > PVRSGX_STREAM_MAX_SIZE)
		return -EINVAL;
	err = stream_get(s, &off, stream_size, sizeof(u32), &main_len);
	if (!err)
		err = stream_get(s, &off, stream_size, sizeof(u32), &padding);
	if (err)
		return err;

	/* The main stream is frozen: exactly the fields this kernel knows. */
	if (padding || main_len != defs->main_len || main_len > stream_size)
		return -EINVAL;

	err = process_fields(defs->main, defs->nr_main, ctx, s, &off, main_len, dest,
			     defs->dest_size);
	if (err)
		return err;
	if (off != main_len)
		return -EINVAL;

	if (off < stream_size)
		return process_ext(defs, ctx, s, off, stream_size, dest);

	for (u32 i = 0; ctx->musthave && i < defs->nr_ext_headers; i++) {
		if (ctx->musthave[i])
			return -EINVAL;
	}
	return 0;
}

static int check_mapped_fields(const struct pvrsgx_stream_def *defs, u32 nr,
			       const struct pvrsgx_stream_ctx *ctx, const u8 *dest)
{
	for (u32 i = 0; i < nr; i++) {
		const struct pvrsgx_stream_def *def = &defs[i];
		u64 addr = 0, size;
		u32 rows;

		if (def->check != PVRSGX_STREAM_CHECK_ADDR || !def->extent)
			continue;
		memcpy(&addr, dest + def->offset, def->size);
		/* An optional address, or an extension stream that was not sent. */
		if (!addr)
			continue;

		switch (def->extent) {
		case PVRSGX_STREAM_EXTENT_BYTES:
			size = def->extent_bytes;
			break;
		case PVRSGX_STREAM_EXTENT_RT_DEPTH:
			size = (u64)ctx->rt_pad_width * ctx->rt_pad_height * 4;
			break;
		case PVRSGX_STREAM_EXTENT_RT_STENCIL:
			size = (u64)ctx->rt_pad_width * ctx->rt_pad_height;
			break;
		case PVRSGX_STREAM_EXTENT_ROWS16:
			memcpy(&rows, dest + def->extent_field, sizeof(rows));
			size = (u64)rows * 16;
			break;
		default:
			return -EINVAL;
		}
		if (!size || !ctx->mapped(ctx->priv, addr, size))
			return -EINVAL;
	}
	return 0;
}

/**
 * pvrsgx_stream_check_mapped() - Check that addresses are mapped
 * @defs: Stream definition.
 * @ctx: The job's VM (@ctx->mapped) and RT dataset size.
 * @dest: Struct decoded by pvrsgx_stream_process().
 *
 * A programming-error check made at submit time: the client can unmap
 * afterwards, so this is not the isolation boundary.
 *
 * Return: 0, or -EINVAL when a range is not mapped.
 */
int pvrsgx_stream_check_mapped(const struct pvrsgx_stream_cmd_defs *defs,
			       const struct pvrsgx_stream_ctx *ctx,
			       const void *dest)
{
	int err = check_mapped_fields(defs->main, defs->nr_main, ctx, dest);

	for (u32 t = 0; !err && t < defs->nr_ext_headers; t++) {
		const struct pvrsgx_stream_ext_header *eh = &defs->ext_headers[t];

		for (u32 i = 0; !err && i < eh->nr_ext_streams; i++)
			err = check_mapped_fields(eh->ext_streams[i].fields,
						  eh->ext_streams[i].nr_fields, ctx, dest);
	}
	return err;
}
