/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_REGS_H
#define PVRSGX_REGS_H

#include <linux/bits.h>
#include <linux/types.h>

/*
 * SGX545 core registers, BAR0 offset 0 on the CE5300. Names and fields follow
 * the GPL DDK's sgx545defs.h. Registers that header does not name are written
 * by offset in pvrsgx_fw.c, as docs/SRVINIT.md lists them.
 *
 * On a core nobody has initialised, only a few registers are safe to read:
 * CLKGATECTL, CLKGATESTATUS, SOFT_RESET, EVENT_STATUS, EVENT_HOST_ENABLE,
 * BIF_CTRL, CORE_ID and CORE_REVISION. Some others (USE_CODE_BASE_0) hang the
 * bus until the reset sequence has run.
 */
#define SGX_CR_CLKGATECTL			0x0000
#define SGX_CR_CLKGATECTL2			0x0004
#define SGX_CR_CLKGATESTATUS			0x0008
#define SGX_CR_CORE_ID				0x001c
#define SGX_CR_CORE_REVISION			0x0020

#define SGX_CR_SOFT_RESET			0x0080
#define   SGX_CR_SOFT_RESET_BIF			BIT(0)
#define   SGX_CR_SOFT_RESET_CORES		0x3e	/* DPM TA USE ISP TSP */

#define SGX_CR_EVENT_HOST_ENABLE2		0x0110
#define SGX_CR_EVENT_HOST_CLEAR2		0x0114
#define SGX_CR_EVENT_STATUS2			0x0118
#define SGX_CR_EVENT_STATUS			0x012c
#define   SGX_CR_EVENT_STATUS_MASTER_INTERRUPT	BIT(31)
#define   SGX_CR_EVENT_STATUS_TIMER		BIT(29)
#define   SGX_CR_EVENT_STATUS_SW_EVENT		BIT(14)
#define SGX_CR_EVENT_HOST_ENABLE		0x0130
#define SGX_CR_EVENT_HOST_CLEAR			0x0134
#define   SGX_CR_EVENT_HOST_CLEAR_MASTER_INTERRUPT BIT(31)
#define   SGX_CR_EVENT_HOST_CLEAR_SW_EVENT	BIT(14)
#define SGX_CR_TIMER				0x0144

#define SGX_CR_USE_CODE_BASE(n)			(0x0a0c + 4 * (n))
#define SGX_CR_PDS_EXEC_BASE			0x0ab8
#define SGX_CR_EVENT_KICKER			0x0ac4
#define SGX_CR_EVENT_KICK			0x0ac8
#define SGX_CR_EVENT_TIMER			0x0acc
#define SGX_CR_EVENT_KICK2			0x0ae8
#define   SGX_CR_EVENT_KICK_NOW			BIT(0)

#define SGX_CR_BIF_CTRL				0x0c00
#define   SGX_CR_BIF_CTRL_PAUSE			BIT(1)
#define   SGX_CR_BIF_CTRL_INVALDC		BIT(3)
#define   SGX_CR_BIF_CTRL_CLEAR_FAULT		BIT(4)
#define SGX_CR_BIF_INT_STAT			0x0c04
#define   SGX_CR_BIF_INT_STAT_FAULT_REQ		GENMASK(15, 0)
#define   SGX_CR_BIF_INT_STAT_FAULT_TYPE	GENMASK(18, 16)
#define SGX_CR_BIF_FAULT			0x0c08
/* Directory lists 1-15 are consecutive; 0 sits apart. */
#define SGX_CR_BIF_DIR_LIST_BASE0		0x0c84
#define SGX_CR_BIF_DIR_LIST_BASE1		0x0c38
#define SGX_CR_BIF_DIR_LIST_BASE15		0x0c70
#define SGX_CR_BIF_BANK_SET			0x0c74
#define SGX_CR_BIF_BANK0			0x0c78
#define   SGX_CR_BIF_BANK0_INDEX_EDM		GENMASK(3, 0)
#define   SGX_CR_BIF_BANK0_INDEX_TA		GENMASK(7, 4)
#define   SGX_CR_BIF_BANK0_INDEX_HOST		GENMASK(11, 8)
#define   SGX_CR_BIF_BANK0_INDEX_3D		GENMASK(15, 12)
#define   SGX_CR_BIF_BANK0_INDEX_2D		GENMASK(19, 16)
#define SGX_CR_BIF_MEM_REQ_STAT			0x0ca8
#define   SGX_CR_BIF_MEM_REQ_STAT_READS		GENMASK(10, 0)

#define SGX_BIF_NUM_DIRLISTS			16
/*
 * The microkernel translates through the last directory list; TA, 3D and
 * host requests through list 0, which the microkernel points at the job's
 * page directory.
 */
#define SGX_BIF_DIRLIST_EDM			(SGX_BIF_NUM_DIRLISTS - 1)

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

#endif /* PVRSGX_REGS_H */
