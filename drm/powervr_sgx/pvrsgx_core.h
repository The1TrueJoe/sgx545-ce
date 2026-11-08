/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_CORE_H
#define PVRSGX_CORE_H

#include <linux/bits.h>
#include <linux/types.h>

/*
 * Core features, as the SGX_FEATURE_* names of the GPL DDK's
 * sgxfeaturedefs.h. Only those the driver's design depends on are listed.
 */
/* One page directory per client: BIF directory lists beyond list 0. */
#define PVRSGX_FEATURE_MULTIPLE_MEM_CONTEXTS	BIT(0)
/* EUR_CR_EVENT_KICK1..3 next to EUR_CR_EVENT_KICK. */
#define PVRSGX_FEATURE_MULTI_EVENT_KICK		BIT(1)

/* What this driver needs: a directory per VM, and the second kick for the microkernel's start. */
#define PVRSGX_FEATURES_REQUIRED \
	(PVRSGX_FEATURE_MULTIPLE_MEM_CONTEXTS | PVRSGX_FEATURE_MULTI_EVENT_KICK)

/**
 * struct pvrsgx_core_info - One SGX5xx core this driver supports
 *
 * A new core is a new entry: the register layout, the microkernel host
 * interface and the client heaps are the same across the family, what
 * differs is here.
 */
struct pvrsgx_core_info {
	/** @name: What the core is called in the log. */
	const char *name;
	/** @core_id: Expected SGX_CR_CORE_ID. */
	u32 core_id;
	/** @core_rev: Expected SGX_CR_CORE_REVISION. */
	u32 core_rev;
	/** @features: PVRSGX_FEATURE_*. */
	u32 features;
	/** @num_dir_lists: BIF directory lists (SGX_FEATURE_BIF_NUM_DIRLISTS). */
	u32 num_dir_lists;
	/** @clock_hz: Core clock, for the microkernel's timer. */
	u32 clock_hz;
	/** @fw_name: The open microkernel under /lib/firmware. */
	const char *fw_name;
};

const struct pvrsgx_core_info *pvrsgx_core_lookup(u32 core_id, u32 core_rev);

#endif /* PVRSGX_CORE_H */
