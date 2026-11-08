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
