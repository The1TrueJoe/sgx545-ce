/* SPDX-License-Identifier: MIT */
/* Copyright © 2026 Joseph Telaak */

/*
 * The layout of every struct in sgx_drm_draft.h as literal numbers. check_layout.c
 * turns each line into static asserts; gen_table.py turns it into the table in
 * docs/UAPI.md. FIELD(struct, member, offset, size) lines must be in member order
 * and cover the whole struct.
 */

STRUCT(drm_sgx545_obj_array, 16)
FIELD(drm_sgx545_obj_array, stride, 0x00, 4)
FIELD(drm_sgx545_obj_array, count, 0x04, 4)
FIELD(drm_sgx545_obj_array, array, 0x08, 8)

STRUCT(drm_sgx545_dev_query_gpu_info, 16)
FIELD(drm_sgx545_dev_query_gpu_info, core_id, 0x00, 4)
FIELD(drm_sgx545_dev_query_gpu_info, core_revision, 0x04, 4)
FIELD(drm_sgx545_dev_query_gpu_info, fw_interface_version, 0x08, 4)
FIELD(drm_sgx545_dev_query_gpu_info, _padding_c, 0x0c, 4)

STRUCT(drm_sgx545_dev_query_limits, 32)
FIELD(drm_sgx545_dev_query_limits, rt_max_width, 0x00, 4)
FIELD(drm_sgx545_dev_query_limits, rt_max_height, 0x04, 4)
FIELD(drm_sgx545_dev_query_limits, rt_max_samples, 0x08, 4)
FIELD(drm_sgx545_dev_query_limits, pb_min_pages, 0x0c, 4)
FIELD(drm_sgx545_dev_query_limits, pb_default_pages, 0x10, 4)
FIELD(drm_sgx545_dev_query_limits, pb_max_pages, 0x14, 4)
FIELD(drm_sgx545_dev_query_limits, max_ta_kicks, 0x18, 4)
FIELD(drm_sgx545_dev_query_limits, _padding_1c, 0x1c, 4)

STRUCT(drm_sgx545_dev_query_quirks, 16)
FIELD(drm_sgx545_dev_query_quirks, quirks, 0x00, 8)
FIELD(drm_sgx545_dev_query_quirks, count, 0x08, 2)
FIELD(drm_sgx545_dev_query_quirks, musthave_count, 0x0a, 2)
FIELD(drm_sgx545_dev_query_quirks, _padding_c, 0x0c, 4)

STRUCT(drm_sgx545_heap, 32)
FIELD(drm_sgx545_heap, base, 0x00, 8)
FIELD(drm_sgx545_heap, size, 0x08, 8)
FIELD(drm_sgx545_heap, flags, 0x10, 4)
FIELD(drm_sgx545_heap, page_size_log2, 0x14, 4)
FIELD(drm_sgx545_heap, usse_base_index, 0x18, 4)
FIELD(drm_sgx545_heap, usse_window_log2, 0x1c, 4)

STRUCT(drm_sgx545_dev_query_heap_info, 16)
FIELD(drm_sgx545_dev_query_heap_info, heaps, 0x00, 16)

STRUCT(drm_sgx545_ioctl_dev_query_args, 16)
FIELD(drm_sgx545_ioctl_dev_query_args, type, 0x00, 4)
FIELD(drm_sgx545_ioctl_dev_query_args, size, 0x04, 4)
FIELD(drm_sgx545_ioctl_dev_query_args, pointer, 0x08, 8)

STRUCT(drm_sgx545_ioctl_create_bo_args, 24)
FIELD(drm_sgx545_ioctl_create_bo_args, size, 0x00, 8)
FIELD(drm_sgx545_ioctl_create_bo_args, handle, 0x08, 4)
FIELD(drm_sgx545_ioctl_create_bo_args, _padding_c, 0x0c, 4)
FIELD(drm_sgx545_ioctl_create_bo_args, flags, 0x10, 8)

STRUCT(drm_sgx545_ioctl_get_bo_mmap_offset_args, 16)
FIELD(drm_sgx545_ioctl_get_bo_mmap_offset_args, handle, 0x00, 4)
FIELD(drm_sgx545_ioctl_get_bo_mmap_offset_args, _padding_4, 0x04, 4)
FIELD(drm_sgx545_ioctl_get_bo_mmap_offset_args, offset, 0x08, 8)

STRUCT(drm_sgx545_ioctl_create_vm_context_args, 8)
FIELD(drm_sgx545_ioctl_create_vm_context_args, handle, 0x00, 4)
FIELD(drm_sgx545_ioctl_create_vm_context_args, _padding_4, 0x04, 4)

STRUCT(drm_sgx545_ioctl_destroy_vm_context_args, 8)
FIELD(drm_sgx545_ioctl_destroy_vm_context_args, handle, 0x00, 4)
FIELD(drm_sgx545_ioctl_destroy_vm_context_args, _padding_4, 0x04, 4)

STRUCT(drm_sgx545_ioctl_vm_map_args, 40)
FIELD(drm_sgx545_ioctl_vm_map_args, vm_context_handle, 0x00, 4)
FIELD(drm_sgx545_ioctl_vm_map_args, flags, 0x04, 4)
FIELD(drm_sgx545_ioctl_vm_map_args, device_addr, 0x08, 8)
FIELD(drm_sgx545_ioctl_vm_map_args, handle, 0x10, 4)
FIELD(drm_sgx545_ioctl_vm_map_args, _padding_14, 0x14, 4)
FIELD(drm_sgx545_ioctl_vm_map_args, offset, 0x18, 8)
FIELD(drm_sgx545_ioctl_vm_map_args, size, 0x20, 8)

STRUCT(drm_sgx545_ioctl_vm_unmap_args, 24)
FIELD(drm_sgx545_ioctl_vm_unmap_args, vm_context_handle, 0x00, 4)
FIELD(drm_sgx545_ioctl_vm_unmap_args, _padding_4, 0x04, 4)
FIELD(drm_sgx545_ioctl_vm_unmap_args, device_addr, 0x08, 8)
FIELD(drm_sgx545_ioctl_vm_unmap_args, size, 0x10, 8)

STRUCT(drm_sgx545_ioctl_create_context_args, 24)
FIELD(drm_sgx545_ioctl_create_context_args, type, 0x00, 4)
FIELD(drm_sgx545_ioctl_create_context_args, flags, 0x04, 4)
FIELD(drm_sgx545_ioctl_create_context_args, priority, 0x08, 4)
FIELD(drm_sgx545_ioctl_create_context_args, vm_context_handle, 0x0c, 4)
FIELD(drm_sgx545_ioctl_create_context_args, param_buffer_pages, 0x10, 4)
FIELD(drm_sgx545_ioctl_create_context_args, handle, 0x14, 4)

STRUCT(drm_sgx545_ioctl_destroy_context_args, 8)
FIELD(drm_sgx545_ioctl_destroy_context_args, handle, 0x00, 4)
FIELD(drm_sgx545_ioctl_destroy_context_args, _padding_4, 0x04, 4)

STRUCT(drm_sgx545_ioctl_get_context_state_args, 16)
FIELD(drm_sgx545_ioctl_get_context_state_args, handle, 0x00, 4)
FIELD(drm_sgx545_ioctl_get_context_state_args, state, 0x04, 4)
FIELD(drm_sgx545_ioctl_get_context_state_args, reset_count, 0x08, 4)
FIELD(drm_sgx545_ioctl_get_context_state_args, _padding_c, 0x0c, 4)

STRUCT(drm_sgx545_ioctl_create_rt_dataset_args, 24)
FIELD(drm_sgx545_ioctl_create_rt_dataset_args, context_handle, 0x00, 4)
FIELD(drm_sgx545_ioctl_create_rt_dataset_args, width, 0x04, 4)
FIELD(drm_sgx545_ioctl_create_rt_dataset_args, height, 0x08, 4)
FIELD(drm_sgx545_ioctl_create_rt_dataset_args, samples, 0x0c, 4)
FIELD(drm_sgx545_ioctl_create_rt_dataset_args, flags, 0x10, 4)
FIELD(drm_sgx545_ioctl_create_rt_dataset_args, handle, 0x14, 4)

STRUCT(drm_sgx545_ioctl_destroy_rt_dataset_args, 8)
FIELD(drm_sgx545_ioctl_destroy_rt_dataset_args, handle, 0x00, 4)
FIELD(drm_sgx545_ioctl_destroy_rt_dataset_args, _padding_4, 0x04, 4)

STRUCT(drm_sgx545_cmd_ta_kick, 16)
FIELD(drm_sgx545_cmd_ta_kick, vdm_base, 0x00, 8)
FIELD(drm_sgx545_cmd_ta_kick, ta_flags2, 0x08, 4)
FIELD(drm_sgx545_cmd_ta_kick, _padding_c, 0x0c, 4)

STRUCT(drm_sgx545_cmd_render, 72)
FIELD(drm_sgx545_cmd_render, depth_addr, 0x00, 8)
FIELD(drm_sgx545_cmd_render, stencil_addr, 0x08, 8)
FIELD(drm_sgx545_cmd_render, pixel_event_pds_addr, 0x10, 8)
FIELD(drm_sgx545_cmd_render, bg_pds_secondary_addr, 0x18, 8)
FIELD(drm_sgx545_cmd_render, bg_pds_primary_addr, 0x20, 8)
FIELD(drm_sgx545_cmd_render, pixel_event_pds_rows, 0x28, 4)
FIELD(drm_sgx545_cmd_render, bg_pds_secondary_rows, 0x2c, 4)
FIELD(drm_sgx545_cmd_render, bg_pds_primary_rows, 0x30, 4)
FIELD(drm_sgx545_cmd_render, zls_control, 0x34, 4)
FIELD(drm_sgx545_cmd_render, flags_3d, 0x38, 4)
FIELD(drm_sgx545_cmd_render, bg_depth, 0x3c, 4)
FIELD(drm_sgx545_cmd_render, depth_bias_units, 0x40, 4)
FIELD(drm_sgx545_cmd_render, depth_bias_factor, 0x44, 4)

STRUCT(drm_sgx545_cmd_transfer, 48)
FIELD(drm_sgx545_cmd_transfer, bg_block_addr, 0x00, 8)
FIELD(drm_sgx545_cmd_transfer, pixel_event_pds_addr, 0x08, 8)
FIELD(drm_sgx545_cmd_transfer, pixel_event_pds_rows, 0x10, 4)
FIELD(drm_sgx545_cmd_transfer, flags, 0x14, 4)
FIELD(drm_sgx545_cmd_transfer, first_tile_x, 0x18, 4)
FIELD(drm_sgx545_cmd_transfer, first_tile_y, 0x1c, 4)
FIELD(drm_sgx545_cmd_transfer, last_tile_x, 0x20, 4)
FIELD(drm_sgx545_cmd_transfer, last_tile_y, 0x24, 4)
FIELD(drm_sgx545_cmd_transfer, bg_depth, 0x28, 4)
FIELD(drm_sgx545_cmd_transfer, _padding_2c, 0x2c, 4)

STRUCT(drm_sgx545_sync_op, 16)
FIELD(drm_sgx545_sync_op, handle, 0x00, 4)
FIELD(drm_sgx545_sync_op, flags, 0x04, 4)
FIELD(drm_sgx545_sync_op, value, 0x08, 8)

STRUCT(drm_sgx545_bo_ref, 8)
FIELD(drm_sgx545_bo_ref, handle, 0x00, 4)
FIELD(drm_sgx545_bo_ref, flags, 0x04, 4)

STRUCT(drm_sgx545_job, 80)
FIELD(drm_sgx545_job, type, 0x00, 4)
FIELD(drm_sgx545_job, context_handle, 0x04, 4)
FIELD(drm_sgx545_job, flags, 0x08, 4)
FIELD(drm_sgx545_job, cmd_stream_len, 0x0c, 4)
FIELD(drm_sgx545_job, cmd_stream, 0x10, 8)
FIELD(drm_sgx545_job, sync_ops, 0x18, 16)
FIELD(drm_sgx545_job, bo_refs, 0x28, 16)
FIELD(drm_sgx545_job, ta_kicks, 0x38, 16)
FIELD(drm_sgx545_job, rt_dataset_handle, 0x48, 4)
FIELD(drm_sgx545_job, _padding_4c, 0x4c, 4)

STRUCT(drm_sgx545_ioctl_submit_jobs_args, 24)
FIELD(drm_sgx545_ioctl_submit_jobs_args, jobs, 0x00, 16)
FIELD(drm_sgx545_ioctl_submit_jobs_args, failed_index, 0x10, 4)
FIELD(drm_sgx545_ioctl_submit_jobs_args, _padding_14, 0x14, 4)
