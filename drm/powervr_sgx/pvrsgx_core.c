// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * The SGX5xx cores this driver knows. A core that is not listed is refused
 * before any register write: on an unknown core the driver can't tell which
 * register writes are safe.
 */

#include <linux/kernel.h>
#include <linux/module.h>

#include "pvrsgx_core.h"

static const struct pvrsgx_core_info cores[] = {
	{
		.name = "Intel CE5300 (SGX545 1.0.14)",
		.core_id = 0x01150000,
		.core_rev = 0x0001000e,
		.features = PVRSGX_FEATURE_MULTIPLE_MEM_CONTEXTS |
			    PVRSGX_FEATURE_MULTI_EVENT_KICK,
		.isa = PVRSGX_ISA_SGX545,
		.num_dir_lists = 16,
		.clock_hz = 400000000,
		.fw_name = "powervr_sgx/sgx545_ukernel.bin",
	},
	{
		/*
		 * The same core one revision earlier (docs/UKERNEL.md, "SGX545
		 * core revisions"): same features and instruction set, the same
		 * microkernel. Its clock depends on the part (400 or 640 MHz);
		 * the microkernel's timer only needs the order of magnitude.
		 */
		.name = "Intel Cedarview (SGX545 1.0.13)",
		.core_id = 0x01150000,
		.core_rev = 0x0001000d,
		.features = PVRSGX_FEATURE_MULTIPLE_MEM_CONTEXTS |
			    PVRSGX_FEATURE_MULTI_EVENT_KICK,
		.isa = PVRSGX_ISA_SGX545,
		.num_dir_lists = 16,
		.clock_hz = 400000000,
		.fw_name = "powervr_sgx/sgx545_ukernel.bin",
	},
};

MODULE_FIRMWARE("powervr_sgx/sgx545_ukernel.bin");

/* The entry for this core, or NULL if the driver doesn't know it or can't drive it. */
const struct pvrsgx_core_info *pvrsgx_core_lookup(u32 core_id, u32 core_rev)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(cores); i++) {
		const struct pvrsgx_core_info *c = &cores[i];

		if (c->core_id == core_id && c->core_rev == core_rev &&
		    (c->features & PVRSGX_FEATURES_REQUIRED) == PVRSGX_FEATURES_REQUIRED)
			return c;
	}
	return NULL;
}
