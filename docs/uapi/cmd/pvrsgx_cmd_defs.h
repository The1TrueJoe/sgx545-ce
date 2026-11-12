/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */

/* The per-core client-command tables, generated from the command description. */

#ifndef PVRSGX_CMD_DEFS_H
#define PVRSGX_CMD_DEFS_H

#include "pvrsgx_cmd.h"

enum pvrsgx_cmd_type {
	PVRSGX_CMD_RENDER,
	PVRSGX_CMD_TA_KICK,
	PVRSGX_CMD_TRANSFER,
	PVRSGX_CMD_COUNT,
};

/* A command with every extension it can take on any core: absent ones zero. */
struct pvrsgx_cmd_render_all {
	struct drm_powervr_sgx_cmd_render base;
	struct drm_powervr_sgx_ext_sgx545_render sgx545_render;
};

struct pvrsgx_cmd_ta_kick_all {
	struct drm_powervr_sgx_cmd_ta_kick base;
	struct drm_powervr_sgx_ext_sgx545_ta_kick sgx545_ta_kick;
};

struct pvrsgx_cmd_transfer_all {
	struct drm_powervr_sgx_cmd_transfer base;
};

/* Extension name -> 1 + the command it belongs to, 0 for none. */
extern const u8 pvrsgx_cmd_ext_owner[64];

extern const struct pvrsgx_cmd_core pvrsgx_cmd_core_sgx545;
extern const struct pvrsgx_cmd_core pvrsgx_cmd_core_sgx530;

#endif /* PVRSGX_CMD_DEFS_H */
