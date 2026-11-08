/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM powervr_sgx

#if !defined(PVRSGX_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define PVRSGX_TRACE_H

#include <linux/tracepoint.h>
#include <linux/types.h>

/* A command handed to the microkernel (kernel CCB, doorbell rung). */
TRACE_EVENT(pvrsgx_kick,
	TP_PROTO(u32 type, u64 seq, u64 fence_seqno),
	TP_ARGS(type, seq, fence_seqno),
	TP_STRUCT__entry(
		__field(u32, type)
		__field(u64, seq)
		__field(u64, fence_seqno)
	),
	TP_fast_assign(
		__entry->type = type;
		__entry->seq = seq;
		__entry->fence_seqno = fence_seqno;
	),
	TP_printk("type=%u ccb=%llu fence=%llu", __entry->type, __entry->seq,
		  __entry->fence_seqno)
);

/* A render's last kick: the dataset and the per-frame values it was given. */
TRACE_EVENT(pvrsgx_render_kick,
	TP_PROTO(u32 rtdata, u32 w, u32 h, u32 frame, u32 entry, u32 rgn_this,
		 u32 rgn_next, u32 cmd_off, u32 pb_desc, u32 te_screen,
		 u32 te_mtile0, u32 te_mtile1, u32 mt_tiles, u32 screen_size),
	TP_ARGS(rtdata, w, h, frame, entry, rgn_this, rgn_next, cmd_off, pb_desc,
		te_screen, te_mtile0, te_mtile1, mt_tiles, screen_size),
	TP_STRUCT__entry(
		__field(u32, rtdata)
		__field(u32, w)
		__field(u32, h)
		__field(u32, frame)
		__field(u32, entry)
		__field(u32, rgn_this)
		__field(u32, rgn_next)
		__field(u32, cmd_off)
		__field(u32, pb_desc)
		__field(u32, te_screen)
		__field(u32, te_mtile0)
		__field(u32, te_mtile1)
		__field(u32, mt_tiles)
		__field(u32, screen_size)
	),
	TP_fast_assign(
		__entry->rtdata = rtdata;
		__entry->w = w;
		__entry->h = h;
		__entry->frame = frame;
		__entry->entry = entry;
		__entry->rgn_this = rgn_this;
		__entry->rgn_next = rgn_next;
		__entry->cmd_off = cmd_off;
		__entry->pb_desc = pb_desc;
		__entry->te_screen = te_screen;
		__entry->te_mtile0 = te_mtile0;
		__entry->te_mtile1 = te_mtile1;
		__entry->mt_tiles = mt_tiles;
		__entry->screen_size = screen_size;
	),
	TP_printk("rtdata=0x%08x %ux%u frame=%u entry=0x%08x rgn=0x%08x next=0x%08x cmd_off=0x%x pb=0x%08x te_screen=0x%08x mtile=0x%08x,0x%08x mt_tiles=0x%08x screen=0x%08x",
		  __entry->rtdata, __entry->w, __entry->h, __entry->frame,
		  __entry->entry, __entry->rgn_this, __entry->rgn_next,
		  __entry->cmd_off, __entry->pb_desc, __entry->te_screen,
		  __entry->te_mtile0, __entry->te_mtile1, __entry->mt_tiles,
		  __entry->screen_size)
);

/* The microkernel's interrupt, in the hard handler. */
TRACE_EVENT(pvrsgx_irq,
	TP_PROTO(u32 status),
	TP_ARGS(status),
	TP_STRUCT__entry(
		__field(u32, status)
	),
	TP_fast_assign(
		__entry->status = status;
	),
	TP_printk("status=0x%08x", __entry->status)
);

/* A job seen done, just before its hardware fence signals. */
TRACE_EVENT(pvrsgx_job_done,
	TP_PROTO(u32 type, u64 fence_seqno),
	TP_ARGS(type, fence_seqno),
	TP_STRUCT__entry(
		__field(u32, type)
		__field(u64, fence_seqno)
	),
	TP_fast_assign(
		__entry->type = type;
		__entry->fence_seqno = fence_seqno;
	),
	TP_printk("type=%u fence=%llu", __entry->type, __entry->fence_seqno)
);

/* A job's imports flushed from the CPU caches. */
TRACE_EVENT(pvrsgx_flush_imports,
	TP_PROTO(u64 fence_seqno, u32 nimports, u64 bytes, u64 ns, bool after),
	TP_ARGS(fence_seqno, nimports, bytes, ns, after),
	TP_STRUCT__entry(
		__field(u64, fence_seqno)
		__field(u32, nimports)
		__field(u64, bytes)
		__field(u64, ns)
		__field(bool, after)
	),
	TP_fast_assign(
		__entry->fence_seqno = fence_seqno;
		__entry->nimports = nimports;
		__entry->bytes = bytes;
		__entry->ns = ns;
		__entry->after = after;
	),
	TP_printk("fence=%llu %s imports=%u bytes=%llu ns=%llu", __entry->fence_seqno,
		  __entry->after ? "after" : "before", __entry->nimports, __entry->bytes,
		  __entry->ns)
);

#endif /* PVRSGX_TRACE_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE pvrsgx_trace
#include <trace/define_trace.h>
