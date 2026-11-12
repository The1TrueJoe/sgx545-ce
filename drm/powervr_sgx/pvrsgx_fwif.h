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

/**
 * struct pvrsgx_fw_ta3d_ctl - The microkernel's control block
 *
 * docs/UKERNEL.md, "The TA3D control block". The host fills the addresses;
 * the rest is the microkernel's live state, zero at every start.
 */
struct pvrsgx_fw_ta3d_ctl {
	/** @self: Device address of this block. */
	u32 self;
	/** @host_ctl: Device address of the host control block. */
	u32 host_ctl;
	/** @ccb_ctl: Device address of the kernel CCB's write and read offsets. */
	u32 ccb_ctl;
	/* private: */
	u32 pad_0c[19];
	/* public: */
	/** @ccb: Device address of the kernel CCB. */
	u32 ccb;
	/** @kernel_pd: Physical address of the firmware's page directory. */
	u32 kernel_pd;
	/* private: the microkernel's own state, to the block's end (word 0x3f) */
	u32 uk_60[40];
};

static_assert(offsetof(struct pvrsgx_fw_ta3d_ctl, ccb) == 0x58);
static_assert(sizeof(struct pvrsgx_fw_ta3d_ctl) == 0x100);

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

/**
 * struct pvrsgx_fw_rt_entry - One of an RT dataset's two render slots
 *
 * Frames alternate between the two (docs/STATE.md section 6, "RT data").
 */
struct pvrsgx_fw_rt_entry {
	/** @hw_render_ctx: Device address of the HW render context. */
	u32 hw_render_ctx;
	/** @render_details: The render in flight; the microkernel's. */
	u32 render_details;
	/** @mt_units: Tiles per macrotile >> 4. */
	u32 mt_units;
	/** @mt_bytes: Tiles per macrotile * 0xc. */
	u32 mt_bytes;
	/** @bg_block: Device address of the background object block. */
	u32 bg_block;
	/** @ta_data: The 0x88-, 0x40- and 0x50-byte TA data blocks. */
	u32 ta_data[3];
	/** @tail_pointers: The tail pointer array. */
	u32 tail_pointers;
	/** @tail_pointers_size: Its size in bytes. */
	u32 tail_pointers_size;
	/** @num_rts: Render targets, 1. */
	u32 num_rts;
	/* private: */
	u32 pad_2c;
	/* public: */
	/** @entry_word: Device address of the slot's status word. */
	u32 entry_word;
	/** @unk_34: 0x10. */
	u32 unk_34;
};

static_assert(sizeof(struct pvrsgx_fw_rt_entry) == 0x38);

/**
 * struct pvrsgx_fw_rt_data - An RT dataset's RT data
 */
struct pvrsgx_fw_rt_data {
	/** @rt_word: Device address of the dataset's status word. */
	u32 rt_word;
	/* private: */
	u32 pad_04;
	/* public: */
	/** @padded_tiles_x: Tiles in X, padded to whole macrotiles. */
	u32 padded_tiles_x;
	/* private: */
	u32 pad_0c[2];
	/* public: */
	/** @num_entries: Render slots, 2. */
	u32 num_entries;
	/** @entry: The render slots. */
	struct pvrsgx_fw_rt_entry entry[2];
};

static_assert(sizeof(struct pvrsgx_fw_rt_data) == 0x88);

/**
 * struct pvrsgx_fw_pb_desc - Parameter buffer descriptor
 *
 * docs/STATE.md section 6. Page numbers count from the 3D parameters heap.
 */
struct pvrsgx_fw_pb_desc {
	/* private: */
	u32 pad_00;
	/* public: */
	/** @pages: Pages in the PB. */
	u32 pages;
	/** @free_head: Free list head: last page << 16 | first page. */
	u32 free_head;
	/** @free_tail: (last page - 1) << 16. */
	u32 free_tail;
	/** @free_list: Device address of the free list. */
	u32 free_list;
	/* private: */
	u32 pad_14;
	/* public: */
	/** @free_head_copy: The free list head again. */
	u32 free_head_copy;
	/** @free_tail_copy: The tail word again. */
	u32 free_tail_copy;
	/* private: */
	u32 pad_20[2];
	/* public: */
	/** @ta_threshold: Pages the TA may use: pages + 0x10. */
	u32 ta_threshold;
	/** @zls_threshold: Pages for depth/stencil load/store: the request. */
	u32 zls_threshold;
	/** @global_threshold: The request. */
	u32 global_threshold;
	/** @pds_threshold: min(ZLS, TA) - 0x100. */
	u32 pds_threshold;
	/** @blocks: Device address of the PB block list. */
	u32 blocks;
};

static_assert(sizeof(struct pvrsgx_fw_pb_desc) == 0x3c);

/**
 * struct pvrsgx_fw_render_details - Per-dataset render details
 *
 * docs/STATE.md section 6. Fields named unk_* hold values captured from the
 * vendor stack whose meaning isn't known.
 */
struct pvrsgx_fw_render_details {
	/** @req_base_3d: 3D request base: the PB's 1 MiB-aligned base. */
	u32 req_base_3d;
	/** @zls_base: Depth/stencil request base, 1 MiB aligned. */
	u32 zls_base;
	/** @unk_08: 0x80. */
	u32 unk_08;
	/** @unk_0c: 0xf0000. */
	u32 unk_0c;
	/** @rt_ctl: Bit 31. */
	u32 rt_ctl;
	/** @depth_bias_units: glPolygonOffset units. */
	u32 depth_bias_units;
	/** @depth_bias_factor: glPolygonOffset factor, a float. */
	u32 depth_bias_factor;
	/* private: */
	u32 pad_1c;
	/* public: */
	/** @region_headers: This render's region header array. */
	u32 region_headers;
	/* private: */
	u32 pad_24;
	/* public: */
	/** @region_headers_next: The next render's. */
	u32 region_headers_next;
	/** @zls_ctl: Depth/stencil load and store control. */
	u32 zls_ctl;
	/** @depth: Depth buffer address - @zls_base, twice. */
	u32 depth[2];
	/** @stencil: Bit 31 | stencil buffer address - @zls_base, twice. */
	u32 stencil[2];
	/** @bg_depth: Depth every tile starts from, a float. */
	u32 bg_depth;
	/** @unk_44: 0x300. */
	u32 unk_44;
	/** @bg_object: 0x2000000 | (background block - PB base) >> 4. */
	u32 bg_object;
	/* private: */
	u32 pad_4c;
	/* public: */
	/** @rt_flags: RT flags, 0x88. */
	u32 rt_flags;
	/* private: */
	u32 pad_54;
	/* public: */
	/** @unk_58: 0x30. */
	u32 unk_58;
	/** @pixel_event_pds: End-of-tile PDS program. */
	u32 pixel_event_pds;
	/** @pixel_event_rows: Its data rows. */
	u32 pixel_event_rows;
	/** @unk_64: 1. */
	u32 unk_64;
	/** @unk_68: 1. */
	u32 unk_68;
	/* private: */
	u32 pad_6c[24];
	/* public: */
	/** @unk_cc: 0x100. */
	u32 unk_cc;
	/** @sync: Device address of the render-details sync word. */
	u32 sync;
	/* private: */
	u32 pad_d4;
	/* public: */
	/** @rt_data: Device address of the RT data. */
	u32 rt_data;
	/** @rt_data_entry: This render's RT data entry. */
	u32 rt_data_entry;
	/* private: */
	u32 pad_e0[9];
	/* public: */
	/** @unk_104: 1. */
	u32 unk_104;
	/* private: */
	u32 pad_108[2];
	/* public: */
	/** @word8: Device address of an 8-byte kernel word. */
	u32 word8;
	/** @bg_pds_word: Device address of the background object's primary PDS word. */
	u32 bg_pds_word;
	/* private: */
	u32 pad_118[9];
	/* public: */
	/** @unk_13c: 0x7c00000. */
	u32 unk_13c;
	/* private: */
	u32 pad_140[43];
};

static_assert(offsetof(struct pvrsgx_fw_render_details, unk_cc) == 0xcc);
static_assert(offsetof(struct pvrsgx_fw_render_details, unk_104) == 0x104);
static_assert(offsetof(struct pvrsgx_fw_render_details, unk_13c) == 0x13c);
static_assert(sizeof(struct pvrsgx_fw_render_details) == 0x1ec);

/* struct pvrsgx_fw_hw_render_ctx.flags */
#define PVRSGX_HW_RENDER_CTX_VALID	BIT(0)
#define PVRSGX_HW_RENDER_CTX_OWN_PB	BIT(5)

/**
 * struct pvrsgx_fw_hw_render_ctx - A render context, as the microkernel sees it
 */
struct pvrsgx_fw_hw_render_ctx {
	/** @flags: PVRSGX_HW_RENDER_CTX_*. */
	u32 flags;
	/* private: */
	u32 pad_04;
	/* public: */
	/** @page_dir: Physical address of the context's page directory. */
	u32 page_dir;
	/** @ccb: Device address of the context's command ring. */
	u32 ccb;
	/** @ccb_ctl: Device address of its write and read offsets. */
	u32 ccb_ctl;
	/* private: */
	u32 pad_14;
	/* public: */
	/** @pb_desc: Device address of the PB descriptor. */
	u32 pb_desc;
	/** @scratch: Device address of the microkernel's per-context scratch block. */
	u32 scratch;
	/* private: */
	u32 pad_20[8];
	/* public: */
	/** @pid: The creating process. */
	u32 pid;
};

static_assert(sizeof(struct pvrsgx_fw_hw_render_ctx) == 0x44);

/**
 * struct pvrsgx_fw_hw_transfer_ctx - A transfer context, as the microkernel sees it
 */
struct pvrsgx_fw_hw_transfer_ctx {
	/** @flags: 1. */
	u32 flags;
	/* private: */
	u32 pad_04;
	/* public: */
	/** @page_dir: Physical address of the context's page directory. */
	u32 page_dir;
	/** @ccb: Device address of the context's command ring. */
	u32 ccb;
	/** @ccb_ctl: Device address of its write and read offsets. */
	u32 ccb_ctl;
	/* private: */
	u32 pad_14[37];
	/* public: */
	/** @pid: The creating process. */
	u32 pid;
	/* private: */
	u32 pad_ac[2];
};

static_assert(offsetof(struct pvrsgx_fw_hw_transfer_ctx, pid) == 0xa8);
static_assert(sizeof(struct pvrsgx_fw_hw_transfer_ctx) == 0xb4);

/* struct pvrsgx_fw_transfer_cmd.flags */
#define PVRSGX_TRANSFER_DST_TWIDDLED	BIT(3)

/**
 * struct pvrsgx_fw_transfer_cmd - One transfer: a 3D render of a background object
 *
 * docs/TEX.md sections 3 and 10. The first words are 3D registers in the
 * render details' encoding; the shared part carries the destination sync.
 */
struct pvrsgx_fw_transfer_cmd {
	/** @bg_object: 0x2000000 | (background block - @req_base_3d) >> 4. */
	u32 bg_object;
	/** @unk_04: 0x200. */
	u32 unk_04;
	/** @bg_depth: Depth every tile starts from, a float. */
	u32 bg_depth;
	/** @unk_0c: 3. */
	u32 unk_0c;
	/* private: */
	u32 pad_10;
	/* public: */
	/** @unk_14: 0x100. */
	u32 unk_14;
	/** @req_base_3d: 3D request base, 1 MiB aligned. */
	u32 req_base_3d;
	/** @rt_ctl: Bit 31. */
	u32 rt_ctl;
	/** @rt_flags: 0x88. */
	u32 rt_flags;
	/** @pixel_event_pds: End-of-tile PDS program. */
	u32 pixel_event_pds;
	/** @pixel_event_rows: Its data rows. */
	u32 pixel_event_rows;
	/** @unk_2c: 1. */
	u32 unk_2c;
	/** @first_tile: First tile, x << 16 | y. */
	u32 first_tile;
	/** @last_tile: Last tile, x << 16 | y. */
	u32 last_tile;
	/** @unk_38: 0x80. */
	u32 unk_38;
	/** @pds_exec_base: PDS execution base: the pixel PDS heap. */
	u32 pds_exec_base;
	/* private: */
	u32 pad_40[25];
	/* public: */
	/** @size: The command's size in bytes. */
	u32 size;
	/** @flags: PVRSGX_TRANSFER_*. */
	u32 flags;
	/* private: */
	u32 pad_ac[33];
	/* public: */
	/** @num_dst: Destination syncs, 1. */
	u32 num_dst;
	/** @dst_read_pending: The destination sync's read-ops-pending value. */
	u32 dst_read_pending;
	/** @dst_read_complete: Its read-ops-complete address. */
	u32 dst_read_complete;
	/** @dst_write_pending: Its write-ops-pending value. */
	u32 dst_write_pending;
	/** @dst_write_complete: Its write-ops-complete address. */
	u32 dst_write_complete;
	/* private: */
	u32 pad_144[20];
	/* public: */
	/** @unk_194: 0x7c00000. */
	u32 unk_194;
	/* private: */
	u32 pad_198[10];
};

static_assert(offsetof(struct pvrsgx_fw_transfer_cmd, size) == 0xa4);
static_assert(offsetof(struct pvrsgx_fw_transfer_cmd, num_dst) == 0x130);
static_assert(offsetof(struct pvrsgx_fw_transfer_cmd, unk_194) == 0x194);
static_assert(sizeof(struct pvrsgx_fw_transfer_cmd) == 0x1c0);

#endif /* PVRSGX_FWIF_H */
