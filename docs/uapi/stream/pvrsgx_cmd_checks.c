// SPDX-License-Identifier: MIT
/* Copyright © 2026 Joseph Telaak */

/*
 * The cross-field rules of docs/UAPI.md section 3, run after
 * pvrsgx_stream_process() has checked each field on its own.
 */

#include <linux/errno.h>

#include "pvrsgx_stream_defs.h"

/* ZLS control bits that use the stencil address (UAPI.md 3.3). */
#define ZLS_STENCIL_BITS 0x3U
/* 3D flags bit that uses the stencil address. */
#define FLAGS_3D_STENCIL BIT(8)
/* ZLS addresses are offsets from a 1 MiB-aligned base; the limit is a guess (G13). */
#define ZLS_BASE_MASK 0xfff00000U
#define ZLS_OFFSET_LIMIT BIT(30)

int pvrsgx_check_render(const struct pvrsgx_stream_render *r)
{
	u64 base;

	if (r->zls_control && !r->depth_addr)
		return -EINVAL;
	if (((r->zls_control & ZLS_STENCIL_BITS) || (r->flags_3d & FLAGS_3D_STENCIL)) &&
	    !r->stencil_addr)
		return -EINVAL;

	if (!r->depth_addr && !r->stencil_addr)
		return 0;
	if (!r->depth_addr)
		base = r->stencil_addr;
	else if (!r->stencil_addr)
		base = r->depth_addr;
	else
		base = r->depth_addr < r->stencil_addr ? r->depth_addr : r->stencil_addr;
	base &= ZLS_BASE_MASK;

	if (r->depth_addr && r->depth_addr - base >= ZLS_OFFSET_LIMIT)
		return -EINVAL;
	if (r->stencil_addr && r->stencil_addr - base >= ZLS_OFFSET_LIMIT)
		return -EINVAL;
	return 0;
}

int pvrsgx_check_transfer(const struct pvrsgx_stream_transfer *t)
{
	if (t->first_tile_x > t->last_tile_x || t->first_tile_y > t->last_tile_y)
		return -EINVAL;
	return 0;
}
