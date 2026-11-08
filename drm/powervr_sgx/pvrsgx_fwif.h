/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_FWIF_H
#define PVRSGX_FWIF_H

#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/stddef.h>
#include <linux/types.h>

/*
 * Structures the host writes and the microkernel reads, in the microkernel's
 * layout (docs/STATE.md, docs/UKERNEL.md). The SGX and its x86 hosts are both
 * little-endian, so the fields are plain u32. Fields the driver leaves zero
 * are padding here; the docs list them.
 */

/* struct pvrsgx_fw_ta_cmd.ta_flags */
#define PVRSGX_TA_FIRST_KICK		BIT(0)
#define PVRSGX_TA_LAST_KICK		BIT(1)
#define PVRSGX_TA_FIRST_RENDER		BIT(4)	/* the RT dataset's first render */
#define PVRSGX_TA_FLAGS_BASE		0x60	/* set on every kick */

/* struct pvrsgx_fw_ta_cmd.ta_flags2 */
#define PVRSGX_TA2_RENDER		BIT(22)	/* this kick starts the 3D */

/* struct pvrsgx_fw_ta_cmd.flags_3d */
#define PVRSGX_3D_RENDER		BIT(10)
#define PVRSGX_3D_FRAME_SHIFT		14

/**
 * struct pvrsgx_fw_ta_cmd - One TA kick (docs/STATE.md section 1)
 *
 * The header and tail come from the client's streams and the kernel's own
 * structures; the shared part carries the synchronisation.
 */
struct pvrsgx_fw_ta_cmd {
	/** @size: The command's size in bytes. */
	u32 size;
	/** @ta_flags: PVRSGX_TA_*. */
	u32 ta_flags;
	/** @bg_pds: Background object PDS words: secondary, middle, primary. */
	u32 bg_pds[3];
	/** @ta_ctl: Two constant control words. */
	u32 ta_ctl[2];
	/** @flags_3d: 3D flags, on the scene's last kick. */
	u32 flags_3d;
	/** @frame: The RT dataset's frame counter. */
	u32 frame;
	/** @pb_desc: Device address of the parameter buffer descriptor. */
	u32 pb_desc;
	/* private: */
	u32 pad_28[4];
	/* public: */
	/** @render_details: Device address of the render details. */
	u32 render_details;
	/** @rt_deps: Device address of the render-target dependency block. */
	u32 rt_deps;
	/** @rt_data: Device address of the RT data. */
	u32 rt_data;
	/** @rt_data_entry: Device address of this render's RT data entry. */
	u32 rt_data_entry;
	/* private: */
	u32 pad_48[4];
	/* public: */

	/** @ready: Set when the command may run. */
	u32 ready;
	/* private: */
	u32 pad_5c[6];
	/* public: */
	/** @sync3d_write_pending: The 3D sync's write-ops-pending value. */
	u32 sync3d_write_pending;
	/** @sync3d_write_complete: Its write-ops-complete address. */
	u32 sync3d_write_complete;
	/** @sync3d_read_pending: The 3D sync's read-ops-pending value. */
	u32 sync3d_read_pending;
	/** @sync3d_read_complete: Its read-ops-complete address. */
	u32 sync3d_read_complete;
	/* private: */
	u32 pad_84[109];
	/* public: */

	/** @te_aa: Tiling engine anti-aliasing word. */
	u32 te_aa;
	/** @te_mtile: Macrotile boundaries in X and Y. */
	u32 te_mtile[2];
	/** @te_screen: Last tile in X (bits 0-11) and Y (bits 12 on). */
	u32 te_screen;
	/** @mt_tiles: Tiles per macrotile. */
	u32 mt_tiles;
	/** @ta_flags2: PVRSGX_TA2_* and the client's kick flags. */
	u32 ta_flags2;
	/** @vdm_base: Device address of the kick's VDM control stream. */
	u32 vdm_base;
	/** @vdm_ctl: Constant word next to it. */
	u32 vdm_ctl;
	/** @region_headers: This render's region header array. */
	u32 region_headers;
	/** @tail_pointers: The tail pointer array. */
	u32 tail_pointers;
	/** @eps: Two floats, 1e-6. */
	u32 eps[2];
	/** @screen_size: (height - 1) << 16 | (width - 1). */
	u32 screen_size;
	/* private: */
	u32 pad_26c[2];
	/* public: */
	/** @params_base: Base of the 3D parameters heap the PB is in. */
	u32 params_base;
	/* private: */
	u32 pad_278[11];
	/* public: */
	/** @rt_flags: RT data flags. */
	u32 rt_flags;
	/* private: */
	u32 pad_2a8[6];
};

static_assert(offsetof(struct pvrsgx_fw_ta_cmd, ready) == 0x58);
static_assert(offsetof(struct pvrsgx_fw_ta_cmd, te_aa) == 0x238);
static_assert(offsetof(struct pvrsgx_fw_ta_cmd, params_base) == 0x274);
static_assert(offsetof(struct pvrsgx_fw_ta_cmd, rt_flags) == 0x2a4);
static_assert(sizeof(struct pvrsgx_fw_ta_cmd) == 0x2c0);

/**
 * struct pvrsgx_fw_rt_deps - A render target's dependency block
 *
 * What the 3D signals when it finishes: one destination sync, whose
 * write-ops-complete the microkernel sets to the pending value plus one.
 */
struct pvrsgx_fw_rt_deps {
	/** @access: Device address of the access word. */
	u32 access;
	/** @num_dst: Destination syncs, 1. */
	u32 num_dst;
	/** @read_pending: The destination's read-ops-pending value. */
	u32 read_pending;
	/** @read_complete: Its read-ops-complete address. */
	u32 read_complete;
	/** @write_pending: Its write-ops-pending value. */
	u32 write_pending;
	/** @write_complete: Its write-ops-complete address. */
	u32 write_complete;
	/* private: */
	u32 pad_18[4];
};

static_assert(sizeof(struct pvrsgx_fw_rt_deps) == 0x28);

#endif /* PVRSGX_FWIF_H */
