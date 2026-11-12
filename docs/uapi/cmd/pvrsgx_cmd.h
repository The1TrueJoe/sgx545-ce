/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */

/*
 * Client commands (docs/uapi/rev3-commands.md): copied from userspace with
 * their extensions, then checked field by field from the tables
 * docs/uapi/gen_cmd.py writes per core (pvrsgx_cmd_defs.c).
 */

#ifndef PVRSGX_CMD_H
#define PVRSGX_CMD_H

#include <linux/types.h>

#include <uapi/drm/powervr_sgx_drm.h>

#define PVRSGX_CMD_MAX_SIZE 4096U
#define PVRSGX_CMD_MAX_EXTS 16U
#define PVRSGX_CMD_TYPES_MAX 8U

enum pvrsgx_cmd_check {
	PVRSGX_CMD_CHECK_NONE = 0,
	PVRSGX_CMD_CHECK_ZERO,	/* padding */
	PVRSGX_CMD_CHECK_RANGE,
	PVRSGX_CMD_CHECK_MASK,
	PVRSGX_CMD_CHECK_ADDR,
};

/* CLIENT has no range: VM_MAP only maps client heaps, and the mapped check covers it. */
enum pvrsgx_cmd_heap {
	PVRSGX_CMD_HEAP_CLIENT = 0,
	PVRSGX_CMD_HEAP_PDS_PIXEL,
	PVRSGX_CMD_HEAP_MAX,
};

enum pvrsgx_cmd_limit {
	PVRSGX_CMD_LIMIT_NONE = 0,
	PVRSGX_CMD_LIMIT_TILES_X,
	PVRSGX_CMD_LIMIT_TILES_Y,
	PVRSGX_CMD_LIMIT_MAX,
};

enum pvrsgx_cmd_extent {
	PVRSGX_CMD_EXTENT_NONE = 0,
	PVRSGX_CMD_EXTENT_BYTES,
	PVRSGX_CMD_EXTENT_RT_DEPTH,
	PVRSGX_CMD_EXTENT_RT_STENCIL,
	PVRSGX_CMD_EXTENT_PDS_ROWS,
};

/* One field's check. Offsets are into the command's all-struct (pvrsgx_cmd_defs.h). */
struct pvrsgx_cmd_field {
	const char *name;
	u16 offset;
	u8 size;		/* 4 or 8 */
	u8 check;
	u8 limit;		/* RANGE: replaces max when set */
	u8 heap;		/* ADDR */
	u8 extent;		/* ADDR */
	bool optional;		/* ADDR: 0 means none */
	u16 extent_field;	/* PDS_ROWS: offset of the u32 row count */
	u32 align;		/* ADDR */
	u32 extent_bytes;	/* BYTES */
	u64 min, max;		/* RANGE */
	u64 mask;		/* MASK */
};

struct pvrsgx_cmd_ext_def {
	u32 id;			/* DRM_POWERVR_SGX_EXT_* */
	u16 size;		/* sizeof its struct */
	u16 offset;		/* in the all-struct */
	const struct pvrsgx_cmd_field *fields;
	u32 nr_fields;
};

/* A command on one core. Not supported: a value it depends on is unknown there. */
struct pvrsgx_cmd_def {
	const char *name;
	u8 type;		/* enum pvrsgx_cmd_type */
	bool supported;
	u16 size;		/* sizeof the base struct */
	u16 all_size;		/* sizeof the all-struct: base and every extension */
	const struct pvrsgx_cmd_field *fields;
	u32 nr_fields;
	const struct pvrsgx_cmd_ext_def *exts;	/* the extensions this core accepts */
	u32 nr_exts;
};

struct pvrsgx_cmd_core {
	const char *name;
	u32 va_bits;
	u32 pds_row_bytes;
	u32 max_samples;	/* RT datasets: 1, or 4 where 4x is known to work */
	const struct pvrsgx_cmd_def *cmds[PVRSGX_CMD_TYPES_MAX];
};

/* What a command is checked against: its VM, its RT, the device. */
struct pvrsgx_cmd_ctx {
	struct {
		u64 base, size;
	} heaps[PVRSGX_CMD_HEAP_MAX];	/* size 0: refuse every address */
	u64 limits[PVRSGX_CMD_LIMIT_MAX];
	u32 rt_pad_width, rt_pad_height;	/* the RT's size in whole tiles */
	u32 rt_samples;				/* 0 is 1 */
	bool (*mapped)(const void *priv, u64 addr, u64 size);
	const void *priv;
};

/*
 * A command at a user pointer, with its extensions, into @all (def->all_size
 * bytes; absent extensions zero). @present gets a bit per extension sent.
 */
int pvrsgx_cmd_copy(const struct pvrsgx_cmd_def *def, u64 uptr, void *all, u64 *present);

/*
 * An array element obj_array_get() already copied at @stride (a TA kick):
 * its head.size must equal @stride. Copies it into @all and walks its extensions.
 */
int pvrsgx_cmd_take_elem(const struct pvrsgx_cmd_def *def, const void *elem, u32 stride,
			 void *all, u64 *present);

/* Every field on its own: padding, ranges, masks, addresses' heaps and alignment. */
int pvrsgx_cmd_check(const struct pvrsgx_cmd_core *core, const struct pvrsgx_cmd_def *def,
		     const struct pvrsgx_cmd_ctx *ctx, const void *all, u64 present);

/* Every address's extent is mapped in the VM (with the VM's lock held). */
int pvrsgx_cmd_check_mapped(const struct pvrsgx_cmd_core *core, const struct pvrsgx_cmd_def *def,
			    const struct pvrsgx_cmd_ctx *ctx, const void *all, u64 present);

/* The extension names @def accepts, a bit per name (DEV_QUERY COMMANDS_GET). */
u64 pvrsgx_cmd_ext_mask(const struct pvrsgx_cmd_def *def);

#endif /* PVRSGX_CMD_H */
