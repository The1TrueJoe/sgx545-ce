// SPDX-License-Identifier: MIT
/* Copyright © 2026 Joseph Telaak */

/*
 * Client commands: the copy from userspace and the per-field checks
 * (docs/uapi/rev3-commands.md sections 3 and 4). The per-command cross-field
 * rules are in pvrsgx_cmd_checks.c.
 */

#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "pvrsgx_cmd_defs.h"

static const struct pvrsgx_cmd_ext_def *ext_find(const struct pvrsgx_cmd_def *def, u32 id)
{
	for (u32 i = 0; i < def->nr_exts; i++) {
		if (def->exts[i].id == id)
			return &def->exts[i];
	}
	return NULL;
}

static int take_exts(const struct pvrsgx_cmd_def *def, u64 uptr, u8 *all, u64 *present)
{
	u32 n = 0;

	*present = 0;
	while (uptr) {
		const struct pvrsgx_cmd_ext_def *ed;
		struct drm_powervr_sgx_ext head;
		u8 *dst;

		if (++n > PVRSGX_CMD_MAX_EXTS)
			return -EINVAL;
		if (copy_from_user(&head, u64_to_user_ptr(uptr), sizeof(head)))
			return -EFAULT;
		if (head._padding_4 || !head.name || head.name >= 64 ||
		    pvrsgx_cmd_ext_owner[head.name] != 1 + def->type)
			return -EINVAL;	/* unknown, or another command's */
		if (*present & BIT_ULL(head.name))
			return -EINVAL;
		ed = ext_find(def, head.name);
		if (!ed)
			return -EOPNOTSUPP;	/* this command's, but not on this core */
		dst = all + ed->offset;
		if (copy_from_user(dst, u64_to_user_ptr(uptr), ed->size))
			return -EFAULT;
		/* the copy is what counts: a racing writer can't change what was checked */
		if (((struct drm_powervr_sgx_ext *)dst)->name != head.name)
			return -EINVAL;
		*present |= BIT_ULL(head.name);
		uptr = ((struct drm_powervr_sgx_ext *)dst)->next;
	}
	return 0;
}

int pvrsgx_cmd_copy(const struct pvrsgx_cmd_def *def, u64 uptr, void *all, u64 *present)
{
	const void __user *src = u64_to_user_ptr(uptr);
	struct drm_powervr_sgx_cmd_head *head = all;
	u32 size;
	int err;

	memset(all, 0, def->all_size);
	*present = 0;
	if (!def->supported)
		return -EOPNOTSUPP;
	if (!uptr)
		return -EINVAL;
	if (get_user(size, (const u32 __user *)src))
		return -EFAULT;
	if (size < sizeof(*head) || size > PVRSGX_CMD_MAX_SIZE)
		return -EINVAL;
	/* -E2BIG for non-zero bytes past what this kernel knows, as everywhere */
	err = copy_struct_from_user(all, def->size, src, size);
	if (err)
		return err;
	if (head->size != size)
		return -EINVAL;
	return take_exts(def, head->extensions, all, present);
}

int pvrsgx_cmd_take_elem(const struct pvrsgx_cmd_def *def, const void *elem, u32 stride,
			 void *all, u64 *present)
{
	const struct drm_powervr_sgx_cmd_head *head = elem;

	memset(all, 0, def->all_size);
	*present = 0;
	if (!def->supported)
		return -EOPNOTSUPP;
	if (stride < sizeof(*head) || head->size != stride)
		return -EINVAL;
	/* obj_array_get() copied min(stride, def->size) and checked the tail */
	memcpy(all, elem, def->size);
	return take_exts(def, head->extensions, all, present);
}

static u64 cmd_field_get(const u8 *all, const struct pvrsgx_cmd_field *f)
{
	u64 v = 0;

	memcpy(&v, all + f->offset, f->size);	/* little-endian: the low bytes */
	return v;
}

static int check_field(const struct pvrsgx_cmd_core *core, const struct pvrsgx_cmd_field *f,
		       const struct pvrsgx_cmd_ctx *ctx, u64 v)
{
	switch (f->check) {
	case PVRSGX_CMD_CHECK_NONE:
		return 0;
	case PVRSGX_CMD_CHECK_ZERO:
		return v ? -EINVAL : 0;
	case PVRSGX_CMD_CHECK_RANGE: {
		u64 max = f->limit ? ctx->limits[f->limit] : f->max;

		return v < f->min || v > max ? -EINVAL : 0;
	}
	case PVRSGX_CMD_CHECK_MASK:
		return v & ~f->mask ? -EINVAL : 0;
	case PVRSGX_CMD_CHECK_ADDR:
		if (!v)
			return f->optional ? 0 : -EINVAL;
		if (core->va_bits < 64 && v >> core->va_bits)
			return -EINVAL;
		if (v & (f->align - 1))
			return -EINVAL;
		if (f->heap == PVRSGX_CMD_HEAP_CLIENT)
			return 0;	/* VM_MAP maps only client heaps; check_mapped covers it */
		if (f->heap >= PVRSGX_CMD_HEAP_MAX || !ctx->heaps[f->heap].size ||
		    v < ctx->heaps[f->heap].base ||
		    v - ctx->heaps[f->heap].base >= ctx->heaps[f->heap].size)
			return -EINVAL;
		return 0;
	}
	return -EINVAL;
}

static int check_fields(const struct pvrsgx_cmd_core *core, const struct pvrsgx_cmd_field *fields,
			u32 n, const struct pvrsgx_cmd_ctx *ctx, const u8 *all)
{
	for (u32 i = 0; i < n; i++) {
		int err = check_field(core, &fields[i], ctx, cmd_field_get(all, &fields[i]));

		if (err)
			return err;
	}
	return 0;
}

int pvrsgx_cmd_check(const struct pvrsgx_cmd_core *core, const struct pvrsgx_cmd_def *def,
		     const struct pvrsgx_cmd_ctx *ctx, const void *all, u64 present)
{
	const struct drm_powervr_sgx_cmd_head *head = all;
	int err;

	if (!def->supported)
		return -EOPNOTSUPP;
	if (head->_padding_4)
		return -EINVAL;
	err = check_fields(core, def->fields, def->nr_fields, ctx, all);
	for (u32 i = 0; !err && i < def->nr_exts; i++) {
		const struct pvrsgx_cmd_ext_def *ed = &def->exts[i];

		if (present & BIT_ULL(ed->id))	/* absent: all zero, rev 1's "off" */
			err = check_fields(core, ed->fields, ed->nr_fields, ctx, all);
	}
	return err;
}

static int mapped_fields(const struct pvrsgx_cmd_core *core, const struct pvrsgx_cmd_field *fields,
			 u32 n, const struct pvrsgx_cmd_ctx *ctx, const u8 *all)
{
	for (u32 i = 0; i < n; i++) {
		const struct pvrsgx_cmd_field *f = &fields[i];
		u64 addr, size, samples = ctx->rt_samples ? ctx->rt_samples : 1;

		if (f->check != PVRSGX_CMD_CHECK_ADDR || !f->extent)
			continue;
		addr = cmd_field_get(all, f);
		if (!addr)
			continue;	/* an optional address left out */
		switch (f->extent) {
		case PVRSGX_CMD_EXTENT_BYTES:
			size = f->extent_bytes;
			break;
		case PVRSGX_CMD_EXTENT_RT_DEPTH:
			size = (u64)ctx->rt_pad_width * ctx->rt_pad_height * 4 * samples;
			break;
		case PVRSGX_CMD_EXTENT_RT_STENCIL:
			size = (u64)ctx->rt_pad_width * ctx->rt_pad_height * samples;
			break;
		case PVRSGX_CMD_EXTENT_PDS_ROWS: {
			u32 rows;

			memcpy(&rows, all + f->extent_field, sizeof(rows));
			size = (u64)rows * core->pds_row_bytes;
			break;
		}
		default:
			return -EINVAL;
		}
		if (!size || !ctx->mapped(ctx->priv, addr, size))
			return -EINVAL;
	}
	return 0;
}

int pvrsgx_cmd_check_mapped(const struct pvrsgx_cmd_core *core, const struct pvrsgx_cmd_def *def,
			    const struct pvrsgx_cmd_ctx *ctx, const void *all, u64 present)
{
	int err = mapped_fields(core, def->fields, def->nr_fields, ctx, all);

	for (u32 i = 0; !err && i < def->nr_exts; i++) {
		const struct pvrsgx_cmd_ext_def *ed = &def->exts[i];

		if (present & BIT_ULL(ed->id))
			err = mapped_fields(core, ed->fields, ed->nr_fields, ctx, all);
	}
	return err;
}

u64 pvrsgx_cmd_ext_mask(const struct pvrsgx_cmd_def *def)
{
	u64 mask = 0;

	for (u32 i = 0; def->supported && i < def->nr_exts; i++)
		mask |= BIT_ULL(def->exts[i].id);
	return mask;
}
