/* SPDX-License-Identifier: (GPL-2.0-only WITH Linux-syscall-note) OR MIT */
/* Copyright © 2026 Joseph Telaak */

#ifndef POWERVR_SGX_DRM_UAPI_H
#define POWERVR_SGX_DRM_UAPI_H

#include "drm.h"

#include <linux/const.h>
#include <linux/types.h>

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * DOC: PowerVR SGX UAPI
 *
 * The render-node interface of the powervr_sgx driver, as designed in
 * docs/UAPI.md. Every struct has the same layout on i386 and x86_64, every
 * reserved field must be zero, and arrays are passed as
 * &struct drm_powervr_sgx_obj_array so they can grow.
 *
 * This header carries the ioctls the driver implements so far. The job
 * streams (render and transfer commands) are added when the stream format of
 * docs/UAPI.md is settled; until then only %DRM_POWERVR_SGX_JOB_TYPE_NULL
 * jobs run.
 */

#define DRM_POWERVR_SGX_DEV_QUERY		0x00
#define DRM_POWERVR_SGX_CREATE_BO		0x01
#define DRM_POWERVR_SGX_GET_BO_MMAP_OFFSET	0x02
#define DRM_POWERVR_SGX_CREATE_VM_CONTEXT	0x03
#define DRM_POWERVR_SGX_DESTROY_VM_CONTEXT	0x04
#define DRM_POWERVR_SGX_VM_MAP			0x05
#define DRM_POWERVR_SGX_VM_UNMAP		0x06
#define DRM_POWERVR_SGX_CREATE_CONTEXT		0x07
#define DRM_POWERVR_SGX_DESTROY_CONTEXT		0x08
#define DRM_POWERVR_SGX_GET_CONTEXT_STATE	0x09
#define DRM_POWERVR_SGX_SUBMIT_JOBS		0x0c

#define POWERVR_SGX_IOCTL(_nr, _dir, _type) \
	_dir(DRM_COMMAND_BASE + DRM_POWERVR_SGX_##_nr, \
	     struct drm_powervr_sgx_ioctl_##_type##_args)

#define DRM_IOCTL_POWERVR_SGX_DEV_QUERY POWERVR_SGX_IOCTL(DEV_QUERY, DRM_IOWR, dev_query)
#define DRM_IOCTL_POWERVR_SGX_CREATE_BO POWERVR_SGX_IOCTL(CREATE_BO, DRM_IOWR, create_bo)
#define DRM_IOCTL_POWERVR_SGX_GET_BO_MMAP_OFFSET \
	POWERVR_SGX_IOCTL(GET_BO_MMAP_OFFSET, DRM_IOWR, get_bo_mmap_offset)
#define DRM_IOCTL_POWERVR_SGX_CREATE_VM_CONTEXT \
	POWERVR_SGX_IOCTL(CREATE_VM_CONTEXT, DRM_IOWR, create_vm_context)
#define DRM_IOCTL_POWERVR_SGX_DESTROY_VM_CONTEXT \
	POWERVR_SGX_IOCTL(DESTROY_VM_CONTEXT, DRM_IOW, destroy_vm_context)
#define DRM_IOCTL_POWERVR_SGX_VM_MAP POWERVR_SGX_IOCTL(VM_MAP, DRM_IOW, vm_map)
#define DRM_IOCTL_POWERVR_SGX_VM_UNMAP POWERVR_SGX_IOCTL(VM_UNMAP, DRM_IOW, vm_unmap)
#define DRM_IOCTL_POWERVR_SGX_CREATE_CONTEXT \
	POWERVR_SGX_IOCTL(CREATE_CONTEXT, DRM_IOWR, create_context)
#define DRM_IOCTL_POWERVR_SGX_DESTROY_CONTEXT \
	POWERVR_SGX_IOCTL(DESTROY_CONTEXT, DRM_IOW, destroy_context)
#define DRM_IOCTL_POWERVR_SGX_GET_CONTEXT_STATE \
	POWERVR_SGX_IOCTL(GET_CONTEXT_STATE, DRM_IOWR, get_context_state)
#define DRM_IOCTL_POWERVR_SGX_SUBMIT_JOBS POWERVR_SGX_IOCTL(SUBMIT_JOBS, DRM_IOWR, submit_jobs)

/**
 * struct drm_powervr_sgx_obj_array - Container for an array of structs
 *
 * The kernel copies min(@stride, its own struct size) bytes of each element.
 * Input elements longer than the kernel's struct must be zero beyond it.
 */
struct drm_powervr_sgx_obj_array {
	/** @stride: Size of each element in bytes. */
	__u32 stride;

	/** @count: Number of elements. */
	__u32 count;

	/** @array: User pointer to the first element. */
	__u64 array;
};

#define DRM_POWERVR_SGX_OBJ_ARRAY(cnt, ptr) \
	{ .stride = sizeof((ptr)[0]), .count = (cnt), .array = (__u64)(uintptr_t)(ptr) }

/**
 * DOC: DEV_QUERY
 *
 * &struct drm_powervr_sgx_ioctl_dev_query_args.pointer names a struct of the
 * type @type selects. The kernel writes min(@size, its size) bytes and sets
 * @size to its own size. A NULL pointer only returns the size.
 */

/**
 * enum drm_powervr_sgx_dev_query - Values of
 * &drm_powervr_sgx_ioctl_dev_query_args.type
 */
enum drm_powervr_sgx_dev_query {
	/** @DRM_POWERVR_SGX_DEV_QUERY_GPU_INFO_GET: &struct drm_powervr_sgx_dev_query_gpu_info */
	DRM_POWERVR_SGX_DEV_QUERY_GPU_INFO_GET = 0,
	/** @DRM_POWERVR_SGX_DEV_QUERY_LIMITS_GET: &struct drm_powervr_sgx_dev_query_limits */
	DRM_POWERVR_SGX_DEV_QUERY_LIMITS_GET,
	/** @DRM_POWERVR_SGX_DEV_QUERY_QUIRKS_GET: &struct drm_powervr_sgx_dev_query_quirks */
	DRM_POWERVR_SGX_DEV_QUERY_QUIRKS_GET,
	/** @DRM_POWERVR_SGX_DEV_QUERY_HEAP_INFO_GET: &struct drm_powervr_sgx_dev_query_heap_info */
	DRM_POWERVR_SGX_DEV_QUERY_HEAP_INFO_GET,
};

/**
 * struct drm_powervr_sgx_dev_query_gpu_info - GPU identity
 */
struct drm_powervr_sgx_dev_query_gpu_info {
	/** @core_id: EUR_CR_CORE_ID as read from the GPU. */
	__u32 core_id;

	/** @core_revision: EUR_CR_CORE_REVISION as read from the GPU. */
	__u32 core_revision;

	/**
	 * @fw_interface_version: Host interface of the running microkernel,
	 * one of ``DRM_POWERVR_SGX_FW_IF_*``.
	 */
	__u32 fw_interface_version;

	/** @_padding_c: Reserved, zero. */
	__u32 _padding_c;
};

/* The DDK 1.7 SGXMKIF host interface (docs/UKERNEL-DECISIONS.md). */
#define DRM_POWERVR_SGX_FW_IF_SGXMKIF_1_7	1

/**
 * struct drm_powervr_sgx_dev_query_limits - Size limits of the device
 */
struct drm_powervr_sgx_dev_query_limits {
	/** @rt_max_width: Widest render target, in pixels. */
	__u32 rt_max_width;
	/** @rt_max_height: Tallest render target, in pixels. */
	__u32 rt_max_height;
	/** @rt_max_samples: Most samples per pixel. */
	__u32 rt_max_samples;
	/** @pb_min_pages: Smallest parameter buffer a context can ask for. */
	__u32 pb_min_pages;
	/** @pb_default_pages: Parameter buffer size when a context asks for 0. */
	__u32 pb_default_pages;
	/** @pb_max_pages: Largest parameter buffer a context can ask for. */
	__u32 pb_max_pages;
	/** @max_ta_kicks: Most TA kicks in one render job. */
	__u32 max_ta_kicks;
	/** @_padding_1c: Reserved, zero. */
	__u32 _padding_1c;
};

/**
 * struct drm_powervr_sgx_dev_query_quirks - Hardware quirks userspace must know
 */
struct drm_powervr_sgx_dev_query_quirks {
	/** @quirks: User pointer to an array of __u32 quirk numbers. */
	__u64 quirks;
	/** @count: [IN] Room in @quirks; [OUT] number of quirks. */
	__u16 count;
	/** @musthave_count: [OUT] How many of the first quirks must be handled. */
	__u16 musthave_count;
	/** @_padding_c: Reserved, zero. */
	__u32 _padding_c;
};

/**
 * enum drm_powervr_sgx_heap_id - Index into the heap array
 *
 * Append only. A heap a device does not have is returned with a size of zero.
 */
enum drm_powervr_sgx_heap_id {
	/** @DRM_POWERVR_SGX_HEAP_GENERAL: Vertices, indices, VDM streams, surfaces. */
	DRM_POWERVR_SGX_HEAP_GENERAL = 0,
	/** @DRM_POWERVR_SGX_HEAP_PDS_PIXEL: PDS programs referenced from state words. */
	DRM_POWERVR_SGX_HEAP_PDS_PIXEL,
	/** @DRM_POWERVR_SGX_HEAP_PDS_VERTEX: PDS programs referenced from VDM streams. */
	DRM_POWERVR_SGX_HEAP_PDS_VERTEX,
	/** @DRM_POWERVR_SGX_HEAP_USSE_PIXEL: USSE code run by pixel tasks. */
	DRM_POWERVR_SGX_HEAP_USSE_PIXEL,
	/** @DRM_POWERVR_SGX_HEAP_USSE_VERTEX: USSE code run by vertex and state tasks. */
	DRM_POWERVR_SGX_HEAP_USSE_VERTEX,
	/** @DRM_POWERVR_SGX_HEAP_TEXTURE: Texels. */
	DRM_POWERVR_SGX_HEAP_TEXTURE,
	/** @DRM_POWERVR_SGX_HEAP_COUNT: Number of heaps this header knows. */
	DRM_POWERVR_SGX_HEAP_COUNT
};

/**
 * struct drm_powervr_sgx_heap - One device-virtual heap, the same in every VM
 */
struct drm_powervr_sgx_heap {
	/** @base: Device-virtual address of the heap. */
	__u64 base;
	/** @size: Bytes the client can use; 0 if the heap is not present. */
	__u64 size;
	/** @flags: Zero. */
	__u32 flags;
	/** @page_size_log2: Log2 of the device page size. */
	__u32 page_size_log2;
	/** @usse_base_index: First USE code base register of a USSE heap, else 0. */
	__u32 usse_base_index;
	/** @usse_window_log2: Log2 of each code base's window for a USSE heap, else 0. */
	__u32 usse_window_log2;
};

/**
 * struct drm_powervr_sgx_dev_query_heap_info - Heaps, indexed by
 * &enum drm_powervr_sgx_heap_id
 */
struct drm_powervr_sgx_dev_query_heap_info {
	/**
	 * @heaps: Array of &struct drm_powervr_sgx_heap. With a NULL array the
	 * kernel only sets the stride and count.
	 */
	struct drm_powervr_sgx_obj_array heaps;
};

/** struct drm_powervr_sgx_ioctl_dev_query_args - Arguments for DEV_QUERY */
struct drm_powervr_sgx_ioctl_dev_query_args {
	/** @type: [IN] One of &enum drm_powervr_sgx_dev_query. */
	__u32 type;
	/** @size: [IN/OUT] Size of the struct at @pointer. */
	__u32 size;
	/** @pointer: [IN] User pointer to the struct, or 0. */
	__u64 pointer;
};

/* CREATE_BO flags. A BO without ALLOW_CPU_ACCESS cannot be mmapped. */
#define DRM_POWERVR_SGX_BO_ALLOW_CPU_ACCESS	(1ULL << 0)
#define DRM_POWERVR_SGX_BO_FLAGS_MASK		DRM_POWERVR_SGX_BO_ALLOW_CPU_ACCESS

/** struct drm_powervr_sgx_ioctl_create_bo_args - Arguments for CREATE_BO */
struct drm_powervr_sgx_ioctl_create_bo_args {
	/** @size: [IN] Size in bytes, page aligned, not zero. */
	__u64 size;
	/** @handle: [OUT] GEM handle. */
	__u32 handle;
	/** @_padding_c: Reserved, zero. */
	__u32 _padding_c;
	/** @flags: [IN] ``DRM_POWERVR_SGX_BO_*`` flags. */
	__u64 flags;
};

/** struct drm_powervr_sgx_ioctl_get_bo_mmap_offset_args - Arguments for GET_BO_MMAP_OFFSET */
struct drm_powervr_sgx_ioctl_get_bo_mmap_offset_args {
	/** @handle: [IN] GEM handle. */
	__u32 handle;
	/** @_padding_4: Reserved, zero. */
	__u32 _padding_4;
	/** @offset: [OUT] Fake offset for mmap() on the DRM file. */
	__u64 offset;
};

/** struct drm_powervr_sgx_ioctl_create_vm_context_args - Arguments for CREATE_VM_CONTEXT */
struct drm_powervr_sgx_ioctl_create_vm_context_args {
	/** @handle: [OUT] Handle of the new VM context. */
	__u32 handle;
	/** @_padding_4: Reserved, zero. */
	__u32 _padding_4;
};

/** struct drm_powervr_sgx_ioctl_destroy_vm_context_args - Arguments for DESTROY_VM_CONTEXT */
struct drm_powervr_sgx_ioctl_destroy_vm_context_args {
	/** @handle: [IN] Handle of the VM context. */
	__u32 handle;
	/** @_padding_4: Reserved, zero. */
	__u32 _padding_4;
};

/**
 * struct drm_powervr_sgx_ioctl_vm_map_args - Arguments for VM_MAP
 *
 * -EINVAL when the range is empty, not page aligned, outside the object or not
 * inside one heap; -EEXIST when it overlaps a mapping.
 */
struct drm_powervr_sgx_ioctl_vm_map_args {
	/** @vm_context_handle: [IN] VM context to map into. */
	__u32 vm_context_handle;
	/** @flags: [IN] Reserved, zero. */
	__u32 flags;
	/** @device_addr: [IN] Device-virtual address of the mapping. */
	__u64 device_addr;
	/** @handle: [IN] GEM handle of the object. */
	__u32 handle;
	/** @_padding_14: Reserved, zero. */
	__u32 _padding_14;
	/** @offset: [IN] Offset into the object. */
	__u64 offset;
	/** @size: [IN] Size of the mapping. */
	__u64 size;
};

/**
 * struct drm_powervr_sgx_ioctl_vm_unmap_args - Arguments for VM_UNMAP
 *
 * Removes exactly one mapping made by VM_MAP (-ENOENT otherwise). Returns once
 * no job submitted before it can still use the range.
 */
struct drm_powervr_sgx_ioctl_vm_unmap_args {
	/** @vm_context_handle: [IN] VM context of the mapping. */
	__u32 vm_context_handle;
	/** @_padding_4: Reserved, zero. */
	__u32 _padding_4;
	/** @device_addr: [IN] Start of the mapping. */
	__u64 device_addr;
	/** @size: [IN] Size of the mapping. */
	__u64 size;
};

/** enum drm_powervr_sgx_ctx_type - Values of &drm_powervr_sgx_ioctl_create_context_args.type */
enum drm_powervr_sgx_ctx_type {
	/** @DRM_POWERVR_SGX_CTX_TYPE_RENDER: TA and 3D. */
	DRM_POWERVR_SGX_CTX_TYPE_RENDER = 0,
	/** @DRM_POWERVR_SGX_CTX_TYPE_TRANSFER: 3D-only blits. */
	DRM_POWERVR_SGX_CTX_TYPE_TRANSFER,
};

/** enum drm_powervr_sgx_ctx_priority - Values of &drm_powervr_sgx_ioctl_create_context_args.priority */
enum drm_powervr_sgx_ctx_priority {
	DRM_POWERVR_SGX_CTX_PRIORITY_LOW = -512,
	DRM_POWERVR_SGX_CTX_PRIORITY_NORMAL = 0,
	/** @DRM_POWERVR_SGX_CTX_PRIORITY_HIGH: Needs CAP_SYS_NICE or DRM master. */
	DRM_POWERVR_SGX_CTX_PRIORITY_HIGH = 512,
};

/** struct drm_powervr_sgx_ioctl_create_context_args - Arguments for CREATE_CONTEXT */
struct drm_powervr_sgx_ioctl_create_context_args {
	/** @type: [IN] One of &enum drm_powervr_sgx_ctx_type. */
	__u32 type;
	/** @flags: [IN] Reserved, zero. */
	__u32 flags;
	/** @priority: [IN] One of &enum drm_powervr_sgx_ctx_priority. */
	__s32 priority;
	/** @vm_context_handle: [IN] VM context the context's jobs run in. */
	__u32 vm_context_handle;
	/** @param_buffer_pages: [IN] Render only: PB size, 0 for the default. */
	__u32 param_buffer_pages;
	/** @handle: [OUT] Handle of the new context. */
	__u32 handle;
};

/** struct drm_powervr_sgx_ioctl_destroy_context_args - Arguments for DESTROY_CONTEXT */
struct drm_powervr_sgx_ioctl_destroy_context_args {
	/** @handle: [IN] Handle of the context. */
	__u32 handle;
	/** @_padding_4: Reserved, zero. */
	__u32 _padding_4;
};

/** enum drm_powervr_sgx_context_state - Values of &drm_powervr_sgx_ioctl_get_context_state_args.state */
enum drm_powervr_sgx_context_state {
	/** @DRM_POWERVR_SGX_CONTEXT_STATE_OK: No reset has affected the context. */
	DRM_POWERVR_SGX_CONTEXT_STATE_OK = 0,
	/** @DRM_POWERVR_SGX_CONTEXT_STATE_GUILTY: One of its jobs caused a reset. */
	DRM_POWERVR_SGX_CONTEXT_STATE_GUILTY,
	/** @DRM_POWERVR_SGX_CONTEXT_STATE_INNOCENT: A reset caused by another context lost its work. */
	DRM_POWERVR_SGX_CONTEXT_STATE_INNOCENT,
};

/** struct drm_powervr_sgx_ioctl_get_context_state_args - Arguments for GET_CONTEXT_STATE */
struct drm_powervr_sgx_ioctl_get_context_state_args {
	/** @handle: [IN] Handle of the context. */
	__u32 handle;
	/** @state: [OUT] One of &enum drm_powervr_sgx_context_state. */
	__u32 state;
	/** @reset_count: [OUT] Device resets since the driver loaded. */
	__u32 reset_count;
	/** @_padding_c: Reserved, zero. */
	__u32 _padding_c;
};

#define DRM_POWERVR_SGX_SYNC_OP_FLAG_HANDLE_TYPE_MASK		0xfU
#define DRM_POWERVR_SGX_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ	0U
#define DRM_POWERVR_SGX_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ 1U
#define DRM_POWERVR_SGX_SYNC_OP_FLAG_WAIT			0U
#define DRM_POWERVR_SGX_SYNC_OP_FLAG_SIGNAL			(1U << 31)
#define DRM_POWERVR_SGX_SYNC_OP_FLAGS_MASK \
	(DRM_POWERVR_SGX_SYNC_OP_FLAG_HANDLE_TYPE_MASK | DRM_POWERVR_SGX_SYNC_OP_FLAG_SIGNAL)

/** struct drm_powervr_sgx_sync_op - A syncobj to wait on before a job or signal after it */
struct drm_powervr_sgx_sync_op {
	/** @handle: Syncobj handle. */
	__u32 handle;
	/** @flags: ``DRM_POWERVR_SGX_SYNC_OP_FLAG_*``. */
	__u32 flags;
	/** @value: Timeline point; zero for a binary syncobj. */
	__u64 value;
};

#define DRM_POWERVR_SGX_BO_REF_FLAG_WRITE	(1U << 0)

/** struct drm_powervr_sgx_bo_ref - A shared object the job reads or writes, for implicit sync */
struct drm_powervr_sgx_bo_ref {
	/** @handle: GEM handle. */
	__u32 handle;
	/** @flags: ``DRM_POWERVR_SGX_BO_REF_FLAG_*``. */
	__u32 flags;
};

/** enum drm_powervr_sgx_job_type - Values of &drm_powervr_sgx_job.type */
enum drm_powervr_sgx_job_type {
	/** @DRM_POWERVR_SGX_JOB_TYPE_RENDER: A whole scene; stream format pending. */
	DRM_POWERVR_SGX_JOB_TYPE_RENDER = 0,
	/** @DRM_POWERVR_SGX_JOB_TYPE_TRANSFER: A blit; stream format pending. */
	DRM_POWERVR_SGX_JOB_TYPE_TRANSFER,
	/**
	 * @DRM_POWERVR_SGX_JOB_TYPE_NULL: No GPU work. The firmware takes it in
	 * queue order with the context's other jobs and the out-fence signals
	 * once it has. For testing synchronisation; takes no stream.
	 */
	DRM_POWERVR_SGX_JOB_TYPE_NULL,
};

/** struct drm_powervr_sgx_job - One job of SUBMIT_JOBS */
struct drm_powervr_sgx_job {
	/** @type: One of &enum drm_powervr_sgx_job_type. */
	__u32 type;
	/** @context_handle: Context to run on. */
	__u32 context_handle;
	/** @flags: Reserved, zero. */
	__u32 flags;
	/** @cmd_stream_len: Bytes at @cmd_stream; zero for a NULL job. */
	__u32 cmd_stream_len;
	/** @cmd_stream: User pointer to the command stream. */
	__u64 cmd_stream;
	/** @sync_ops: Array of &struct drm_powervr_sgx_sync_op. */
	struct drm_powervr_sgx_obj_array sync_ops;
	/** @bo_refs: Array of &struct drm_powervr_sgx_bo_ref. */
	struct drm_powervr_sgx_obj_array bo_refs;
	/** @ta_kicks: Render only: the scene's TA kicks; empty otherwise. */
	struct drm_powervr_sgx_obj_array ta_kicks;
	/** @rt_dataset_handle: Render only; zero otherwise. */
	__u32 rt_dataset_handle;
	/** @_padding_4c: Reserved, zero. */
	__u32 _padding_4c;
};

/**
 * struct drm_powervr_sgx_ioctl_submit_jobs_args - Arguments for SUBMIT_JOBS
 *
 * All or nothing: on error no job was queued and @failed_index names the job
 * that caused it.
 */
struct drm_powervr_sgx_ioctl_submit_jobs_args {
	/** @jobs: [IN] Array of &struct drm_powervr_sgx_job. */
	struct drm_powervr_sgx_obj_array jobs;
	/** @failed_index: [OUT] Index of the failing job, or ~0. */
	__u32 failed_index;
	/** @_padding_14: Reserved, zero. */
	__u32 _padding_14;
};

#if defined(__cplusplus)
}
#endif

#endif /* POWERVR_SGX_DRM_UAPI_H */
