// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * The USSE instruction check behind UPLOAD_CODE (pvrsgx_validate.c): each
 * word must be exactly one form of the table generated from the isaspec
 * description, with every operand in a register file and number the form
 * allows, and branch and phase targets inside the object. Nothing here
 * depends on the device, so the same code also builds for a host fuzzer
 * (drm/tests/usse_fuzz).
 */

#include <linux/array_size.h>
#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/kconfig.h>

#include "pvrsgx_usse.h"

enum pvrsgx_usse_kind {
	USSE_OK,
	USSE_REFUSE,
	USSE_BR,
	USSE_PHAS,
};

enum pvrsgx_usse_slot_kind {
	USSE_SLOT_NONE,
	USSE_SLOT_DST,
	USSE_SLOT_DST1,
	USSE_SLOT_SRC,
	USSE_SLOT_SRC0,
};

#define USSE_MAX_SLOTS	4
#define USSE_MAX_PIECES	3
#define USSE_ALWAYS	0xff

/* An operand slot: its value is the pieces' bits, each shifted into place. */
struct pvrsgx_usse_slot {
	u8 kind;
	/* Under a half or C10 format, number bit 6 is a format flag. */
	bool mode;
	/* The slot exists only while this bit of the word is set. */
	u8 when;
	struct {
		u8 lo, width, shift;
	} p[USSE_MAX_PIECES];
};

struct pvrsgx_usse_form {
	u64 mask, match;
	u8 kind;
	u8 slots;
};

#include "pvrsgx_usse_table.h"

/* The forms of each instruction set (pvrsgx_core.h). */
static const struct {
	const struct pvrsgx_usse_form *forms;
	u32 n;
} isas[] = {
	[PVRSGX_ISA_SGX545] = { pvrsgx_usse_forms, ARRAY_SIZE(pvrsgx_usse_forms) },
};

/* The register files an operand's bank and extension bits select. */
enum usse_file {
	USSE_R,		/* temporaries */
	USSE_O,		/* outputs */
	USSE_PA,	/* primary attributes */
	USSE_SA,	/* secondary attributes */
	USSE_INDEXED,	/* through idx1 or idx2 */
	USSE_CG,	/* constants c0-63, then globals g0- */
	USSE_IDX,	/* the index registers */
	USSE_IMM,	/* an immediate */
	USSE_I,		/* internal registers */
};

/* By bank | extension << 2 (dst, src) or << 1 (src0, dst1). */
static const u8 dst_files[8] = {
	USSE_R, USSE_O, USSE_PA, USSE_INDEXED, USSE_SA, USSE_CG, USSE_IDX, USSE_I,
};

static const u8 src_files[8] = {
	USSE_R, USSE_O, USSE_PA, USSE_SA, USSE_INDEXED, USSE_CG, USSE_IMM, USSE_I,
};

static const u8 src0_files[4] = { USSE_R, USSE_PA, USSE_O, USSE_SA };

/* g46 on (number 110) is undocumented; i0-i3 and idx1-idx2 are what exists. */
#define USSE_CG_END		110
#define USSE_I_LAST		3

static bool usse_slot_ok(const struct pvrsgx_usse_slot *s, u64 w)
{
	u32 v = 0, n, file;

	if (s->when != USSE_ALWAYS && !(w & BIT_ULL(s->when)))
		return true;
	for (int i = 0; i < USSE_MAX_PIECES && s->p[i].width; i++)
		v |= ((w >> s->p[i].lo) & GENMASK_ULL(s->p[i].width - 1, 0)) << s->p[i].shift;
	n = v & 0x7f;
	switch (s->kind) {
	case USSE_SLOT_DST:
		file = dst_files[(v >> 7) & 7];
		break;
	case USSE_SLOT_SRC:
		file = src_files[(v >> 7) & 7];
		break;
	case USSE_SLOT_SRC0:
		file = src0_files[(v >> 7) & 3];
		break;
	case USSE_SLOT_DST1:
		file = (v >> 7) & 1 ? USSE_PA : USSE_R;
		break;
	default:
		return false;
	}
	if (s->mode)
		n &= 0x3f;
	switch (file) {
	case USSE_CG:
		/* Read only: what writing c or g does isn't known. */
		return s->kind != USSE_SLOT_DST && (v & 0x7f) < USSE_CG_END;
	case USSE_I:
		return n <= USSE_I_LAST;
	case USSE_IDX:
		return n == 1 || n == 2;
	default:
		return true;
	}
}

/* br's and phas.i's targets: l0-17, in instructions. */
#define USSE_TARGET		GENMASK_ULL(17, 0)

static s64 refuse(enum pvrsgx_usse_refusal *why, enum pvrsgx_usse_refusal r, u32 i)
{
	if (why)
		*why = r;
	return i;
}

/**
 * pvrsgx_usse_scan() - Check USSE code before it goes into a code object
 * @isa: The core's instruction set.
 * @w: The instructions.
 * @n: Number of @w.
 * @first: Instruction index @w[0] will sit at in the object.
 * @obj_insts: The object's size in instructions.
 * @place: The object's start, in instructions from the base of the USSE heap
 *	it is mapped in, or %PVRSGX_USSE_PLACE_NONE. An absolute target
 *	(phas.i) needs it.
 * @abs: Set when an instruction has an absolute target.
 * @why: If not NULL, why the instruction returned was refused.
 *
 * Linear in @n, no allocation.
 *
 * Return: the index in @w of the first refused instruction, or -1.
 */
s64 pvrsgx_usse_scan(enum pvrsgx_isa isa, const u64 *w, u32 n, u32 first, u32 obj_insts,
		     s64 place, bool *abs, enum pvrsgx_usse_refusal *why)
{
	const struct pvrsgx_usse_form *forms = isas[isa].forms;

	if (why)
		*why = PVRSGX_USSE_REFUSAL_NONE;
	for (u32 i = 0; i < n; i++) {
		const struct pvrsgx_usse_form *f = NULL;
		s64 target;

		for (u32 k = 0; k < isas[isa].n; k++) {
			if ((w[i] & forms[k].mask) == forms[k].match) {
				f = &forms[k];
				break;
			}
		}
		if (!f)
			return refuse(why, PVRSGX_USSE_REFUSAL_FORM, i);
		for (int k = 0; k < USSE_MAX_SLOTS; k++) {
			const struct pvrsgx_usse_slot *s = &pvrsgx_usse_slots[f->slots][k];

			if (s->kind != USSE_SLOT_NONE && !usse_slot_ok(s, w[i]))
				return refuse(why, PVRSGX_USSE_REFUSAL_OPERAND, i);
		}
		switch (f->kind) {
		case USSE_OK:
			break;
		case USSE_BR:
			target = (s64)first + i + sign_extend64(w[i] & USSE_TARGET, 17);
			if (target < 0 || target >= obj_insts)
				return refuse(why, PVRSGX_USSE_REFUSAL_BRANCH, i);
			break;
		case USSE_PHAS:
			/* 0: no next phase. Otherwise from the heap base. */
			target = w[i] & USSE_TARGET;
			if (!target)
				break;
			if (place == PVRSGX_USSE_PLACE_NONE || target < place ||
			    target >= place + obj_insts)
				return refuse(why, PVRSGX_USSE_REFUSAL_PHAS, i);
			*abs = true;
			break;
		default:
			return refuse(why, PVRSGX_USSE_REFUSAL_KIND, i);
		}
	}
	return -1;
}

const char *pvrsgx_usse_refusal_name(enum pvrsgx_usse_refusal why)
{
	static const char *const names[] = {
		[PVRSGX_USSE_REFUSAL_NONE] = "none",
		[PVRSGX_USSE_REFUSAL_FORM] = "no known form",
		[PVRSGX_USSE_REFUSAL_OPERAND] = "operand",
		[PVRSGX_USSE_REFUSAL_KIND] = "refused instruction",
		[PVRSGX_USSE_REFUSAL_BRANCH] = "branch target outside the object",
		[PVRSGX_USSE_REFUSAL_PHAS] = "phase target outside the object",
	};

	return why < ARRAY_SIZE(names) ? names[why] : "?";
}

#if IS_ENABLED(CONFIG_DRM_POWERVR_SGX_KUNIT_TEST)
#include "tests/pvrsgx_usse_test.c"
#endif
