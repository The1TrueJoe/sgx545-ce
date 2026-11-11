/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_USSE_H
#define PVRSGX_USSE_H

#include <linux/bits.h>
#include <linux/limits.h>
#include <linux/types.h>

#include "pvrsgx_core.h"

/* A code object's place when it isn't mapped, or is mapped at more than one. */
#define PVRSGX_USSE_PLACE_NONE	S64_MIN

/*
 * One code base's window, in instructions: 512 KiB. A task's absolute targets
 * (phas.i) count from the base it runs from, the window of its first word.
 */
#define PVRSGX_USSE_WINDOW_INSTS	BIT(16)

/* Why pvrsgx_usse_scan() refused an instruction. */
enum pvrsgx_usse_refusal {
	PVRSGX_USSE_REFUSAL_NONE,
	/* Not exactly one known form. */
	PVRSGX_USSE_REFUSAL_FORM,
	/* An operand's register file or number. */
	PVRSGX_USSE_REFUSAL_OPERAND,
	/* A form that is refused outright. */
	PVRSGX_USSE_REFUSAL_KIND,
	/* br's target outside the object. */
	PVRSGX_USSE_REFUSAL_BRANCH,
	/*
	 * phas.i's target outside the object, the object has no one place, or it
	 * crosses a code base window.
	 */
	PVRSGX_USSE_REFUSAL_PHAS,
};

s64 pvrsgx_usse_scan(enum pvrsgx_isa isa, const u64 *w, u32 n, u32 first, u32 obj_insts,
		     s64 place, bool *abs, enum pvrsgx_usse_refusal *why);
const char *pvrsgx_usse_refusal_name(enum pvrsgx_usse_refusal why);

#endif /* PVRSGX_USSE_H */
