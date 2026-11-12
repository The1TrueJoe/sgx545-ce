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
 *    address at or above 2^&drm_powervr_sgx_dev_query_limits.va_bits is
 *    rejected.
 *  - Structs reached through &struct drm_powervr_sgx_obj_array can grow at the
 *    end. A short object is zero-extended. In an object longer than the kernel
 *    knows, the unknown bytes must be zero.
 *  - Client commands grow the same way, through &drm_powervr_sgx_cmd_head.size,
 *    and carry core-specific fields in extensions: see "PowerVR SGX client
 *    commands" below.
 *
 * The DRM driver's version is 1.0. Additions to the interface raise the minor
 * version; nothing is removed or renumbered.
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
#define DRM_IOCTL_POWERVR_SGX_VM_BIND PVR_SGX_IOCTL(0x05, DRM_IOW, vm_bind)
#define DRM_IOCTL_POWERVR_SGX_CREATE_CONTEXT PVR_SGX_IOCTL(0x06, DRM_IOWR, create_context)
#define DRM_IOCTL_POWERVR_SGX_DESTROY_CONTEXT PVR_SGX_IOCTL(0x07, DRM_IOW, destroy_context)
#define DRM_IOCTL_POWERVR_SGX_GET_CONTEXT_STATE PVR_SGX_IOCTL(0x08, DRM_IOWR, get_context_state)
#define DRM_IOCTL_POWERVR_SGX_CREATE_RT_DATASET PVR_SGX_IOCTL(0x09, DRM_IOWR, create_rt_dataset)
#define DRM_IOCTL_POWERVR_SGX_DESTROY_RT_DATASET PVR_SGX_IOCTL(0x0a, DRM_IOW, destroy_rt_dataset)
#define DRM_IOCTL_POWERVR_SGX_UPLOAD_CODE PVR_SGX_IOCTL(0x0b, DRM_IOWR, upload_code)
#define DRM_IOCTL_POWERVR_SGX_SUBMIT_JOBS PVR_SGX_IOCTL(0x0c, DRM_IOWR, submit_jobs)

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

/*
 * The microkernel speaks the host interface of the vendor's SGXMKIF, as of its
 * DDK 1.7: the command buffer, control blocks and command layouts.
 */
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

	/**
	 * @va_bits: Device-virtual address bits: every address a command names
	 * is below 2^va_bits.
	 */
	__u32 va_bits;

	/** @caps: ``DRM_POWERVR_SGX_CAP_*`` bits of this core. */
	__u32 caps;

	/** @max_layers: Largest number of render-target layers. */
	__u32 max_layers;
};

/**
 * DRM_POWERVR_SGX_CAP_BACKWARD_BLITS - A transfer may copy between overlapping
 * ranges with the destination above the source.
 */
#define DRM_POWERVR_SGX_CAP_BACKWARD_BLITS _BITUL(0)

/**
 * struct drm_powervr_sgx_dev_query_quirks - Hardware fixes userspace has to know
 *
 * Fetched with %DRM_POWERVR_SGX_DEV_QUERY_QUIRKS_GET. A quirk, once reported
 * for a GPU, is never removed for it.
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
	/**
	 * @DRM_POWERVR_SGX_DEV_QUERY_COMMANDS_GET: &struct drm_powervr_sgx_dev_query_commands.
	 */
	DRM_POWERVR_SGX_DEV_QUERY_COMMANDS_GET,
};

/**
 * struct drm_powervr_sgx_dev_query_commands - What this core accepts
 *
 * Fetched with %DRM_POWERVR_SGX_DEV_QUERY_COMMANDS_GET. A job type whose bit is
 * clear is refused with -EOPNOTSUPP: a value it depends on is not known for this
 * core yet. An extension name outside its command's mask is refused the same way.
 */
struct drm_powervr_sgx_dev_query_commands {
	/** @job_types: Bit n: &enum drm_powervr_sgx_job_type n is accepted. */
	__u32 job_types;

	/** @_padding_4: Reserved. This field must be zero. */
	__u32 _padding_4;

	/** @render_exts: Bit n: extension name n is accepted in a render command. */
	__u64 render_exts;

	/** @ta_kick_exts: Bit n: extension name n is accepted in a TA kick. */
	__u64 ta_kick_exts;

	/** @transfer_exts: Bit n: extension name n is accepted in a transfer command. */
	__u64 transfer_exts;
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
 * Copies USSE instructions into a code object and validates them
 * (Documentation/gpu/powervr_sgx, "Shader validation"). An instruction is
 * rejected if:
 *
 *  - it is not exactly one of the forms the kernel knows;
 *  - it is a form the kernel refuses: register-file access (``ldr``, ``str``),
 *    the link address (``setl``, ``savl``, ``lapc``, a branch with ``link``),
 *    ``ba``, ``phas`` with a register operand, ``lock``, ``release``, the
 *    operand-stepping controls other than ``smlsi``, ``fdual``, and forms
 *    whose meaning is not known;
 *  - it is a ``br`` whose target is outside the object;
 *  - it is a ``phas.i`` with a target other than 0 that is outside the object,
 *    counting from the base of the USSE heap the object is mapped in, or the
 *    object is not mapped, or mapped at more than one place;
 *  - an operand writes a constant or global, reads a global past ``g45``, or
 *    names an internal register past ``i3`` or an index register other than
 *    ``idx1`` and ``idx2``.
 *
 * Once an object holds code with ``phas.i`` targets, a VM_BIND map that would
 * put it anywhere else fails with -EINVAL.
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
 *
 * The handle goes at once. Once no context uses the VM either and every job on
 * it is done, its mappings go and their objects are released; the ioctl
 * doesn't wait for that.
 */
struct drm_powervr_sgx_ioctl_destroy_vm_context_args {
	/** @handle: [IN] Handle of the VM context. */
	__u32 handle;

	/** @_padding_4: Reserved. This field must be zero. */
	__u32 _padding_4;
};

/**
 * DOC: VM_BIND operations
 *
 * .. c:macro:: DRM_POWERVR_SGX_VM_BIND_OP_MAP
 *
 *    Map @size bytes of the object @handle, from @bo_offset, at @device_addr.
 *    The range must lie inside one heap, page aligned, and must not overlap a
 *    mapping (-EEXIST). A code object maps only into a USSE heap, and only a
 *    code object does.
 *
 * .. c:macro:: DRM_POWERVR_SGX_VM_BIND_OP_UNMAP
 *
 *    Unmap every mapping in [@device_addr, @device_addr + @size), splitting a
 *    mapping that straddles either end. @handle and @bo_offset must be 0. A
 *    range with no mapping in it is not an error.
 *
 * The page tables of an unmapped range change before the ioctl returns, so a
 * job that is still running and uses the range faults inside this VM. The
 * objects stay allocated until every job submitted before the unmap is done.
 */
#define DRM_POWERVR_SGX_VM_BIND_OP_MAP 0
#define DRM_POWERVR_SGX_VM_BIND_OP_UNMAP 1

/**
 * struct drm_powervr_sgx_vm_bind_op - One map or unmap of %DRM_IOCTL_POWERVR_SGX_VM_BIND
 */
struct drm_powervr_sgx_vm_bind_op {
	/** @op: [IN] ``DRM_POWERVR_SGX_VM_BIND_OP_*``. */
	__u32 op;

	/** @handle: [IN] GEM handle of the object to map; 0 for an unmap. */
	__u32 handle;

	/** @bo_offset: [IN] Offset into the object, in bytes; 0 for an unmap. */
	__u64 bo_offset;

	/** @device_addr: [IN] Device-virtual start of the range. */
	__u64 device_addr;

	/** @size: [IN] Size of the range, in bytes. */
	__u64 size;

	/** @flags: [IN] Reserved, must be 0. */
	__u32 flags;

	/** @_padding_24: Reserved. This field must be zero. */
	__u32 _padding_24;
};

/**
 * DOC: Flags for struct drm_powervr_sgx_ioctl_vm_bind_args
 *
 * .. c:macro:: DRM_POWERVR_SGX_VM_BIND_FLAG_SYNC
 *
 *    The ioctl binds and returns when the binds are done. A synchronous bind
 *    takes no fences: @sync_ops must be empty. Required: a bind without it
 *    fails with -EINVAL.
 */
#define DRM_POWERVR_SGX_VM_BIND_FLAG_SYNC _BITUL(0)

/**
 * struct drm_powervr_sgx_ioctl_vm_bind_args - Arguments for %DRM_IOCTL_POWERVR_SGX_VM_BIND
 *
 * The ops apply in array order, and all or none of them apply: every op is
 * checked, and everything it needs allocated, before any mapping changes. The
 * ranges of the ops in one call must not overlap each other (-EINVAL). A wait
 * that is interrupted fails with -EINTR before anything changes.
 */
struct drm_powervr_sgx_ioctl_vm_bind_args {
	/** @vm_context_handle: [IN] VM context to bind in. */
	__u32 vm_context_handle;

	/** @flags: [IN] ``DRM_POWERVR_SGX_VM_BIND_FLAG_*``. */
	__u32 flags;

	/** @ops: [IN] Array of &struct drm_powervr_sgx_vm_bind_op, at least one. */
	struct drm_powervr_sgx_obj_array ops;

	/**
	 * @sync_ops: [IN] Must be empty (count 0) with
	 * %DRM_POWERVR_SGX_VM_BIND_FLAG_SYNC, the only mode. Reserved for an
	 * asynchronous mode's &struct drm_powervr_sgx_sync_op array.
	 */
	struct drm_powervr_sgx_obj_array sync_ops;
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

	/**
	 * @reset_count: [OUT] Number of GPU resets since the driver bound the
	 * device, for every context alike; it only grows. Compare two reads to see
	 * whether a reset happened in between. @state says whether this context
	 * was to blame.
	 */
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

	/**
	 * @samples: [IN] Samples per pixel: 1 or 4 (0 is 1). 4 needs
	 * &drm_powervr_sgx_dev_query_limits.rt_max_samples of 4 on this core;
	 * otherwise it is -EOPNOTSUPP. A 4-sample target's renders need a
	 * depth buffer and ZLS control, with depth and stencil per sample.
	 */
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
 * DOC: PowerVR SGX client commands
 *
 * The client's part of a GPU command is one struct per command: &struct
 * drm_powervr_sgx_cmd_render, &struct drm_powervr_sgx_cmd_ta_kick and &struct
 * drm_powervr_sgx_cmd_transfer. Each starts with &struct drm_powervr_sgx_cmd_head:
 * its size, and a chain of core-specific extensions. The kernel copies the
 * command and its extensions, checks every field and builds the firmware command
 * from the result.
 *
 * Addresses are device-virtual addresses in the job's VM. A field the kernel
 * does not check is one whose value only reaches a hardware register, with no
 * effect outside the VM. The structs below and the kernel's check tables are
 * generated by gen_cmd.py from one description of the fields and their checks,
 * powervr_sgx_cmd.xml; both live in the driver's source repository,
 * https://github.com/The1TrueJoe/powervr_sgx.
 */

/* BEGIN commands: generated from powervr_sgx_cmd.xml */

/**
 * struct drm_powervr_sgx_cmd_head - The head of every command struct
 *
 * The kernel copies a command as copy_struct_from_user() does: a shorter
 * struct is zero-filled, non-zero bytes past what the kernel knows are
 * -E2BIG. A field appended later takes 0 to mean "absent".
 */
struct drm_powervr_sgx_cmd_head {
	/** @size: sizeof() the struct as the caller built it. */
	__u32 size;

	/** @_padding_4: Reserved. This field must be zero. */
	__u32 _padding_4;

	/**
	 * @extensions: User pointer to the first &struct drm_powervr_sgx_ext,
	 * or 0.
	 */
	__u64 extensions;
};

/**
 * struct drm_powervr_sgx_ext - The head of every command extension
 *
 * At most 16 in a chain, each name once. A name of another command is
 * -EINVAL; a name this core doesn't accept is -EOPNOTSUPP
 * (%DRM_POWERVR_SGX_DEV_QUERY_COMMANDS_GET lists the accepted ones).
 */
struct drm_powervr_sgx_ext {
	/** @name: One of the ``DRM_POWERVR_SGX_EXT_*`` names. */
	__u32 name;

	/** @_padding_4: Reserved. This field must be zero. */
	__u32 _padding_4;

	/** @next: User pointer to the next extension, or 0. */
	__u64 next;
};

/* &struct drm_powervr_sgx_ext_sgx545_render, for sgx545 */
#define DRM_POWERVR_SGX_EXT_SGX545_RENDER 1
/* &struct drm_powervr_sgx_ext_sgx545_ta_kick, for sgx545 */
#define DRM_POWERVR_SGX_EXT_SGX545_TA_KICK 2

/**
 * struct drm_powervr_sgx_cmd_render - A render command
 *
 * Extensions: &struct drm_powervr_sgx_ext_sgx545_render.
 */
struct drm_powervr_sgx_cmd_render {
	/** @head: The common head. */
	struct drm_powervr_sgx_cmd_head head;

	/** @depth_addr: Depth buffer the ZLS loads and stores, or 0. */
	__u64 depth_addr;

	/** @stencil_addr: Stencil buffer the ZLS loads and stores, or 0. */
	__u64 stencil_addr;

	/** @pixel_event_pds_addr: PDS program run at the end of each tile (the pixel event). */
	__u64 pixel_event_pds_addr;

	/** @bg_pds_secondary_addr: Background object: secondary PDS program. */
	__u64 bg_pds_secondary_addr;

	/** @bg_pds_primary_addr: Background object: primary PDS program. */
	__u64 bg_pds_primary_addr;

	/** @pixel_event_pds_rows: Data rows of the pixel event program. */
	__u32 pixel_event_pds_rows;

	/** @bg_pds_secondary_rows: Data rows of the background secondary program. */
	__u32 bg_pds_secondary_rows;

	/** @bg_pds_primary_rows: Data rows of the background primary program. */
	__u32 bg_pds_primary_rows;

	/**
	 * @bg_depth_f32: Depth the background object clears to. IEEE-754
	 * binary32 bits, any value.
	 */
	__u32 bg_depth_f32;

	/** @depth_bias_units: Constant depth bias (glPolygonOffset units). */
	__s32 depth_bias_units;

	/**
	 * @depth_bias_factor_f32: Slope-scaled depth bias (glPolygonOffset
	 * factor). IEEE-754 binary32 bits, any value.
	 */
	__u32 depth_bias_factor_f32;
};

/**
 * struct drm_powervr_sgx_cmd_ta_kick - A ta kick command
 *
 * Extensions: &struct drm_powervr_sgx_ext_sgx545_ta_kick.
 */
struct drm_powervr_sgx_cmd_ta_kick {
	/** @head: The common head. */
	struct drm_powervr_sgx_cmd_head head;

	/** @vdm_base: Start of the kick's VDM control stream. */
	__u64 vdm_base;
};

/**
 * struct drm_powervr_sgx_cmd_transfer - A transfer command
 */
struct drm_powervr_sgx_cmd_transfer {
	/** @head: The common head. */
	struct drm_powervr_sgx_cmd_head head;

	/** @bg_block_addr: Background block: the blit's PDS program triple. */
	__u64 bg_block_addr;

	/** @pixel_event_pds_addr: PDS program run at the end of each tile. */
	__u64 pixel_event_pds_addr;

	/** @pixel_event_pds_rows: Data rows of the pixel event program. */
	__u32 pixel_event_pds_rows;

	/** @first_tile_x: First tile column to blit. */
	__u32 first_tile_x;

	/** @first_tile_y: First tile row to blit. */
	__u32 first_tile_y;

	/** @last_tile_x: Last tile column to blit. */
	__u32 last_tile_x;

	/** @last_tile_y: Last tile row to blit. */
	__u32 last_tile_y;

	/** @bg_depth_f32: Depth the background object writes. IEEE-754 binary32 bits, any value. */
	__u32 bg_depth_f32;
};

/**
 * struct drm_powervr_sgx_ext_sgx545_render - Extension %DRM_POWERVR_SGX_EXT_SGX545_RENDER
 *
 * For &struct drm_powervr_sgx_cmd_render, on sgx545.
 * Absent, its fields read as zero.
 */
struct drm_powervr_sgx_ext_sgx545_render {
	/** @base: The common head. */
	struct drm_powervr_sgx_ext base;

	/** @zls_control: ZLS control register image; bits 3-11 are the kernel's. */
	__u32 zls_control;

	/** @flags_3d: 3D flags; bit 8 uses the stencil buffer. */
	__u32 flags_3d;
};

/**
 * struct drm_powervr_sgx_ext_sgx545_ta_kick - Extension %DRM_POWERVR_SGX_EXT_SGX545_TA_KICK
 *
 * For &struct drm_powervr_sgx_cmd_ta_kick, on sgx545.
 * Absent, its fields read as zero.
 */
struct drm_powervr_sgx_ext_sgx545_ta_kick {
	/** @base: The common head. */
	struct drm_powervr_sgx_ext base;

	/** @ta_flags2: TA flags register image. */
	__u32 ta_flags2;

	/** @_padding_14: Reserved. This field must be zero. */
	__u32 _padding_14;
};

/* END commands */

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
 * DOC: Flags for struct drm_powervr_sgx_cache_range
 *
 * .. c:macro:: DRM_POWERVR_SGX_CACHE_RANGE_FLAG_READ
 *
 *    The job reads the range.
 *
 * .. c:macro:: DRM_POWERVR_SGX_CACHE_RANGE_FLAG_WRITE
 *
 *    The job writes the range.
 */
#define DRM_POWERVR_SGX_CACHE_RANGE_FLAG_READ _BITUL(0)
#define DRM_POWERVR_SGX_CACHE_RANGE_FLAG_WRITE _BITUL(1)

/**
 * struct drm_powervr_sgx_cache_range - Bytes of an import a job reads or writes
 *
 * The GPU doesn't snoop the CPU caches, and an imported object's pages may be
 * cached. The kernel keeps a job coherent with the CPU on the listed bytes
 * only:
 *
 *  - when the job is about to run, it writes back and drops the CPU's lines of
 *    every range, so the GPU reads what the CPU wrote and no dirty line lands on
 *    the job's output later;
 *  - once the job's writes have reached memory, and before its out-fence
 *    signals, it drops the lines of every range with
 *    %DRM_POWERVR_SGX_CACHE_RANGE_FLAG_WRITE, which the CPU may have fetched
 *    again meanwhile. A job whose out-fence carries an error gets no such
 *    flush: its WRITE ranges are undefined.
 *
 * The range is @rows rows of @size bytes, @stride bytes apart, from @offset,
 * widened to whole cache lines: one damage rectangle of a linear buffer is one
 * entry. @rows 0 is one row. An entry for an object that isn't an import (ours
 * are write-combined, and a dma-buf of this device imports as the original
 * object) is checked and flushes nothing. No fence is attached.
 *
 * Bytes of an import the job changes but doesn't list have undefined contents
 * afterwards, for the CPU and the GPU alike. That is never a leak: the kernel
 * writes the whole import back once when it is imported, so a stale line or a
 * stale DRAM word is the object's own earlier contents. Bytes a job rewrites
 * unchanged (a render's tiles outside its draws, which it loaded) need no entry
 * while the CPU doesn't write the object during the job.
 */
struct drm_powervr_sgx_cache_range {
	/** @handle: GEM handle. */
	__u32 handle;

	/** @flags: ``DRM_POWERVR_SGX_CACHE_RANGE_FLAG_*``, at least one. */
	__u32 flags;

	/** @offset: Byte offset of the first row. */
	__u64 offset;

	/** @size: Bytes per row. Must not be 0. */
	__u64 size;

	/** @stride: Bytes from one row to the next; at least @size when @rows > 1. */
	__u32 stride;

	/** @rows: Number of rows; 0 and 1 both mean one. */
	__u32 rows;
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
	 * once it has. Runs on a context of either type; no command, no flags,
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

	/** @_padding_c: Reserved. This field must be zero. */
	__u32 _padding_c;

	/**
	 * @cmd: User pointer to the type's command: &struct
	 * drm_powervr_sgx_cmd_render or &struct drm_powervr_sgx_cmd_transfer.
	 */
	__u64 cmd;

	/** @sync_ops: Array of &struct drm_powervr_sgx_sync_op. May be empty. */
	struct drm_powervr_sgx_obj_array sync_ops;

	/**
	 * @cache_ranges: Array of &struct drm_powervr_sgx_cache_range, at most
	 * 64: the bytes of imports the job reads or writes. May be empty.
	 */
	struct drm_powervr_sgx_obj_array cache_ranges;

	/**
	 * @ta_kicks: Array of &struct drm_powervr_sgx_cmd_ta_kick, each with
	 * &drm_powervr_sgx_cmd_head.size equal to the stride. At least one
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

	/**
	 * @failed_index: [OUT] On error, the index of the job that caused it.
	 * When the error is -ENOMEM from a wait on an earlier job of the same
	 * call, the jobs before it were submitted; for any other error none were.
	 */
	__u32 failed_index;

	/** @_padding_14: Reserved. This field must be zero. */
	__u32 _padding_14;
};

#if defined(__cplusplus)
}
#endif

#endif /* POWERVR_SGX_DRM_H */
