/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_CMD_CHECKS_H
#define PVRSGX_CMD_CHECKS_H

#include "pvrsgx_cmd_defs.h"

int pvrsgx_check_render(const struct pvrsgx_cmd_core *core, const struct pvrsgx_cmd_render_all *r,
			u32 samples);
int pvrsgx_check_transfer(const struct pvrsgx_cmd_transfer_all *t);

#endif /* PVRSGX_CMD_CHECKS_H */
