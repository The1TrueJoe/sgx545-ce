/* SPDX-License-Identifier: (GPL-2.0-only WITH Linux-syscall-note) OR MIT */
/* Copyright © 2026 Joseph Telaak */

/*
 * Design draft of the SGX545 DRM uAPI. docs/UAPI.md is the design document this
 * header belongs to; docs/uapi/check.sh compiles it for i386 and x86_64 against
 * the literal layouts in docs/uapi/layout.inc.
 *
 * Not a merged interface. Nothing includes this outside docs/uapi. The name
 * "sgx545" in every identifier is a working placeholder (UAPI.md, decision D1).
 */

#ifndef SGX545_DRM_UAPI_H
#define SGX545_DRM_UAPI_H

#include "drm.h"

#include <linux/types.h>

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * DOC: SGX545 UAPI
 *
 * The argument structs follow the rules of the upstream PowerVR uAPI:
 *
 *  - All members are type-aligned, on i386 as on x86_64.
 *  - Every struct is padded to 64-bit alignment.
 *  - Padding is explicit, in ``_padding_x`` members, where x is the member's
 *    offset in hexadecimal. Padding and reserved fields must be zero; the
 *    kernel always checks them and returns -EINVAL otherwise.
 *  - User pointers and device-virtual addresses are __u64. A device-virtual
 *    address whose upper 32 bits are not zero is rejected: the GPU's address
 *    words are 32 bits wide.
 *  - Structs reached through &struct drm_sgx545_obj_array or a stream length
 *    can grow at the end. A short object is zero-extended. In an object longer
 *    than the kernel knows, the unknown bytes must be zero.
 */

/**
 * struct drm_sgx545_obj_array - Container used to pass arrays of objects
 *
 * The stride versions the element type. A stride smaller than the kernel's
 * element size zero-extends each element; a larger one requires the unknown
 * bytes to be zero. When the kernel writes elements back it writes the smaller
 * of the two sizes.
 */
struct drm_sgx545_obj_array {
	/** @stride: Size of one element, in bytes. */
	__u32 stride;

	/** @count: Number of elements. */
	__u32 count;

	/** @array: User pointer to the elements. */
	__u64 array;
};

/**
 * DOC: SGX545 IOCTL interface
 *
 * Every ioctl below is valid on the render node and is marked DRM_RENDER_ALLOW.
 * The driver has no modeset or DRM-master ioctls.
 *
 * Common errors, in addition to the ones listed per ioctl:
 *
 *  - -EINVAL: a reserved field or flag is not zero, or an argument is invalid.
 *  - -ENOENT: a handle does not name a live object of the required type.
 *  - -EFAULT: a user pointer cannot be read or written.
 *  - -EIO: the GPU could not be recovered by a reset (device lost). Every later
 *    ioctl other than destroy and GEM close returns it too.
 */

/**
 * SGX545_IOCTL() - Build an SGX545 ioctl number
 * @_ioctl: Incrementing id of the ioctl. Added to %DRM_COMMAND_BASE.
 * @_mode: One of %DRM_IOW or %DRM_IOWR.
 * @_data: Middle of the args struct name, drm_sgx545_ioctl_<_data>_args.
 */
#define SGX545_IOCTL(_ioctl, _mode, _data) \
	_mode(DRM_COMMAND_BASE + (_ioctl), struct drm_sgx545_ioctl_##_data##_args)

#define DRM_IOCTL_SGX545_DEV_QUERY SGX545_IOCTL(0x00, DRM_IOWR, dev_query)
#define DRM_IOCTL_SGX545_CREATE_BO SGX545_IOCTL(0x01, DRM_IOWR, create_bo)
#define DRM_IOCTL_SGX545_GET_BO_MMAP_OFFSET SGX545_IOCTL(0x02, DRM_IOWR, get_bo_mmap_offset)
#define DRM_IOCTL_SGX545_CREATE_VM_CONTEXT SGX545_IOCTL(0x03, DRM_IOWR, create_vm_context)
#define DRM_IOCTL_SGX545_DESTROY_VM_CONTEXT SGX545_IOCTL(0x04, DRM_IOW, destroy_vm_context)
#define DRM_IOCTL_SGX545_VM_MAP SGX545_IOCTL(0x05, DRM_IOW, vm_map)
#define DRM_IOCTL_SGX545_VM_UNMAP SGX545_IOCTL(0x06, DRM_IOW, vm_unmap)
#define DRM_IOCTL_SGX545_CREATE_CONTEXT SGX545_IOCTL(0x07, DRM_IOWR, create_context)
#define DRM_IOCTL_SGX545_DESTROY_CONTEXT SGX545_IOCTL(0x08, DRM_IOW, destroy_context)
#define DRM_IOCTL_SGX545_GET_CONTEXT_STATE SGX545_IOCTL(0x09, DRM_IOWR, get_context_state)
#define DRM_IOCTL_SGX545_CREATE_RT_DATASET SGX545_IOCTL(0x0a, DRM_IOWR, create_rt_dataset)
#define DRM_IOCTL_SGX545_DESTROY_RT_DATASET SGX545_IOCTL(0x0b, DRM_IOW, destroy_rt_dataset)
#define DRM_IOCTL_SGX545_SUBMIT_JOBS SGX545_IOCTL(0x0c, DRM_IOWR, submit_jobs)

/**
 * DOC: SGX545 IOCTL DEV_QUERY interface
 */

/**
 * struct drm_sgx545_dev_query_gpu_info - Identity of the GPU and its firmware
 *
 * Fetched with %DRM_SGX545_DEV_QUERY_GPU_INFO_GET.
 */
struct drm_sgx545_dev_query_gpu_info {
	/** @core_id: EUR_CR_CORE_ID as read from the hardware. */
	__u32 core_id;

	/**
	 * @core_revision: EUR_CR_CORE_REVISION as read from the hardware.
	 * 0x1000e on the CE5300's revision 1.0.14.
	 */
	__u32 core_revision;

	/** @fw_interface_version: Host interface version of the loaded firmware. */
	__u32 fw_interface_version;

	/** @_padding_c: Reserved. This field must be zero. */
	__u32 _padding_c;
};

/**
 * struct drm_sgx545_dev_query_limits - Limits of the object-creating ioctls
 *
 * Fetched with %DRM_SGX545_DEV_QUERY_LIMITS_GET.
 */
struct drm_sgx545_dev_query_limits {
	/** @rt_max_width: Largest &drm_sgx545_ioctl_create_rt_dataset_args.width. */
	__u32 rt_max_width;

	/** @rt_max_height: Largest &drm_sgx545_ioctl_create_rt_dataset_args.height. */
	__u32 rt_max_height;

	/** @rt_max_samples: Largest &drm_sgx545_ioctl_create_rt_dataset_args.samples. */
	__u32 rt_max_samples;

	/**
	 * @pb_min_pages: Smallest
	 * &drm_sgx545_ioctl_create_context_args.param_buffer_pages.
	 */
	__u32 pb_min_pages;

	/**
	 * @pb_default_pages: Size used when
	 * &drm_sgx545_ioctl_create_context_args.param_buffer_pages is 0.
	 */
	__u32 pb_default_pages;

	/**
	 * @pb_max_pages: Largest
	 * &drm_sgx545_ioctl_create_context_args.param_buffer_pages.
	 */
	__u32 pb_max_pages;

	/** @max_ta_kicks: Largest &drm_sgx545_job.ta_kicks count. */
	__u32 max_ta_kicks;

	/** @_padding_1c: Reserved. This field must be zero. */
	__u32 _padding_1c;
};

/**
 * struct drm_sgx545_dev_query_quirks - Hardware fixes userspace has to know
 *
 * Fetched with %DRM_SGX545_DEV_QUERY_QUIRKS_GET.
 */
struct drm_sgx545_dev_query_quirks {
	/**
	 * @quirks: User pointer to a __u32 array of quirk numbers. The first
	 * @musthave_count entries are mandatory: a client that does not
	 * implement them must fail to initialise. The rest are opt-in.
	 */
	__u64 quirks;

	/** @count: Length of @quirks. Updated with the number of quirks known. */
	__u16 count;

	/** @musthave_count: Number of mandatory entries at the start of @quirks. */
	__u16 musthave_count;

	/** @_padding_c: Reserved. This field must be zero. */
	__u32 _padding_c;
};

/**
 * enum drm_sgx545_heap_id - Index into the array returned by
 * %DRM_SGX545_DEV_QUERY_HEAP_INFO_GET
 *
 * Append only. A heap a device does not have is returned with a size of zero.
 */
enum drm_sgx545_heap_id {
	/** @DRM_SGX545_HEAP_GENERAL: Vertices, indices, VDM streams, surfaces. */
	DRM_SGX545_HEAP_GENERAL = 0,
	/** @DRM_SGX545_HEAP_PDS_PIXEL: PDS programs referenced from state words. */
	DRM_SGX545_HEAP_PDS_PIXEL,
	/** @DRM_SGX545_HEAP_PDS_VERTEX: PDS programs referenced from VDM streams. */
	DRM_SGX545_HEAP_PDS_VERTEX,
	/** @DRM_SGX545_HEAP_USSE_PIXEL: USSE code run by pixel tasks. */
	DRM_SGX545_HEAP_USSE_PIXEL,
	/** @DRM_SGX545_HEAP_USSE_VERTEX: USSE code run by vertex and state tasks. */
	DRM_SGX545_HEAP_USSE_VERTEX,
	/** @DRM_SGX545_HEAP_TEXTURE: Texels. */
	DRM_SGX545_HEAP_TEXTURE,
	/** @DRM_SGX545_HEAP_COUNT: Number of heaps known to this header. */
	DRM_SGX545_HEAP_COUNT
};

/**
 * struct drm_sgx545_heap - One device-virtual heap
 *
 * Heaps are the same in every VM context of a device.
 */
struct drm_sgx545_heap {
	/** @base: Device-virtual address of the heap. */
	__u64 base;

	/** @size: Size in bytes the client can use. 0 if the heap is not present. */
	__u64 size;

	/** @flags: Always 0. */
	__u32 flags;

	/** @page_size_log2: Log2 of the device page size. */
	__u32 page_size_log2;

	/**
	 * @usse_base_index: First EUR_CR_USE_CODE_BASE register covering this
	 * heap, for the DOUTU words a client writes. 0 for a non-USSE heap.
	 */
	__u32 usse_base_index;

	/**
	 * @usse_window_log2: Log2 of the window each code base covers. 0 for a
	 * non-USSE heap.
	 */
	__u32 usse_window_log2;
};

/**
 * struct drm_sgx545_dev_query_heap_info - Array of heaps
 *
 * Fetched with %DRM_SGX545_DEV_QUERY_HEAP_INFO_GET.
 */
struct drm_sgx545_dev_query_heap_info {
	/**
	 * @heaps: Array of &struct drm_sgx545_heap, indexed by &enum
	 * drm_sgx545_heap_id. With a NULL array pointer, the stride and count
	 * are set to what the kernel knows and nothing else is written.
	 */
	struct drm_sgx545_obj_array heaps;
};

/**
 * enum drm_sgx545_dev_query - Values of &drm_sgx545_ioctl_dev_query_args.type
 *
 * Append only.
 */
enum drm_sgx545_dev_query {
	/** @DRM_SGX545_DEV_QUERY_GPU_INFO_GET: &struct drm_sgx545_dev_query_gpu_info. */
	DRM_SGX545_DEV_QUERY_GPU_INFO_GET = 0,
	/** @DRM_SGX545_DEV_QUERY_LIMITS_GET: &struct drm_sgx545_dev_query_limits. */
	DRM_SGX545_DEV_QUERY_LIMITS_GET,
	/** @DRM_SGX545_DEV_QUERY_QUIRKS_GET: &struct drm_sgx545_dev_query_quirks. */
	DRM_SGX545_DEV_QUERY_QUIRKS_GET,
	/** @DRM_SGX545_DEV_QUERY_HEAP_INFO_GET: &struct drm_sgx545_dev_query_heap_info. */
	DRM_SGX545_DEV_QUERY_HEAP_INFO_GET,
};

/**
 * struct drm_sgx545_ioctl_dev_query_args - Arguments for %DRM_IOCTL_SGX545_DEV_QUERY
 */
struct drm_sgx545_ioctl_dev_query_args {
	/** @type: [IN] Query, one of &enum drm_sgx545_dev_query. */
	__u32 type;

	/**
	 * @size: [IN/OUT] Size of the receiving struct. Set to the bytes
	 * written. With a NULL @pointer, set to the size the kernel knows.
	 */
	__u32 size;

	/** @pointer: [IN] User pointer to the receiving struct. */
	__u64 pointer;
};

/**
 * DOC: SGX545 IOCTL CREATE_BO interface
 *
 * Buffer objects are GEM objects backed by shmem and zeroed at allocation. PRIME
 * export and import use the DRM core ioctls.
 */

/**
 * DRM_SGX545_BO_ALLOW_CPU_ACCESS - Allow %DRM_IOCTL_SGX545_GET_BO_MMAP_OFFSET
 * on this object. Bits 1..63 are reserved.
 */
#define DRM_SGX545_BO_ALLOW_CPU_ACCESS (1ULL << 0)

/**
 * struct drm_sgx545_ioctl_create_bo_args - Arguments for %DRM_IOCTL_SGX545_CREATE_BO
 */
struct drm_sgx545_ioctl_create_bo_args {
	/** @size: [IN] Size in bytes. Must be page aligned and not zero. */
	__u64 size;

	/** @handle: [OUT] GEM handle of the new object. */
	__u32 handle;

	/** @_padding_c: Reserved. This field must be zero. */
	__u32 _padding_c;

	/** @flags: [IN] ``DRM_SGX545_BO_*`` flags. */
	__u64 flags;
};

/**
 * struct drm_sgx545_ioctl_get_bo_mmap_offset_args - Arguments for
 * %DRM_IOCTL_SGX545_GET_BO_MMAP_OFFSET
 */
struct drm_sgx545_ioctl_get_bo_mmap_offset_args {
	/** @handle: [IN] GEM handle. */
	__u32 handle;

	/** @_padding_4: Reserved. This field must be zero. */
	__u32 _padding_4;

	/** @offset: [OUT] Fake offset to pass to mmap() on the DRM file. */
	__u64 offset;
};

/**
 * DOC: SGX545 IOCTL VM context interfaces
 *
 * A VM context is one GPU page directory, managed with drm_gpuvm. The client
 * manages the address space: it picks the address of each mapping inside the
 * heaps %DRM_SGX545_DEV_QUERY_HEAP_INFO_GET returns. Addresses outside them
 * cannot be mapped: the kernel keeps the rest of the 4 GiB space for itself.
 */

/**
 * struct drm_sgx545_ioctl_create_vm_context_args - Arguments for
 * %DRM_IOCTL_SGX545_CREATE_VM_CONTEXT
 */
struct drm_sgx545_ioctl_create_vm_context_args {
	/** @handle: [OUT] Handle of the new VM context. */
	__u32 handle;

	/** @_padding_4: Reserved. This field must be zero. */
	__u32 _padding_4;
};

/**
 * struct drm_sgx545_ioctl_destroy_vm_context_args - Arguments for
 * %DRM_IOCTL_SGX545_DESTROY_VM_CONTEXT
 */
struct drm_sgx545_ioctl_destroy_vm_context_args {
	/** @handle: [IN] Handle of the VM context. */
	__u32 handle;

	/** @_padding_4: Reserved. This field must be zero. */
	__u32 _padding_4;
};

/**
 * struct drm_sgx545_ioctl_vm_map_args - Arguments for %DRM_IOCTL_SGX545_VM_MAP
 *
 * Returns -EINVAL when the range is not page aligned or does not fit inside one
 * heap, and -EEXIST when it overlaps an existing mapping.
 */
struct drm_sgx545_ioctl_vm_map_args {
	/** @vm_context_handle: [IN] VM context to map into. */
	__u32 vm_context_handle;

	/** @flags: [IN] Reserved, must be 0. */
	__u32 flags;

	/** @device_addr: [IN] Device-virtual address of the mapping. */
	__u64 device_addr;

	/** @handle: [IN] GEM handle of the object to map. */
	__u32 handle;

	/** @_padding_14: Reserved. This field must be zero. */
	__u32 _padding_14;

	/** @offset: [IN] Offset into the object, in bytes. */
	__u64 offset;

	/** @size: [IN] Size of the mapping, in bytes. */
	__u64 size;
};

/**
 * struct drm_sgx545_ioctl_vm_unmap_args - Arguments for %DRM_IOCTL_SGX545_VM_UNMAP
 *
 * The object's pages stay allocated until the GPU's translation caches have been
 * invalidated and every job submitted before the unmap has finished. A job
 * already running that still uses the range faults or reads stale data inside
 * this VM only.
 */
struct drm_sgx545_ioctl_vm_unmap_args {
	/** @vm_context_handle: [IN] VM context of the mapping. */
	__u32 vm_context_handle;

	/** @_padding_4: Reserved. This field must be zero. */
	__u32 _padding_4;

	/** @device_addr: [IN] Start of the mapping. */
	__u64 device_addr;

	/** @size: [IN] Size of the mapping. */
	__u64 size;
};

/**
 * DOC: SGX545 IOCTL CREATE_CONTEXT interface
 *
 * A context is one scheduling queue on the GPU. Its firmware-visible state (HW
 * context, command ring, parameter buffer, free list) belongs to the kernel and
 * is never mapped into the client's address space for CPU access.
 */

/**
 * enum drm_sgx545_ctx_type - Values of &drm_sgx545_ioctl_create_context_args.type
 */
enum drm_sgx545_ctx_type {
	/** @DRM_SGX545_CTX_TYPE_RENDER: Runs %DRM_SGX545_JOB_TYPE_RENDER jobs. */
	DRM_SGX545_CTX_TYPE_RENDER = 0,
	/** @DRM_SGX545_CTX_TYPE_TRANSFER: Runs %DRM_SGX545_JOB_TYPE_TRANSFER jobs. */
	DRM_SGX545_CTX_TYPE_TRANSFER,
};

/**
 * enum drm_sgx545_ctx_priority - Values of
 * &drm_sgx545_ioctl_create_context_args.priority
 */
enum drm_sgx545_ctx_priority {
	/** @DRM_SGX545_CTX_PRIORITY_LOW: Below normal. */
	DRM_SGX545_CTX_PRIORITY_LOW = -512,
	/** @DRM_SGX545_CTX_PRIORITY_NORMAL: Normal. */
	DRM_SGX545_CTX_PRIORITY_NORMAL = 0,
	/** @DRM_SGX545_CTX_PRIORITY_HIGH: Above normal. Requires CAP_SYS_NICE. */
	DRM_SGX545_CTX_PRIORITY_HIGH = 512,
};

/**
 * struct drm_sgx545_ioctl_create_context_args - Arguments for
 * %DRM_IOCTL_SGX545_CREATE_CONTEXT
 */
struct drm_sgx545_ioctl_create_context_args {
	/** @type: [IN] One of &enum drm_sgx545_ctx_type. */
	__u32 type;

	/** @flags: [IN] Reserved, must be 0. */
	__u32 flags;

	/** @priority: [IN] One of &enum drm_sgx545_ctx_priority. */
	__s32 priority;

	/** @vm_context_handle: [IN] VM context whose page directory the jobs use. */
	__u32 vm_context_handle;

	/**
	 * @param_buffer_pages: [IN] Parameter buffer size for a render context,
	 * in device pages, or 0 for the default in
	 * &drm_sgx545_dev_query_limits.pb_default_pages. Must be 0 for a transfer
	 * context.
	 */
	__u32 param_buffer_pages;

	/** @handle: [OUT] Handle of the new context. */
	__u32 handle;
};

/**
 * struct drm_sgx545_ioctl_destroy_context_args - Arguments for
 * %DRM_IOCTL_SGX545_DESTROY_CONTEXT
 */
struct drm_sgx545_ioctl_destroy_context_args {
	/** @handle: [IN] Handle of the context. */
	__u32 handle;

	/** @_padding_4: Reserved. This field must be zero. */
	__u32 _padding_4;
};

/**
 * enum drm_sgx545_context_state - Values of
 * &drm_sgx545_ioctl_get_context_state_args.state
 *
 * These correspond to the reset statuses of GL_ARB_robustness.
 */
enum drm_sgx545_context_state {
	/** @DRM_SGX545_CONTEXT_STATE_OK: No reset has affected this context. */
	DRM_SGX545_CONTEXT_STATE_OK = 0,
	/**
	 * @DRM_SGX545_CONTEXT_STATE_GUILTY: A job of this context hung the GPU
	 * or faulted. Its fence carries -ETIME or -EFAULT.
	 */
	DRM_SGX545_CONTEXT_STATE_GUILTY,
	/**
	 * @DRM_SGX545_CONTEXT_STATE_INNOCENT: A job of this context was running
	 * when another context's job reset the GPU. Its fence carries -ECANCELED.
	 */
	DRM_SGX545_CONTEXT_STATE_INNOCENT,
};

/**
 * struct drm_sgx545_ioctl_get_context_state_args - Arguments for
 * %DRM_IOCTL_SGX545_GET_CONTEXT_STATE
 *
 * A context that is not %DRM_SGX545_CONTEXT_STATE_OK fails every later submission
 * with -ECANCELED; the client destroys it and creates a new one. A context whose
 * jobs were only queued during a reset stays OK and its jobs run afterwards.
 */
struct drm_sgx545_ioctl_get_context_state_args {
	/** @handle: [IN] Handle of the context. */
	__u32 handle;

	/** @state: [OUT] One of &enum drm_sgx545_context_state. */
	__u32 state;

	/** @reset_count: [OUT] Number of GPU resets since the device was opened. */
	__u32 reset_count;

	/** @_padding_c: Reserved. This field must be zero. */
	__u32 _padding_c;
};

/**
 * DOC: SGX545 IOCTL CREATE_RT_DATASET interface
 *
 * An RT dataset is what tiling a target of a given size needs: the macrotile
 * numbers, region headers, tail pointers, background object block and firmware
 * render data. The kernel derives all of it from the size, allocates it and keeps
 * it. A client sees only the handle.
 */

/**
 * struct drm_sgx545_ioctl_create_rt_dataset_args - Arguments for
 * %DRM_IOCTL_SGX545_CREATE_RT_DATASET
 */
struct drm_sgx545_ioctl_create_rt_dataset_args {
	/** @context_handle: [IN] Render context that uses this dataset. */
	__u32 context_handle;

	/** @width: [IN] Target width in pixels. */
	__u32 width;

	/** @height: [IN] Target height in pixels. */
	__u32 height;

	/** @samples: [IN] Samples per pixel. Must be 1 in this version. */
	__u32 samples;

	/** @flags: [IN] Reserved, must be 0. */
	__u32 flags;

	/** @handle: [OUT] Handle of the new dataset. */
	__u32 handle;
};

/**
 * struct drm_sgx545_ioctl_destroy_rt_dataset_args - Arguments for
 * %DRM_IOCTL_SGX545_DESTROY_RT_DATASET
 */
struct drm_sgx545_ioctl_destroy_rt_dataset_args {
	/** @handle: [IN] Handle of the dataset. */
	__u32 handle;

	/** @_padding_4: Reserved. This field must be zero. */
	__u32 _padding_4;
};

/**
 * DOC: SGX545 command streams
 *
 * The structs below are the client's part of a GPU command. The kernel copies one
 * into kernel memory, checks it field by field and builds the firmware command
 * from it. docs/UAPI.md lists how every word of the firmware command is produced.
 *
 * Addresses in a stream are device-virtual addresses in the job's VM. A field the
 * kernel does not check is one whose value only reaches a hardware register or a
 * shader, with no effect outside the VM.
 */

/**
 * struct drm_sgx545_cmd_ta_kick - One tiling kick of a render job
 *
 * The first element of &drm_sgx545_job.ta_kicks starts the scene. The last one
 * ends it, and the kernel starts the 3D after it.
 */
struct drm_sgx545_cmd_ta_kick {
	/**
	 * @vdm_base: VDM control stream this kick runs. Must be 4-byte aligned
	 * and mapped in the job's VM.
	 */
	__u64 vdm_base;

	/**
	 * @ta_flags2: Bits 19, 22, 23 and 25 of the TA flags word. Any other
	 * bit is rejected.
	 */
	__u32 ta_flags2;

	/** @_padding_c: Reserved. This field must be zero. */
	__u32 _padding_c;
};

/**
 * struct drm_sgx545_cmd_render - The 3D part of a render job
 *
 * @drm_sgx545_job.cmd_stream of a %DRM_SGX545_JOB_TYPE_RENDER job.
 */
struct drm_sgx545_cmd_render {
	/**
	 * @depth_addr: Depth buffer, or 0. Required when @zls_control is not 0
	 * and must be mapped for the whole RT dataset.
	 */
	__u64 depth_addr;

	/**
	 * @stencil_addr: Stencil buffer, or 0. Required when @zls_control has
	 * bit 0 or 1 set, or @flags_3d has bit 8 set.
	 */
	__u64 stencil_addr;

	/**
	 * @pixel_event_pds_addr: Pixel event PDS program. 16-byte aligned, in
	 * %DRM_SGX545_HEAP_PDS_PIXEL.
	 */
	__u64 pixel_event_pds_addr;

	/**
	 * @bg_pds_secondary_addr: Background object's secondary PDS program.
	 * 16-byte aligned, in %DRM_SGX545_HEAP_PDS_PIXEL.
	 */
	__u64 bg_pds_secondary_addr;

	/**
	 * @bg_pds_primary_addr: Background object's primary PDS program.
	 * 16-byte aligned, in %DRM_SGX545_HEAP_PDS_PIXEL.
	 */
	__u64 bg_pds_primary_addr;

	/** @pixel_event_pds_rows: Data rows of the pixel event program, 1..63. */
	__u32 pixel_event_pds_rows;

	/** @bg_pds_secondary_rows: Data rows of the secondary program, 1..63. */
	__u32 bg_pds_secondary_rows;

	/** @bg_pds_primary_rows: Data rows of the primary program, 1..63. */
	__u32 bg_pds_primary_rows;

	/**
	 * @zls_control: ZLS control word. The kernel fills the tile count in
	 * bits 3..11. In the rest, only bits 0-2 and 12-20 are accepted.
	 */
	__u32 zls_control;

	/** @flags_3d: 3D flags. Only bits 7 and 8 are accepted. */
	__u32 flags_3d;

	/** @bg_depth: Depth every tile starts from, as IEEE-754 float bits. */
	__u32 bg_depth;

	/** @depth_bias_units: Depth bias units, rounded to an integer. */
	__u32 depth_bias_units;

	/** @depth_bias_factor: Depth bias slope factor, as IEEE-754 float bits. */
	__u32 depth_bias_factor;
};

/**
 * DRM_SGX545_TRANSFER_DST_TWIDDLED - The destination surface is twiddled.
 */
#define DRM_SGX545_TRANSFER_DST_TWIDDLED (1U << 0)

/**
 * struct drm_sgx545_cmd_transfer - A transfer: one 3D-only render
 *
 * @drm_sgx545_job.cmd_stream of a %DRM_SGX545_JOB_TYPE_TRANSFER job.
 */
struct drm_sgx545_cmd_transfer {
	/**
	 * @bg_block_addr: Background object block. 16-byte aligned and mapped
	 * for its 0x200 bytes.
	 */
	__u64 bg_block_addr;

	/**
	 * @pixel_event_pds_addr: Pixel event PDS program. 16-byte aligned, in
	 * %DRM_SGX545_HEAP_PDS_PIXEL.
	 */
	__u64 pixel_event_pds_addr;

	/** @pixel_event_pds_rows: Data rows of the pixel event program, 1..63. */
	__u32 pixel_event_pds_rows;

	/** @flags: ``DRM_SGX545_TRANSFER_*`` flags. */
	__u32 flags;

	/** @first_tile_x: First tile column, in 32-pixel tiles. */
	__u32 first_tile_x;

	/** @first_tile_y: First tile row, in 16-pixel tiles. */
	__u32 first_tile_y;

	/** @last_tile_x: Last tile column, at least @first_tile_x. */
	__u32 last_tile_x;

	/** @last_tile_y: Last tile row, at least @first_tile_y. */
	__u32 last_tile_y;

	/** @bg_depth: Background depth, as IEEE-754 float bits. */
	__u32 bg_depth;

	/** @_padding_2c: Reserved. This field must be zero. */
	__u32 _padding_2c;
};

/**
 * DOC: SGX545 IOCTL SUBMIT_JOBS interface
 *
 * The ioctl is all-or-nothing: when it fails, no job of the array was queued.
 * Jobs on one context run in submission order. Jobs on different contexts are
 * ordered only by the sync operations.
 *
 * A job has one out-fence, signalled from the completion interrupt when the whole
 * job is done: for a render job, when its 3D has finished. A job that does not
 * finish within the driver's timeout is killed and the GPU reset.
 */

/**
 * DOC: Flags for struct drm_sgx545_sync_op
 *
 * .. c:macro:: DRM_SGX545_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ
 *
 *    The handle is a binary syncobj. This is the default.
 *
 * .. c:macro:: DRM_SGX545_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ
 *
 *    The handle is a timeline syncobj and the value names a point on it.
 *
 * .. c:macro:: DRM_SGX545_SYNC_OP_FLAG_WAIT
 *
 *    The job waits for this syncobj before it starts. This is the default.
 *
 * .. c:macro:: DRM_SGX545_SYNC_OP_FLAG_SIGNAL
 *
 *    The job's out-fence is attached to this syncobj.
 */
#define DRM_SGX545_SYNC_OP_FLAG_HANDLE_TYPE_MASK 0xfU
#define DRM_SGX545_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ 0U
#define DRM_SGX545_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ 1U
#define DRM_SGX545_SYNC_OP_FLAG_WAIT 0U
#define DRM_SGX545_SYNC_OP_FLAG_SIGNAL (1U << 31)

/**
 * struct drm_sgx545_sync_op - One wait or signal of a job
 */
struct drm_sgx545_sync_op {
	/** @handle: Syncobj handle. */
	__u32 handle;

	/** @flags: ``DRM_SGX545_SYNC_OP_FLAG_*`` flags. */
	__u32 flags;

	/** @value: Timeline point. Must be 0 for a binary syncobj. */
	__u64 value;
};

/**
 * DOC: Flags for struct drm_sgx545_bo_ref
 *
 * .. c:macro:: DRM_SGX545_BO_REF_FLAG_WRITE
 *
 *    The job writes this object: the out-fence is added to its reservation
 *    object as a write fence and the job waits for all earlier fences on it.
 *    Without this flag the job reads it: the out-fence is added as a read fence
 *    and the job waits for earlier write fences.
 */
#define DRM_SGX545_BO_REF_FLAG_WRITE (1U << 0)

/**
 * struct drm_sgx545_bo_ref - An object a job takes part in implicit sync on
 *
 * Only objects shared with another client or process need an entry. An object
 * must be mapped in the job's VM to be useful, and that is not checked here.
 */
struct drm_sgx545_bo_ref {
	/** @handle: GEM handle. */
	__u32 handle;

	/** @flags: ``DRM_SGX545_BO_REF_FLAG_*`` flags. */
	__u32 flags;
};

/**
 * enum drm_sgx545_job_type - Values of &drm_sgx545_job.type
 */
enum drm_sgx545_job_type {
	/** @DRM_SGX545_JOB_TYPE_RENDER: A whole scene: TA kicks and the 3D. */
	DRM_SGX545_JOB_TYPE_RENDER = 0,
	/** @DRM_SGX545_JOB_TYPE_TRANSFER: One 3D-only blit. */
	DRM_SGX545_JOB_TYPE_TRANSFER,
};

/**
 * struct drm_sgx545_job - One job of %DRM_IOCTL_SGX545_SUBMIT_JOBS
 */
struct drm_sgx545_job {
	/** @type: One of &enum drm_sgx545_job_type. */
	__u32 type;

	/** @context_handle: Context of the matching type. */
	__u32 context_handle;

	/** @flags: Reserved, must be 0. */
	__u32 flags;

	/**
	 * @cmd_stream_len: Size of the object at @cmd_stream, in bytes. At most
	 * 4 KiB.
	 */
	__u32 cmd_stream_len;

	/**
	 * @cmd_stream: User pointer to &struct drm_sgx545_cmd_render or
	 * &struct drm_sgx545_cmd_transfer, by @type. 8-byte aligned.
	 */
	__u64 cmd_stream;

	/** @sync_ops: Array of &struct drm_sgx545_sync_op. May be empty. */
	struct drm_sgx545_obj_array sync_ops;

	/** @bo_refs: Array of &struct drm_sgx545_bo_ref. May be empty. */
	struct drm_sgx545_obj_array bo_refs;

	/**
	 * @ta_kicks: Array of &struct drm_sgx545_cmd_ta_kick. At least one
	 * element and at most &drm_sgx545_dev_query_limits.max_ta_kicks for a
	 * render job, empty for a transfer job.
	 */
	struct drm_sgx545_obj_array ta_kicks;

	/** @rt_dataset_handle: RT dataset of a render job, 0 for a transfer job. */
	__u32 rt_dataset_handle;

	/** @_padding_4c: Reserved. This field must be zero. */
	__u32 _padding_4c;
};

/**
 * struct drm_sgx545_ioctl_submit_jobs_args - Arguments for %DRM_IOCTL_SGX545_SUBMIT_JOBS
 *
 * Returns -ECANCELED when a job's context is not usable after a reset.
 */
struct drm_sgx545_ioctl_submit_jobs_args {
	/** @jobs: [IN] Array of &struct drm_sgx545_job. */
	struct drm_sgx545_obj_array jobs;

	/** @failed_index: [OUT] On error, the index of the job that caused it. */
	__u32 failed_index;

	/** @_padding_14: Reserved. This field must be zero. */
	__u32 _padding_14;
};

#if defined(__cplusplus)
}
#endif

#endif /* SGX545_DRM_UAPI_H */
