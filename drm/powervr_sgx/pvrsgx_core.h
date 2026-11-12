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
/* The PDS data store interleaves DS0 and DS1 two words at a time (docs/PDS.md). */
#define PVRSGX_FEATURE_PDS_DATA_INTERLEAVE_2DWORDS	BIT(2)
/*
 * The EDM reaches PDS code anywhere. Without it, the kernel code and PDS
 * vertex heaps lie within 64 MiB above the PDS pixel heap (PDS_EXEC_BASE):
 * the GPL DDK's sgxconfig.h refuses any other layout.
 */
#define PVRSGX_FEATURE_EDM_VERTEX_PDSADDR_FULL_RANGE	BIT(3)

/* Errata the kernel works around (the GPL DDK's FIX_HW_BRN_*). */
/* A BIF directory or table invalidate and a data invalidate never in one command. */
#define PVRSGX_QUIRK_BRN_28889			BIT(0)

/* The USSE instruction set, for the shader validator's tables. */
enum pvrsgx_isa {
	PVRSGX_ISA_SGX545,
};

/* A device-virtual range, [base, end). */
struct pvrsgx_va_range {
	u64 base;
	u64 end;
};

/*
 * struct pvrsgx_heap_layout - Where every heap sits (docs/UAPI.md section 5)
 *
 * Device-wide, the same in every VM. The kernel heaps (kernel code, kernel
 * data, sync) are whole 4 MiB directory entries.
 */
struct pvrsgx_heap_layout {
	struct pvrsgx_va_range params_3d;
	struct pvrsgx_va_range ta_data;
	struct pvrsgx_va_range sync;
	struct pvrsgx_va_range pds_pixel;
	struct pvrsgx_va_range kernel_code;
	struct pvrsgx_va_range pds_vertex;
	struct pvrsgx_va_range kernel_data;
	struct pvrsgx_va_range usse_pixel;
	struct pvrsgx_va_range usse_vertex;
	struct pvrsgx_va_range general;
	struct pvrsgx_va_range texture;
	/* The largest PB, in pages, the 3D parameters heap holds. */
	u32 pb_max_pages;
};

/*
 * struct pvrsgx_core_regs - Registers that move or change between SGX5xx cores
 *
 * The other registers in pvrsgx_regs.h sit at the same offsets with the same
 * fields on SGX530, 535, 540, 544 and 545 (the GPL DDK's sgx5xxdefs.h). A
 * register a core lacks is 0, and the driver never writes or reads it. A core
 * the driver can only identify has just @core_id and @core_revision.
 */
struct pvrsgx_core_regs {
	u32 core_id;
	u32 core_revision;
	u32 clkgatectl_units;		/* every gating field of CLKGATECTL */
	u32 clkgatectl_dpm;		/* CLKGATECTL's DPM field */
	u32 clkgatectl2;
	u32 clkgatectl2_units;
	u32 clkgatestatus;
	u32 clkgatestatus_dpm;
	u32 soft_reset_bif;
	u32 soft_reset_cores;		/* every unit's reset but the BIF's */
	u32 event_kick2;
	u32 event_kick2_now;
	u32 use_code_base0;
	u32 pds_exec_base;
	u32 bif_int_stat_fault_req;
	u32 bif_int_stat_fault_type;
	u32 bif_fault_addr;		/* BIF_FAULT's address field */
	u32 bif_bank_set;
	u32 bif_bank0;
	u32 use_code_base_dm_shift;	/* USE_CODE_BASE's data-master field */
	u32 vendor_b30;			/* unnamed, 0x100 in the SGX545's vendor script */
	u32 pds_pc_base;		/* PDS_PC_BASE (SGX530), likely the PDS's PC */
	u32 pds_ctrl;			/* PDS (DOUT_TIMEOUT_DISABLE b6), read only */
	/* Not in the GPL header (pvrsgx_hw.h). */
	u32 event_pds_enable;
	u32 event_pds_enable_all;
	u32 event_pds_base;
	u32 event_pds_data0;
	u32 event_pds_data1;
	u32 edm_task_reg0;
	u32 edm_task_reg1;
};

/* One write of the vendor init script (docs/SRVINIT.md, "Init scripts"). */
struct pvrsgx_core_init_reg {
	u16 reg;
	u32 val;
};

/**
 * struct pvrsgx_core_family - What one SGX5xx core design has in common
 *
 * Every revision and part of the core shares these. The bus says which core
 * to expect, since even SGX_CR_CORE_ID moves between cores.
 */
struct pvrsgx_core_family {
	/** @name: The core, as "SGX545". */
	const char *name;
	/** @core: Its number, as the bus glue names it: 545 for SGX545. */
	unsigned int core;
	/** @core_id: Expected SGX_CR_CORE_ID. */
	u32 core_id;
	/** @regs: Where its registers and fields are. */
	const struct pvrsgx_core_regs *regs;
	/** @features: PVRSGX_FEATURE_*. */
	u32 features;
	/** @isa: Its USSE instruction set. */
	enum pvrsgx_isa isa;
	/** @num_dir_lists: BIF directory lists (SGX_FEATURE_BIF_NUM_DIRLISTS). */
	u32 num_dir_lists;
	/** @va_bits: Device-virtual address bits (SGX_FEATURE_ADDRESS_SPACE_SIZE). */
	u32 va_bits;
	/** @heaps: The device-virtual layout, within @va_bits. */
	const struct pvrsgx_heap_layout *heaps;
	/** @quirks: PVRSGX_QUIRK_*. */
	u32 quirks;
	/**
	 * @event_pds_guessed: The event PDS registers are a guess, not yet
	 * confirmed on this core: read them once before the first write, and
	 * refuse to kick if the program address doesn't read back.
	 */
	bool event_pds_guessed;
	/**
	 * @ukernel_temps: Temporary registers the microkernel's entry DOUTU
	 * asks for (bits 24-31, in fours). 0 keeps pvrsgx_doutu()'s 32.
	 */
	u8 ukernel_temps;
	/** @init_regs: Init script part 2's register writes, in order. */
	const struct pvrsgx_core_init_reg *init_regs;
	/** @num_init_regs: Entries in @init_regs. */
	unsigned int num_init_regs;
};

/**
 * struct pvrsgx_core_info - One SGX5xx part this driver supports
 *
 * A new part of a known core is a new entry; a new core is a new
 * &struct pvrsgx_core_family as well. The microkernel host interface and the
 * client heaps are the same across the family.
 */
struct pvrsgx_core_info {
	/** @family: The core design. */
	const struct pvrsgx_core_family *family;
	/** @name: What the part is called in the log. */
	const char *name;
	/** @core_rev: Expected SGX_CR_CORE_REVISION. */
	u32 core_rev;
	/** @clock_hz: Core clock, for the microkernel's timer. */
	u32 clock_hz;
	/** @fw_name: The open microkernel under /lib/firmware. */
	const char *fw_name;
};

const struct pvrsgx_core_family *pvrsgx_core_family_get(unsigned int core);
bool pvrsgx_core_family_usable(const struct pvrsgx_core_family *f);
u16 pvrsgx_core_uapi_quirks(const struct pvrsgx_core_family *f, const u32 **quirks);

/*
 * One memory context: the microkernel, TA, 3D and 2D all translate through
 * directory list 0, so the firmware heaps are in every VM (docs/porting/
 * single-context.md). Clients aren't isolated from the firmware or from
 * each other's commands.
 */
static inline bool pvrsgx_core_single_context(const struct pvrsgx_core_family *f)
{
	return !(f->features & PVRSGX_FEATURE_MULTIPLE_MEM_CONTEXTS);
}

extern bool pvrsgx_allow_unisolated;
const struct pvrsgx_core_info *pvrsgx_core_lookup(const struct pvrsgx_core_family *family,
						  u32 core_id, u32 core_rev);

#endif /* PVRSGX_CORE_H */
