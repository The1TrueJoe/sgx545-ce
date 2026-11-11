/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_DUMP_H
#define PVRSGX_DUMP_H

struct pvrsgx_device;
struct pvrsgx_job;
struct seq_buf;

void pvrsgx_dump_status(struct seq_buf *sb, struct pvrsgx_device *sdev, bool regs);
void pvrsgx_coredump(struct pvrsgx_device *sdev, struct pvrsgx_job *bad);

#endif /* PVRSGX_DUMP_H */
