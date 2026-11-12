/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_HOST_H
#define PVRSGX_HOST_H

#include <linux/types.h>

struct pvrsgx_device;
struct pvrsgx_job;

/*
 * Host-kick: the CPU programs the TA and 3D cores itself, with no EDM
 * microkernel, on a single-context core whose EDM task can't retire writes
 * (docs/porting/cores/sgx530-host-kick.md). The entry points mirror the
 * microkernel's CCB commands; the per-pass register sequences are filled in
 * as the SGX530's TA/3D/DPM/ISP offsets are confirmed on hardware (gap 13).
 */

/* HOST-mode init: the reset and init script, without the event program or task. */
int pvrsgx_host_init(struct pvrsgx_device *sdev);

/* A render job (TA kicks then the 3D), driven from the CPU. */
int pvrsgx_host_render(struct pvrsgx_device *sdev, struct pvrsgx_job *job);

/* A transfer job (a 3D-only pass), driven from the CPU. */
int pvrsgx_host_transfer(struct pvrsgx_device *sdev, struct pvrsgx_job *job);

#endif /* PVRSGX_HOST_H */
