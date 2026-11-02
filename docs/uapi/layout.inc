/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */

/*
 * The layout of every struct in powervr_sgx_drm.h as literal numbers. check_layout.c
 * turns each line into static asserts; gen_table.py turns it into the table in
 * docs/UAPI.md. FIELD(struct, member, offset, size) lines must be in member order
 * and cover the whole struct.
 */

STRUCT(drm_powervr_sgx_obj_array, 16)
FIELD(drm_powervr_sgx_obj_array, stride, 0x00, 4)
FIELD(drm_powervr_sgx_obj_array, count, 0x04, 4)
FIELD(drm_powervr_sgx_obj_array, array, 0x08, 8)

STRUCT(drm_powervr_sgx_dev_query_gpu_info, 16)
FIELD(drm_powervr_sgx_dev_query_gpu_info, core_id, 0x00, 4)
FIELD(drm_powervr_sgx_dev_query_gpu_info, core_revision, 0x04, 4)
FIELD(drm_powervr_sgx_dev_query_gpu_info, fw_interface_version, 0x08, 4)
FIELD(drm_powervr_sgx_dev_query_gpu_info, _padding_c, 0x0c, 4)

STRUCT(drm_powervr_sgx_dev_query_limits, 32)
FIELD(drm_powervr_sgx_dev_query_limits, rt_max_width, 0x00, 4)
FIELD(drm_powervr_sgx_dev_query_limits, rt_max_height, 0x04, 4)
FIELD(drm_powervr_sgx_dev_query_limits, rt_max_samples, 0x08, 4)
FIELD(drm_powervr_sgx_dev_query_limits, pb_min_pages, 0x0c, 4)
FIELD(drm_powervr_sgx_dev_query_limits, pb_default_pages, 0x10, 4)
FIELD(drm_powervr_sgx_dev_query_limits, pb_max_pages, 0x14, 4)
FIELD(drm_powervr_sgx_dev_query_limits, max_ta_kicks, 0x18, 4)
FIELD(drm_powervr_sgx_dev_query_limits, _padding_1c, 0x1c, 4)

STRUCT(drm_powervr_sgx_dev_query_quirks, 16)
FIELD(drm_powervr_sgx_dev_query_quirks, quirks, 0x00, 8)
FIELD(drm_powervr_sgx_dev_query_quirks, count, 0x08, 2)
FIELD(drm_powervr_sgx_dev_query_quirks, musthave_count, 0x0a, 2)
FIELD(drm_powervr_sgx_dev_query_quirks, _padding_c, 0x0c, 4)

STRUCT(drm_powervr_sgx_heap, 32)
FIELD(drm_powervr_sgx_heap, base, 0x00, 8)
FIELD(drm_powervr_sgx_heap, size, 0x08, 8)
FIELD(drm_powervr_sgx_heap, flags, 0x10, 4)
FIELD(drm_powervr_sgx_heap, page_size_log2, 0x14, 4)
FIELD(drm_powervr_sgx_heap, usse_base_index, 0x18, 4)
FIELD(drm_powervr_sgx_heap, usse_window_log2, 0x1c, 4)

STRUCT(drm_powervr_sgx_dev_query_heap_info, 16)
FIELD(drm_powervr_sgx_dev_query_heap_info, heaps, 0x00, 16)

STRUCT(drm_powervr_sgx_ioctl_dev_query_args, 16)
FIELD(drm_powervr_sgx_ioctl_dev_query_args, type, 0x00, 4)
FIELD(drm_powervr_sgx_ioctl_dev_query_args, size, 0x04, 4)
FIELD(drm_powervr_sgx_ioctl_dev_query_args, pointer, 0x08, 8)

STRUCT(drm_powervr_sgx_ioctl_create_bo_args, 24)
FIELD(drm_powervr_sgx_ioctl_create_bo_args, size, 0x00, 8)
FIELD(drm_powervr_sgx_ioctl_create_bo_args, handle, 0x08, 4)
FIELD(drm_powervr_sgx_ioctl_create_bo_args, _padding_c, 0x0c, 4)
FIELD(drm_powervr_sgx_ioctl_create_bo_args, flags, 0x10, 8)

STRUCT(drm_powervr_sgx_ioctl_get_bo_mmap_offset_args, 16)
FIELD(drm_powervr_sgx_ioctl_get_bo_mmap_offset_args, handle, 0x00, 4)
FIELD(drm_powervr_sgx_ioctl_get_bo_mmap_offset_args, _padding_4, 0x04, 4)
FIELD(drm_powervr_sgx_ioctl_get_bo_mmap_offset_args, offset, 0x08, 8)

STRUCT(drm_powervr_sgx_ioctl_create_vm_context_args, 8)
FIELD(drm_powervr_sgx_ioctl_create_vm_context_args, handle, 0x00, 4)
FIELD(drm_powervr_sgx_ioctl_create_vm_context_args, _padding_4, 0x04, 4)

STRUCT(drm_powervr_sgx_ioctl_destroy_vm_context_args, 8)
FIELD(drm_powervr_sgx_ioctl_destroy_vm_context_args, handle, 0x00, 4)
FIELD(drm_powervr_sgx_ioctl_destroy_vm_context_args, _padding_4, 0x04, 4)

STRUCT(drm_powervr_sgx_ioctl_vm_map_args, 40)
FIELD(drm_powervr_sgx_ioctl_vm_map_args, vm_context_handle, 0x00, 4)
FIELD(drm_powervr_sgx_ioctl_vm_map_args, flags, 0x04, 4)
FIELD(drm_powervr_sgx_ioctl_vm_map_args, device_addr, 0x08, 8)
FIELD(drm_powervr_sgx_ioctl_vm_map_args, handle, 0x10, 4)
FIELD(drm_powervr_sgx_ioctl_vm_map_args, _padding_14, 0x14, 4)
FIELD(drm_powervr_sgx_ioctl_vm_map_args, offset, 0x18, 8)
FIELD(drm_powervr_sgx_ioctl_vm_map_args, size, 0x20, 8)

STRUCT(drm_powervr_sgx_ioctl_vm_unmap_args, 24)
FIELD(drm_powervr_sgx_ioctl_vm_unmap_args, vm_context_handle, 0x00, 4)
FIELD(drm_powervr_sgx_ioctl_vm_unmap_args, _padding_4, 0x04, 4)
FIELD(drm_powervr_sgx_ioctl_vm_unmap_args, device_addr, 0x08, 8)
FIELD(drm_powervr_sgx_ioctl_vm_unmap_args, size, 0x10, 8)

STRUCT(drm_powervr_sgx_ioctl_create_context_args, 24)
FIELD(drm_powervr_sgx_ioctl_create_context_args, type, 0x00, 4)
FIELD(drm_powervr_sgx_ioctl_create_context_args, flags, 0x04, 4)
FIELD(drm_powervr_sgx_ioctl_create_context_args, priority, 0x08, 4)
FIELD(drm_powervr_sgx_ioctl_create_context_args, vm_context_handle, 0x0c, 4)
FIELD(drm_powervr_sgx_ioctl_create_context_args, param_buffer_pages, 0x10, 4)
FIELD(drm_powervr_sgx_ioctl_create_context_args, handle, 0x14, 4)

STRUCT(drm_powervr_sgx_ioctl_destroy_context_args, 8)
FIELD(drm_powervr_sgx_ioctl_destroy_context_args, handle, 0x00, 4)
FIELD(drm_powervr_sgx_ioctl_destroy_context_args, _padding_4, 0x04, 4)

STRUCT(drm_powervr_sgx_ioctl_get_context_state_args, 16)
FIELD(drm_powervr_sgx_ioctl_get_context_state_args, handle, 0x00, 4)
FIELD(drm_powervr_sgx_ioctl_get_context_state_args, state, 0x04, 4)
FIELD(drm_powervr_sgx_ioctl_get_context_state_args, reset_count, 0x08, 4)
FIELD(drm_powervr_sgx_ioctl_get_context_state_args, _padding_c, 0x0c, 4)

STRUCT(drm_powervr_sgx_ioctl_create_rt_dataset_args, 24)
FIELD(drm_powervr_sgx_ioctl_create_rt_dataset_args, context_handle, 0x00, 4)
FIELD(drm_powervr_sgx_ioctl_create_rt_dataset_args, width, 0x04, 4)
FIELD(drm_powervr_sgx_ioctl_create_rt_dataset_args, height, 0x08, 4)
FIELD(drm_powervr_sgx_ioctl_create_rt_dataset_args, samples, 0x0c, 4)
FIELD(drm_powervr_sgx_ioctl_create_rt_dataset_args, flags, 0x10, 4)
FIELD(drm_powervr_sgx_ioctl_create_rt_dataset_args, handle, 0x14, 4)

STRUCT(drm_powervr_sgx_ioctl_destroy_rt_dataset_args, 8)
FIELD(drm_powervr_sgx_ioctl_destroy_rt_dataset_args, handle, 0x00, 4)
FIELD(drm_powervr_sgx_ioctl_destroy_rt_dataset_args, _padding_4, 0x04, 4)

STRUCT(drm_powervr_sgx_ta_kick, 16)
FIELD(drm_powervr_sgx_ta_kick, cmd_stream, 0x00, 8)
FIELD(drm_powervr_sgx_ta_kick, cmd_stream_len, 0x08, 4)
FIELD(drm_powervr_sgx_ta_kick, flags, 0x0c, 4)

STRUCT(drm_powervr_sgx_sync_op, 16)
FIELD(drm_powervr_sgx_sync_op, handle, 0x00, 4)
FIELD(drm_powervr_sgx_sync_op, flags, 0x04, 4)
FIELD(drm_powervr_sgx_sync_op, value, 0x08, 8)

STRUCT(drm_powervr_sgx_bo_ref, 8)
FIELD(drm_powervr_sgx_bo_ref, handle, 0x00, 4)
FIELD(drm_powervr_sgx_bo_ref, flags, 0x04, 4)

STRUCT(drm_powervr_sgx_job, 80)
FIELD(drm_powervr_sgx_job, type, 0x00, 4)
FIELD(drm_powervr_sgx_job, context_handle, 0x04, 4)
FIELD(drm_powervr_sgx_job, flags, 0x08, 4)
FIELD(drm_powervr_sgx_job, cmd_stream_len, 0x0c, 4)
FIELD(drm_powervr_sgx_job, cmd_stream, 0x10, 8)
FIELD(drm_powervr_sgx_job, sync_ops, 0x18, 16)
FIELD(drm_powervr_sgx_job, bo_refs, 0x28, 16)
FIELD(drm_powervr_sgx_job, ta_kicks, 0x38, 16)
FIELD(drm_powervr_sgx_job, rt_dataset_handle, 0x48, 4)
FIELD(drm_powervr_sgx_job, _padding_4c, 0x4c, 4)

STRUCT(drm_powervr_sgx_ioctl_submit_jobs_args, 24)
FIELD(drm_powervr_sgx_ioctl_submit_jobs_args, jobs, 0x00, 16)
FIELD(drm_powervr_sgx_ioctl_submit_jobs_args, failed_index, 0x10, 4)
FIELD(drm_powervr_sgx_ioctl_submit_jobs_args, _padding_14, 0x14, 4)
