/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_HW_H
#define PVRSGX_HW_H

#include <linux/bits.h>
#include <linux/types.h>

#include "pvrsgx_regs.h"

/* Gating modes of the 2-bit CLKGATECTL/CLKGATECTL2 fields. */
#define SGX_CLKG_ON				1
#define SGX_CLKG_AUTO				2
/* The same mode in every 2-bit field of mask. */
#define SGX_CLKG_ALL(mask, mode)		((mask) / 3 * (mode))

/*
 * SGX545 registers not in the GPL header; the names are the driver's, from
 * what the microkernel and the init script do with them. Code reaches them
 * through the core table.
 */
#define SGX545_CR_EVENT_PDS_ENABLE		0x0a58	/* host events start the event program */
#define   SGX545_CR_EVENT_PDS_ENABLE_ALL	0x2204b00f
#define SGX545_CR_EVENT_PDS_BASE		0x0a68	/* the event PDS program, 16-byte aligned */
#define SGX545_CR_EVENT_PDS_DATA0		0x0a6c
#define SGX545_CR_EVENT_PDS_DATA1		0x0a70
#define SGX545_CR_EDM_TASK_REG0			0x0b14	/* where the microkernel is */
#define SGX545_CR_EDM_TASK_REG1			0x0b20

/* Directory lists 1 and up are consecutive; 0 sits apart. */
#define SGX_BIF_MAX_DIRLISTS			16

static inline u32 sgx_cr_bif_dir_list_base(unsigned int n)
{
	return n ? SGX_CR_BIF_DIR_LIST_BASE1 + 4 * (n - 1) : SGX_CR_BIF_DIR_LIST_BASE0;
}

/* MMU: two levels, 4 KiB pages, 32-bit device addresses and entries. */
#define SGX_MMU_PAGE_SHIFT			12
#define SGX_MMU_PAGE_SIZE			BIT(SGX_MMU_PAGE_SHIFT)
#define SGX_MMU_PT_SHIFT			10
#define SGX_MMU_PD_ENTRIES			1024
#define SGX_MMU_PT_ENTRIES			1024
#define SGX_MMU_PDE_SPAN			BIT(SGX_MMU_PAGE_SHIFT + SGX_MMU_PT_SHIFT)
#define SGX_MMU_ADDR_MASK			GENMASK(31, 12)
#define SGX_MMU_PDE_VALID			BIT(0)
#define SGX_MMU_PDE_PAGE_SIZE_4K		0
#define SGX_MMU_PTE_VALID			BIT(0)
#define SGX_MMU_PTE_WRITEONLY			BIT(1)
#define SGX_MMU_PTE_READONLY			BIT(2)
#define SGX_MMU_PTE_CACHECONSISTENT		BIT(3)
#define SGX_MMU_PTE_EDMPROTECT			BIT(4)

#endif /* PVRSGX_HW_H */
