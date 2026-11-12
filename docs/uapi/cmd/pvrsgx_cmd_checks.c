// SPDX-License-Identifier: MIT
/* Copyright © 2026 Joseph Telaak */

/*
 * The cross-field rules of the client commands, run
 * after pvrsgx_cmd_check() has checked each field on its own.
 */

#include <linux/bits.h>
#include <linux/errno.h>

#include "pvrsgx_cmd_checks.h"

/* SGX545 ZLS control bits that use the stencil address. */
#define ZLS_STENCIL_BITS 0x3U
/* SGX545 3D flags bit that uses the stencil address. */
#define FLAGS_3D_STENCIL BIT(8)
/* ZLS addresses are offsets from a 1 MiB-aligned base; the limit is a guess (G13). */
#define ZLS_BASE_MASK 0xfff00000U
#define ZLS_OFFSET_LIMIT_LOG2 30

int pvrsgx_check_render(const struct pvrsgx_cmd_core *core, const struct pvrsgx_cmd_render_all *r,
			u32 samples)
{
	/* The SGX545 extension; all zero when it wasn't sent or on another core. */
	u32 zls = r->sgx545_render.zls_control, flags = r->sgx545_render.flags_3d;
	u64 depth = r->base.depth_addr, stencil = r->base.stencil_addr, base, limit;

	/* Multisampled depth and stencil are per sample and always stored. */
	if (samples == 4 && !zls)
		return -EINVAL;
	if (zls && !depth)
		return -EINVAL;
	if (((zls & ZLS_STENCIL_BITS) || (flags & FLAGS_3D_STENCIL)) && !stencil)
		return -EINVAL;

	if (!depth && !stencil)
		return 0;
	if (!depth)
		base = stencil;
	else if (!stencil)
		base = depth;
	else
		base = depth < stencil ? depth : stencil;
	base &= ZLS_BASE_MASK;
	limit = BIT_ULL(core->va_bits < ZLS_OFFSET_LIMIT_LOG2 ? core->va_bits :
			ZLS_OFFSET_LIMIT_LOG2);

	if (depth && depth - base >= limit)
		return -EINVAL;
	if (stencil && stencil - base >= limit)
		return -EINVAL;
	return 0;
}

int pvrsgx_check_transfer(const struct pvrsgx_cmd_transfer_all *t)
{
	if (t->base.first_tile_x > t->base.last_tile_x ||
	    t->base.first_tile_y > t->base.last_tile_y)
		return -EINVAL;
	return 0;
}
