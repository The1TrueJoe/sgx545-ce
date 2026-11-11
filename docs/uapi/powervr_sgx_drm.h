/* SPDX-License-Identifier: (GPL-2.0-only WITH Linux-syscall-note) OR MIT */
/* Copyright © 2026 Joseph Telaak */

#ifndef POWERVR_SGX_DRM_H
#define POWERVR_SGX_DRM_H

#include "drm.h"

#include <linux/const.h>
#include <linux/types.h>

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * DOC: PowerVR SGX UAPI
 *
 * The DRM driver is ``powervr_sgx``. Identifiers use the same name in full,
 * ``drm_powervr_sgx_`` and ``DRM_IOCTL_POWERVR_SGX_``, because ``pvr`` is the
 * upstream Rogue driver's prefix.
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
 *  - Structs reached through &struct drm_powervr_sgx_obj_array can grow at the
 *    end. A short object is zero-extended. In an object longer than the kernel
 *    knows, the unknown bytes must be zero.
 *  - Command streams are not structs and are not defined in this header: see
 *    "PowerVR SGX command streams" below.
 */

/**
 * struct drm_powervr_sgx_obj_array - Container used to pass arrays of objects
 *
 * The stride versions the element type. A stride smaller than the kernel's
 * element size zero-extends each element; a larger one requires the unknown
 * bytes to be zero. When the kernel writes elements back it writes the smaller
 * of the two sizes.
 */
struct drm_powervr_sgx_obj_array {
	/** @stride: Size of one element, in bytes. */
	__u32 stride;

	/** @count: Number of elements. */
	__u32 count;

	/** @array: User pointer to the elements. */
	__u64 array;
};

/**
 * DOC: PowerVR SGX IOCTL interface
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
 * PVR_SGX_IOCTL() - Build a powervr_sgx ioctl number
 * @_ioctl: Incrementing id of the ioctl. Added to %DRM_COMMAND_BASE.
 * @_mode: One of %DRM_IOW or %DRM_IOWR.
 * @_data: Middle of the args struct name, drm_powervr_sgx_ioctl_<_data>_args.
 */
#define PVR_SGX_IOCTL(_ioctl, _mode, _data) \
	_mode(DRM_COMMAND_BASE + (_ioctl), struct drm_powervr_sgx_ioctl_##_data##_args)

#define DRM_IOCTL_POWERVR_SGX_DEV_QUERY PVR_SGX_IOCTL(0x00, DRM_IOWR, dev_query)
#define DRM_IOCTL_POWERVR_SGX_CREATE_BO PVR_SGX_IOCTL(0x01, DRM_IOWR, create_bo)
#define DRM_IOCTL_POWERVR_SGX_GET_BO_MMAP_OFFSET PVR_SGX_IOCTL(0x02, DRM_IOWR, get_bo_mmap_offset)
#define DRM_IOCTL_POWERVR_SGX_CREATE_VM_CONTEXT PVR_SGX_IOCTL(0x03, DRM_IOWR, create_vm_context)
#define DRM_IOCTL_POWERVR_SGX_DESTROY_VM_CONTEXT PVR_SGX_IOCTL(0x04, DRM_IOW, destroy_vm_context)
#define DRM_IOCTL_POWERVR_SGX_VM_MAP PVR_SGX_IOCTL(0x05, DRM_IOW, vm_map)
#define DRM_IOCTL_POWERVR_SGX_VM_UNMAP PVR_SGX_IOCTL(0x06, DRM_IOW, vm_unmap)
#define DRM_IOCTL_POWERVR_SGX_CREATE_CONTEXT PVR_SGX_IOCTL(0x07, DRM_IOWR, create_context)
#define DRM_IOCTL_POWERVR_SGX_DESTROY_CONTEXT PVR_SGX_IOCTL(0x08, DRM_IOW, destroy_context)
#define DRM_IOCTL_POWERVR_SGX_GET_CONTEXT_STATE PVR_SGX_IOCTL(0x09, DRM_IOWR, get_context_state)
#define DRM_IOCTL_POWERVR_SGX_CREATE_RT_DATASET PVR_SGX_IOCTL(0x0a, DRM_IOWR, create_rt_dataset)
#define DRM_IOCTL_POWERVR_SGX_DESTROY_RT_DATASET PVR_SGX_IOCTL(0x0b, DRM_IOW, destroy_rt_dataset)
#define DRM_IOCTL_POWERVR_SGX_SUBMIT_JOBS PVR_SGX_IOCTL(0x0c, DRM_IOWR, submit_jobs)
#define DRM_IOCTL_POWERVR_SGX_UPLOAD_CODE PVR_SGX_IOCTL(0x0d, DRM_IOWR, upload_code)

/**
 * DOC: PowerVR SGX IOCTL DEV_QUERY interface
 */

/**
 * struct drm_powervr_sgx_dev_query_gpu_info - Identity of the GPU and its firmware
 *
 * Fetched with %DRM_POWERVR_SGX_DEV_QUERY_GPU_INFO_GET.
 */
struct drm_powervr_sgx_dev_query_gpu_info {
	/** @core_id: EUR_CR_CORE_ID as read from the hardware. */
	__u32 core_id;

	/**
	 * @core_revision: EUR_CR_CORE_REVISION as read from the hardware.
	 * 0x1000e on the CE5300's revision 1.0.14.
	 */
	__u32 core_revision;

	/**
	 * @fw_interface_version: Host interface of the loaded firmware, one of
	 * ``DRM_POWERVR_SGX_FW_IF_*``.
	 */
	__u32 fw_interface_version;

	/** @_padding_c: Reserved. This field must be zero. */
	__u32 _padding_c;
};

/* The DDK 1.7 SGXMKIF host interface (D8, docs/UKERNEL-DECISIONS.md). */
#define DRM_POWERVR_SGX_FW_IF_SGXMKIF_1_7 1

/**
 * struct drm_powervr_sgx_dev_query_limits - Limits of the object-creating ioctls
 *
 * Fetched with %DRM_POWERVR_SGX_DEV_QUERY_LIMITS_GET.
 */
struct drm_powervr_sgx_dev_query_limits {
	/** @rt_max_width: Largest &drm_powervr_sgx_ioctl_create_rt_dataset_args.width. */
	__u32 rt_max_width;

	/** @rt_max_height: Largest &drm_powervr_sgx_ioctl_create_rt_dataset_args.height. */
	__u32 rt_max_height;

	/** @rt_max_samples: Largest &drm_powervr_sgx_ioctl_create_rt_dataset_args.samples. */
	__u32 rt_max_samples;

	/**
	 * @pb_min_pages: Smallest
	 * &drm_powervr_sgx_ioctl_create_context_args.param_buffer_pages.
	 */
	__u32 pb_min_pages;

	/**
	 * @pb_default_pages: Size used when
	 * &drm_powervr_sgx_ioctl_create_context_args.param_buffer_pages is 0.
	 */
	__u32 pb_default_pages;

	/**
	 * @pb_max_pages: Largest
	 * &drm_powervr_sgx_ioctl_create_context_args.param_buffer_pages.
	 */
	__u32 pb_max_pages;

	/** @max_ta_kicks: Largest &drm_powervr_sgx_job.ta_kicks count. */
	__u32 max_ta_kicks;

	/** @_padding_1c: Reserved. This field must be zero. */
	__u32 _padding_1c;
};

/**
 * struct drm_powervr_sgx_dev_query_quirks - Hardware fixes userspace has to know
 *
 * Fetched with %DRM_POWERVR_SGX_DEV_QUERY_QUIRKS_GET.
 */
struct drm_powervr_sgx_dev_query_quirks {
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
 * DRM_POWERVR_SGX_QUIRK_UNISOLATED - The device does not isolate clients
 *
 * Not an erratum number. Listed, never as a mandatory quirk, on a core with
 * one memory context (the SGX530), which the driver runs only with the
 * allow_unisolated module parameter. The firmware is mapped in every client's
 * address space there, so a client's shaders can read the firmware's
 * structures and the other clients' commands, and can corrupt the firmware.
 */
#define DRM_POWERVR_SGX_QUIRK_UNISOLATED 1

/**
 * enum drm_powervr_sgx_heap_id - Index into the array returned by
 * %DRM_POWERVR_SGX_DEV_QUERY_HEAP_INFO_GET
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
	/** @DRM_POWERVR_SGX_HEAP_COUNT: Number of heaps known to this header. */
	DRM_POWERVR_SGX_HEAP_COUNT
};

/**
 * struct drm_powervr_sgx_heap - One device-virtual heap
 *
 * Heaps are the same in every VM context of a device.
 */
struct drm_powervr_sgx_heap {
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
 * struct drm_powervr_sgx_dev_query_heap_info - Array of heaps
 *
 * Fetched with %DRM_POWERVR_SGX_DEV_QUERY_HEAP_INFO_GET.
 */
struct drm_powervr_sgx_dev_query_heap_info {
	/**
	 * @heaps: Array of &struct drm_powervr_sgx_heap, indexed by &enum
	 * drm_powervr_sgx_heap_id. With a NULL array pointer, the stride and count
	 * are set to what the kernel knows and nothing else is written.
	 */
	struct drm_powervr_sgx_obj_array heaps;
};

/**
 * enum drm_powervr_sgx_dev_query - Values of &drm_powervr_sgx_ioctl_dev_query_args.type
 *
 * Append only.
 */
enum drm_powervr_sgx_dev_query {
	/** @DRM_POWERVR_SGX_DEV_QUERY_GPU_INFO_GET: &struct drm_powervr_sgx_dev_query_gpu_info. */
	DRM_POWERVR_SGX_DEV_QUERY_GPU_INFO_GET = 0,
	/** @DRM_POWERVR_SGX_DEV_QUERY_LIMITS_GET: &struct drm_powervr_sgx_dev_query_limits. */
	DRM_POWERVR_SGX_DEV_QUERY_LIMITS_GET,
	/** @DRM_POWERVR_SGX_DEV_QUERY_QUIRKS_GET: &struct drm_powervr_sgx_dev_query_quirks. */
	DRM_POWERVR_SGX_DEV_QUERY_QUIRKS_GET,
	/**
	 * @DRM_POWERVR_SGX_DEV_QUERY_HEAP_INFO_GET: &struct drm_powervr_sgx_dev_query_heap_info.
	 */
	DRM_POWERVR_SGX_DEV_QUERY_HEAP_INFO_GET,
};

/**
 * struct drm_powervr_sgx_ioctl_dev_query_args - Arguments for %DRM_IOCTL_POWERVR_SGX_DEV_QUERY
 */
struct drm_powervr_sgx_ioctl_dev_query_args {
	/** @type: [IN] Query, one of &enum drm_powervr_sgx_dev_query. */
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
 * DOC: PowerVR SGX IOCTL CREATE_BO interface
 *
 * Buffer objects are GEM objects backed by shmem and zeroed at allocation. PRIME
 * export and import use the DRM core ioctls.
 */

/**
 * DRM_POWERVR_SGX_BO_ALLOW_CPU_ACCESS - Allow %DRM_IOCTL_POWERVR_SGX_GET_BO_MMAP_OFFSET
 * on this object.
 */
#define DRM_POWERVR_SGX_BO_ALLOW_CPU_ACCESS _BITULL(0)

/**
 * DRM_POWERVR_SGX_BO_CODE - A code object: USSE programs.
 *
 * Only code objects can be mapped into the USSE heaps, and a code object can be
 * mapped nowhere else. The GPU sees it read-only. It has no CPU mapping and
 * can't be exported, so %DRM_POWERVR_SGX_BO_ALLOW_CPU_ACCESS must be clear. Its
 * contents arrive only through %DRM_IOCTL_POWERVR_SGX_UPLOAD_CODE, which
 * validates them. Bits 2..63 of the flags are reserved.
 */
#define DRM_POWERVR_SGX_BO_CODE _BITULL(1)

/**
 * struct drm_powervr_sgx_ioctl_create_bo_args - Arguments for %DRM_IOCTL_POWERVR_SGX_CREATE_BO
 */
struct drm_powervr_sgx_ioctl_create_bo_args {
	/** @size: [IN] Size in bytes. Must be page aligned and not zero. */
	__u64 size;

	/** @handle: [OUT] GEM handle of the new object. */
	__u32 handle;

	/** @_padding_c: Reserved. This field must be zero. */
	__u32 _padding_c;

	/** @flags: [IN] ``DRM_POWERVR_SGX_BO_*`` flags. */
	__u64 flags;
};

/**
 * struct drm_powervr_sgx_ioctl_get_bo_mmap_offset_args - Arguments for
 * %DRM_IOCTL_POWERVR_SGX_GET_BO_MMAP_OFFSET
 */
struct drm_powervr_sgx_ioctl_get_bo_mmap_offset_args {
	/** @handle: [IN] GEM handle. */
	__u32 handle;

	/** @_padding_4: Reserved. This field must be zero. */
	__u32 _padding_4;

	/** @offset: [OUT] Fake offset to pass to mmap() on the DRM file. */
	__u64 offset;
};

/**
 * DOC: PowerVR SGX IOCTL UPLOAD_CODE interface
 *
 * Copies USSE instructions into a code object and validates them (docs/UAPI.md
 * section 7.2, decision D9). An instruction is rejected if:
 *
 *  - it is not one of the forms the kernel knows (an unknown encoding might
 *    reach what a known one can't);
 *  - it reads or writes the core's register file (``ldr``, ``str``);
 *  - it sets the link address from a register (``setl``, ``savl``), which
 *    would let ``lapc`` jump anywhere;
 *  - it is a relative branch (``br``) whose target is outside the object.
 *
 * On a rejection nothing is copied, the ioctl fails with -EINVAL, and
 * @bad_offset is the byte offset in the object of the first rejected
 * instruction.
 *
 * Bytes no upload wrote are zero, which is a harmless instruction. While a VM
 * maps the object an upload may only write past every byte an earlier upload
 * wrote, so code the GPU may be running never changes under it; otherwise
 * -EBUSY. Once no VM maps the object it can be uploaded anywhere again.
 */

/**
 * struct drm_powervr_sgx_ioctl_upload_code_args - Arguments for
 * %DRM_IOCTL_POWERVR_SGX_UPLOAD_CODE
 */
struct drm_powervr_sgx_ioctl_upload_code_args {
	/** @handle: [IN] GEM handle of a code object (%DRM_POWERVR_SGX_BO_CODE). */
	__u32 handle;

	/** @flags: [IN] Reserved. Must be zero. */
	__u32 flags;

	/** @offset: [IN] Byte offset in the object. Must be a multiple of 8. */
	__u64 offset;

	/** @size: [IN] Bytes to copy. Must be a multiple of 8 and not zero. */
	__u64 size;

	/** @pointer: [IN] User pointer to the instructions, low word first. */
	__u64 pointer;

	/**
	 * @bad_offset: [OUT] When validation rejects the code, the object offset
	 * of the first rejected instruction; otherwise all ones.
	 */
	__u64 bad_offset;
};

/**
 * DOC: PowerVR SGX IOCTL VM context interfaces
 *
 * A VM context is one GPU page directory, managed with drm_gpuvm. The client
 * manages the address space: it picks the address of each mapping inside the
 * heaps %DRM_POWERVR_SGX_DEV_QUERY_HEAP_INFO_GET returns. Addresses outside them
 * cannot be mapped: the kernel keeps the rest of the 4 GiB space for itself.
 */

/**
 * struct drm_powervr_sgx_ioctl_create_vm_context_args - Arguments for
 * %DRM_IOCTL_POWERVR_SGX_CREATE_VM_CONTEXT
 */
struct drm_powervr_sgx_ioctl_create_vm_context_args {
	/** @handle: [OUT] Handle of the new VM context. */
	__u32 handle;

	/** @_padding_4: Reserved. This field must be zero. */
	__u32 _padding_4;
};

/**
 * struct drm_powervr_sgx_ioctl_destroy_vm_context_args - Arguments for
 * %DRM_IOCTL_POWERVR_SGX_DESTROY_VM_CONTEXT
 */
struct drm_powervr_sgx_ioctl_destroy_vm_context_args {
	/** @handle: [IN] Handle of the VM context. */
	__u32 handle;

	/** @_padding_4: Reserved. This field must be zero. */
	__u32 _padding_4;
};

/**
 * struct drm_powervr_sgx_ioctl_vm_map_args - Arguments for %DRM_IOCTL_POWERVR_SGX_VM_MAP
 *
 * Returns -EINVAL when the range is not page aligned or does not fit inside one
 * heap, and -EEXIST when it overlaps an existing mapping.
 */
struct drm_powervr_sgx_ioctl_vm_map_args {
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
 * struct drm_powervr_sgx_ioctl_vm_unmap_args - Arguments for %DRM_IOCTL_POWERVR_SGX_VM_UNMAP
 *
 * The object's pages stay allocated until the GPU's translation caches have been
 * invalidated and every job submitted before the unmap has finished. A job
 * already running that still uses the range faults or reads stale data inside
 * this VM only.
 */
struct drm_powervr_sgx_ioctl_vm_unmap_args {
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
 * DOC: PowerVR SGX IOCTL CREATE_CONTEXT interface
 *
 * A context is one scheduling queue on the GPU. Its firmware-visible state (HW
 * context, command ring, parameter buffer, free list) belongs to the kernel and
 * is never mapped into the client's address space for CPU access.
 */

/**
 * enum drm_powervr_sgx_ctx_type - Values of &drm_powervr_sgx_ioctl_create_context_args.type
 */
enum drm_powervr_sgx_ctx_type {
	/** @DRM_POWERVR_SGX_CTX_TYPE_RENDER: Runs %DRM_POWERVR_SGX_JOB_TYPE_RENDER jobs. */
	DRM_POWERVR_SGX_CTX_TYPE_RENDER = 0,
	/** @DRM_POWERVR_SGX_CTX_TYPE_TRANSFER: Runs %DRM_POWERVR_SGX_JOB_TYPE_TRANSFER jobs. */
	DRM_POWERVR_SGX_CTX_TYPE_TRANSFER,
};

/**
 * enum drm_powervr_sgx_ctx_priority - Values of
 * &drm_powervr_sgx_ioctl_create_context_args.priority
 */
enum drm_powervr_sgx_ctx_priority {
	/** @DRM_POWERVR_SGX_CTX_PRIORITY_LOW: Below normal. */
	DRM_POWERVR_SGX_CTX_PRIORITY_LOW = -512,
	/** @DRM_POWERVR_SGX_CTX_PRIORITY_NORMAL: Normal. */
	DRM_POWERVR_SGX_CTX_PRIORITY_NORMAL = 0,
	/** @DRM_POWERVR_SGX_CTX_PRIORITY_HIGH: Above normal. Requires CAP_SYS_NICE. */
	DRM_POWERVR_SGX_CTX_PRIORITY_HIGH = 512,
};

/**
 * struct drm_powervr_sgx_ioctl_create_context_args - Arguments for
 * %DRM_IOCTL_POWERVR_SGX_CREATE_CONTEXT
 */
struct drm_powervr_sgx_ioctl_create_context_args {
	/** @type: [IN] One of &enum drm_powervr_sgx_ctx_type. */
	__u32 type;

	/** @flags: [IN] Reserved, must be 0. */
	__u32 flags;

	/** @priority: [IN] One of &enum drm_powervr_sgx_ctx_priority. */
	__s32 priority;

	/** @vm_context_handle: [IN] VM context whose page directory the jobs use. */
	__u32 vm_context_handle;

	/**
	 * @param_buffer_pages: [IN] Parameter buffer size for a render context,
	 * in device pages, or 0 for the default in
	 * &drm_powervr_sgx_dev_query_limits.pb_default_pages. Must be 0 for a transfer
	 * context. The device has one parameter buffer, shared by every render
	 * context of every client: the first render context sets its size, and
	 * later requests are ignored.
	 */
	__u32 param_buffer_pages;

	/** @handle: [OUT] Handle of the new context. */
	__u32 handle;
};

/**
 * struct drm_powervr_sgx_ioctl_destroy_context_args - Arguments for
 * %DRM_IOCTL_POWERVR_SGX_DESTROY_CONTEXT
 */
struct drm_powervr_sgx_ioctl_destroy_context_args {
	/** @handle: [IN] Handle of the context. */
	__u32 handle;

	/** @_padding_4: Reserved. This field must be zero. */
	__u32 _padding_4;
};

/**
 * enum drm_powervr_sgx_context_state - Values of
 * &drm_powervr_sgx_ioctl_get_context_state_args.state
 *
 * These correspond to the reset statuses of GL_ARB_robustness.
 */
enum drm_powervr_sgx_context_state {
	/** @DRM_POWERVR_SGX_CONTEXT_STATE_OK: No reset has affected this context. */
	DRM_POWERVR_SGX_CONTEXT_STATE_OK = 0,
	/**
	 * @DRM_POWERVR_SGX_CONTEXT_STATE_GUILTY: A job of this context hung the GPU
	 * or faulted. Its fence carries -ETIME or -EFAULT.
	 */
	DRM_POWERVR_SGX_CONTEXT_STATE_GUILTY,
	/**
	 * @DRM_POWERVR_SGX_CONTEXT_STATE_INNOCENT: A job of this context was running
	 * when another context's job reset the GPU. Its fence carries -ECANCELED.
	 */
	DRM_POWERVR_SGX_CONTEXT_STATE_INNOCENT,
};

/**
 * struct drm_powervr_sgx_ioctl_get_context_state_args - Arguments for
 * %DRM_IOCTL_POWERVR_SGX_GET_CONTEXT_STATE
 *
 * A context that is not %DRM_POWERVR_SGX_CONTEXT_STATE_OK fails every later submission
 * with -ECANCELED; the client destroys it and creates a new one. A context whose
 * jobs were only queued during a reset stays OK and its jobs run afterwards.
 */
struct drm_powervr_sgx_ioctl_get_context_state_args {
	/** @handle: [IN] Handle of the context. */
	__u32 handle;

	/** @state: [OUT] One of &enum drm_powervr_sgx_context_state. */
	__u32 state;

	/** @reset_count: [OUT] Number of GPU resets since the device was opened. */
	__u32 reset_count;

	/** @_padding_c: Reserved. This field must be zero. */
	__u32 _padding_c;
};

/**
 * DOC: PowerVR SGX IOCTL CREATE_RT_DATASET interface
 *
 * An RT dataset is what tiling a target of a given size needs: the macrotile
 * numbers, region headers, tail pointers, background object block and firmware
 * render data. The kernel derives all of it from the size, allocates it and keeps
 * it. A client sees only the handle.
 */

/**
 * struct drm_powervr_sgx_ioctl_create_rt_dataset_args - Arguments for
 * %DRM_IOCTL_POWERVR_SGX_CREATE_RT_DATASET
 */
struct drm_powervr_sgx_ioctl_create_rt_dataset_args {
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
 * struct drm_powervr_sgx_ioctl_destroy_rt_dataset_args - Arguments for
 * %DRM_IOCTL_POWERVR_SGX_DESTROY_RT_DATASET
 */
struct drm_powervr_sgx_ioctl_destroy_rt_dataset_args {
	/** @handle: [IN] Handle of the dataset. */
	__u32 handle;

	/** @_padding_4: Reserved. This field must be zero. */
	__u32 _padding_4;
};

/**
 * DOC: PowerVR SGX command streams
 *
 * The client's part of a GPU command is a stream of scalars, in the manner of the
 * upstream PowerVR driver's streams. The kernel copies the stream into kernel
 * memory, decodes it, checks every field and builds the firmware command from the
 * result; docs/UAPI.md lists how every word of the firmware command is produced.
 *
 * A stream is:
 *
 *  - A header of two __u32: the length of the main stream in bytes, header
 *    included, then a word that must be zero.
 *  - The main stream: the command's fields, in their defined order, each naturally
 *    aligned from the start of the stream. Its length must be exactly the one the
 *    kernel knows: a main stream never changes once released.
 *  - Optionally, extension streams. Each is announced by a __u32 header: bits
 *    29-31 the header type, bit 28 "another header follows", bits 0-27 one bit per
 *    extension stream, whose fields follow in bit order. Each header type appears
 *    at most once. New fields are only ever added as extension streams.
 *
 * Bytes skipped to align a field must be zero, and nothing may follow the last
 * stream. A stream is at most 4096 bytes and starts on an 8-byte boundary.
 *
 * Addresses in a stream are device-virtual addresses in the job's VM. A field the
 * kernel does not check is one whose value only reaches a hardware register, with
 * no effect outside the VM.
 *
 * The field lists are not in this header. They are in the stream definition file
 * (powervr_sgx_kmd_stream.xml in docs/uapi), which generates both the kernel's
 * decoder tables and the userspace stream writers. The streams are:
 *
 *  - CMD_RENDER: the 3D part of a render job, at &drm_powervr_sgx_job.cmd_stream.
 *  - CMD_TA_KICK: one tiling kick, at &drm_powervr_sgx_ta_kick.cmd_stream.
 *  - CMD_TRANSFER: a transfer job, at &drm_powervr_sgx_job.cmd_stream.
 */

/**
 * struct drm_powervr_sgx_ta_kick - One tiling kick of a render job
 *
 * The first element of &drm_powervr_sgx_job.ta_kicks starts the scene. The last
 * one ends it, and the kernel starts the 3D after it.
 */
struct drm_powervr_sgx_ta_kick {
	/** @cmd_stream: User pointer to a CMD_TA_KICK stream. 8-byte aligned. */
	__u64 cmd_stream;

	/** @cmd_stream_len: Length of the stream, in bytes. */
	__u32 cmd_stream_len;

	/** @flags: Reserved, must be 0. */
	__u32 flags;
};

/**
 * DOC: PowerVR SGX IOCTL SUBMIT_JOBS interface
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
 * DOC: Flags for struct drm_powervr_sgx_sync_op
 *
 * .. c:macro:: DRM_POWERVR_SGX_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ
 *
 *    The handle is a binary syncobj. This is the default.
 *
 * .. c:macro:: DRM_POWERVR_SGX_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ
 *
 *    The handle is a timeline syncobj and the value names a point on it.
 *
 * .. c:macro:: DRM_POWERVR_SGX_SYNC_OP_FLAG_WAIT
 *
 *    The job waits for this syncobj before it starts. This is the default.
 *
 * .. c:macro:: DRM_POWERVR_SGX_SYNC_OP_FLAG_SIGNAL
 *
 *    The job's out-fence is attached to this syncobj.
 */
#define DRM_POWERVR_SGX_SYNC_OP_FLAG_HANDLE_TYPE_MASK 0xfU
#define DRM_POWERVR_SGX_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ 0U
#define DRM_POWERVR_SGX_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ 1U
#define DRM_POWERVR_SGX_SYNC_OP_FLAG_WAIT 0U
#define DRM_POWERVR_SGX_SYNC_OP_FLAG_SIGNAL _BITUL(31)

/**
 * struct drm_powervr_sgx_sync_op - One wait or signal of a job
 */
struct drm_powervr_sgx_sync_op {
	/** @handle: Syncobj handle. */
	__u32 handle;

	/** @flags: ``DRM_POWERVR_SGX_SYNC_OP_FLAG_*`` flags. */
	__u32 flags;

	/** @value: Timeline point. Must be 0 for a binary syncobj. */
	__u64 value;
};

/**
 * DOC: Flags for struct drm_powervr_sgx_bo_ref
 *
 * .. c:macro:: DRM_POWERVR_SGX_BO_REF_FLAG_WRITE
 *
 *    The job writes this object: the out-fence is added to its reservation
 *    object as a write fence and the job waits for all earlier fences on it.
 *    Without this flag the job reads it: the out-fence is added as a read fence
 *    and the job waits for earlier write fences.
 */
#define DRM_POWERVR_SGX_BO_REF_FLAG_WRITE _BITUL(0)

/**
 * struct drm_powervr_sgx_bo_ref - An object a job takes part in implicit sync on
 *
 * Only objects shared with another client or process need an entry. An object
 * must be mapped in the job's VM to be useful, and that is not checked here.
 *
 * Every object imported from another device that the job reads or writes
 * must be listed: the GPU doesn't snoop the CPU caches, and the kernel flushes
 * only listed imports around the job. An unlisted import may be read stale or
 * have its contents overwritten by dirty CPU cache lines.
 */
struct drm_powervr_sgx_bo_ref {
	/** @handle: GEM handle. */
	__u32 handle;

	/** @flags: ``DRM_POWERVR_SGX_BO_REF_FLAG_*`` flags. */
	__u32 flags;
};

/**
 * DOC: Flags for &drm_powervr_sgx_job.flags
 *
 * Valid flags depend on the job type. Render jobs have none yet.
 *
 * .. c:macro:: DRM_POWERVR_SGX_SUBMIT_JOB_TRANSFER_DST_TWIDDLED
 *
 *    The transfer's destination surface is twiddled.
 */
#define DRM_POWERVR_SGX_SUBMIT_JOB_RENDER_FLAGS_MASK 0U
#define DRM_POWERVR_SGX_SUBMIT_JOB_TRANSFER_DST_TWIDDLED _BITUL(0)
#define DRM_POWERVR_SGX_SUBMIT_JOB_TRANSFER_FLAGS_MASK \
	DRM_POWERVR_SGX_SUBMIT_JOB_TRANSFER_DST_TWIDDLED

/**
 * enum drm_powervr_sgx_job_type - Values of &drm_powervr_sgx_job.type
 */
enum drm_powervr_sgx_job_type {
	/** @DRM_POWERVR_SGX_JOB_TYPE_RENDER: A whole scene: TA kicks and the 3D. */
	DRM_POWERVR_SGX_JOB_TYPE_RENDER = 0,
	/** @DRM_POWERVR_SGX_JOB_TYPE_TRANSFER: One 3D-only blit. */
	DRM_POWERVR_SGX_JOB_TYPE_TRANSFER,
	/**
	 * @DRM_POWERVR_SGX_JOB_TYPE_NULL: No GPU work. The firmware takes it in
	 * queue order with the context's other jobs, and its out-fences signal
	 * once it has. Runs on a context of either type; no stream, no flags,
	 * no TA kicks, no RT dataset. For testing synchronisation (and for a
	 * fence that follows everything queued before it).
	 */
	DRM_POWERVR_SGX_JOB_TYPE_NULL,
};

/**
 * struct drm_powervr_sgx_job - One job of %DRM_IOCTL_POWERVR_SGX_SUBMIT_JOBS
 */
struct drm_powervr_sgx_job {
	/** @type: One of &enum drm_powervr_sgx_job_type. */
	__u32 type;

	/** @context_handle: Context of the matching type. */
	__u32 context_handle;

	/**
	 * @flags: ``DRM_POWERVR_SGX_SUBMIT_JOB_<type>_*`` flags. Bits outside
	 * the type's mask are rejected.
	 */
	__u32 flags;

	/** @cmd_stream_len: Length of the stream at @cmd_stream, in bytes. */
	__u32 cmd_stream_len;

	/**
	 * @cmd_stream: User pointer to a CMD_RENDER or CMD_TRANSFER stream, by
	 * @type. 8-byte aligned.
	 */
	__u64 cmd_stream;

	/** @sync_ops: Array of &struct drm_powervr_sgx_sync_op. May be empty. */
	struct drm_powervr_sgx_obj_array sync_ops;

	/** @bo_refs: Array of &struct drm_powervr_sgx_bo_ref. May be empty. */
	struct drm_powervr_sgx_obj_array bo_refs;

	/**
	 * @ta_kicks: Array of &struct drm_powervr_sgx_ta_kick. At least one
	 * element and at most &drm_powervr_sgx_dev_query_limits.max_ta_kicks for a
	 * render job, empty for a transfer job.
	 */
	struct drm_powervr_sgx_obj_array ta_kicks;

	/** @rt_dataset_handle: RT dataset of a render job, 0 for a transfer job. */
	__u32 rt_dataset_handle;

	/** @_padding_4c: Reserved. This field must be zero. */
	__u32 _padding_4c;
};

/**
 * struct drm_powervr_sgx_ioctl_submit_jobs_args - Arguments for %DRM_IOCTL_POWERVR_SGX_SUBMIT_JOBS
 *
 * Returns -ECANCELED when a job's context is not usable after a reset.
 */
struct drm_powervr_sgx_ioctl_submit_jobs_args {
	/** @jobs: [IN] Array of &struct drm_powervr_sgx_job. */
	struct drm_powervr_sgx_obj_array jobs;

	/** @failed_index: [OUT] On error, the index of the job that caused it. */
	__u32 failed_index;

	/** @_padding_14: Reserved. This field must be zero. */
	__u32 _padding_14;
};

#if defined(__cplusplus)
}
#endif

#endif /* POWERVR_SGX_DRM_H */
