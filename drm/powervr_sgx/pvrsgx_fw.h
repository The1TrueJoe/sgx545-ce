/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_FW_H
#define PVRSGX_FW_H

#include <linux/bits.h>
#include <linux/types.h>

struct pvrsgx_device;
struct seq_buf;

/* Fixed device-virtual layout (docs/UAPI.md section 5). */
#define PVRSGX_HEAP_3D_PARAMS		0x10000000u
#define PVRSGX_HEAP_TA_DATA		0x20000000u
#define PVRSGX_HEAP_SYNC		0x2d000000u
#define PVRSGX_HEAP_PDS_PIXEL		0x2e000000u
#define PVRSGX_HEAP_KERNEL_CODE		0x30000000u
#define PVRSGX_HEAP_PDS_VERTEX		0x30400000u
#define PVRSGX_HEAP_KERNEL_DATA		0x32000000u
#define PVRSGX_HEAP_USSE_PIXEL		0x37000000u
#define PVRSGX_HEAP_USSE_PIXEL_SIZE	0x00500000u
#define PVRSGX_HEAP_USSE_VERTEX		0x3c000000u
#define PVRSGX_HEAP_USSE_VERTEX_SIZE	0x00200000u
#define PVRSGX_HEAP_GENERAL		0x3e000000u
#define PVRSGX_HEAP_TEXTURE		0xf0000000u

/* Each USE code base register covers a 512 KiB window. */
#define PVRSGX_USSE_WINDOW_LOG2		19
#define PVRSGX_USSE_PIXEL_BASE_INDEX	0
#define PVRSGX_USSE_VERTEX_BASE_INDEX	10
#define PVRSGX_USSE_UKERNEL_BASE_INDEX	15

/* Host control block words (docs/UKERNEL.md section 1, "The TA3D control block"). */
#define HOSTCTL_INIT_STATUS		0
#define HOSTCTL_POWER_STATUS		1
#define HOSTCTL_CLEANUP_STATUS		2
#define HOSTCTL_UK_LOCKUPS		3
#define HOSTCTL_HOST_LOCKUPS		4
#define HOSTCTL_HWR_SAMPLE_RATE		5
#define HOSTCTL_UK_TIMER_CLOCK		6
#define HOSTCTL_APM_SAMPLE_RATE		7
#define HOSTCTL_INTERRUPT_FLAGS		8
#define HOSTCTL_INTERRUPT_CLEAR		9
#define HOSTCTL_HOST_CLOCK		13
#define HOSTCTL_ASSERT_FAIL		14

#define HOSTCTL_FLAGS			15	/* UKERNEL.md 4a */

/* HOSTCTL_FLAGS: the host writes the background object's PDS words. */
#define HOSTCTL_HOST_BGBLOCK		BIT(0)

/* Feature bits of the open microkernel's file header (docs/UKERNEL.md 4a). */
#define PVRSGX_UKFW_CLEANUP_V2		BIT(0)	/* accepted, unused */
#define PVRSGX_UKFW_SECURE		BIT(1)

#define HOSTCTL_INIT_COMPLETE		BIT(0)
#define HOSTCTL_POWMAN_POWEROFF_COMPLETE BIT(3)
#define HOSTCTL_POWMAN_NO_WORK		BIT(5)
#define HOSTCTL_INTERRUPT_HWR		BIT(0)
#define HOSTCTL_INTERRUPT_ACTIVE_POWER	BIT(1)

int pvrsgx_fw_init(struct pvrsgx_device *sdev);
void pvrsgx_fw_fini(struct pvrsgx_device *sdev);
int pvrsgx_fw_start(struct pvrsgx_device *sdev);
void pvrsgx_fw_stop(struct pvrsgx_device *sdev);
u32 pvrsgx_fw_hostctl(struct pvrsgx_device *sdev, unsigned int word);
void pvrsgx_fw_hostctl_set(struct pvrsgx_device *sdev, unsigned int word, u32 v);
void pvrsgx_fw_dump(struct seq_buf *sb, struct pvrsgx_device *sdev, unsigned int b,
		    const char *name);

#endif /* PVRSGX_FW_H */
