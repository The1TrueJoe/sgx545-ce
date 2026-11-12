.. SPDX-License-Identifier: GPL-2.0-only OR MIT

====
UAPI
====
The sources associated with this section can be found in ``powervr_sgx_drm.h``.

.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :doc: PowerVR SGX UAPI

OBJECT ARRAYS
=============
.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :identifiers: drm_powervr_sgx_obj_array

IOCTLS
======
.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :doc: PowerVR SGX IOCTL interface

.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :identifiers: PVR_SGX_IOCTL

DEV_QUERY
---------
.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :doc: PowerVR SGX IOCTL DEV_QUERY interface

.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :identifiers: drm_powervr_sgx_dev_query
                 drm_powervr_sgx_ioctl_dev_query_args
                 drm_powervr_sgx_dev_query_gpu_info
                 drm_powervr_sgx_dev_query_limits
                 drm_powervr_sgx_dev_query_commands
                 drm_powervr_sgx_dev_query_quirks
                 drm_powervr_sgx_heap_id
                 drm_powervr_sgx_heap
                 drm_powervr_sgx_dev_query_heap_info

CREATE_BO
---------
.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :doc: PowerVR SGX IOCTL CREATE_BO interface

.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :identifiers: drm_powervr_sgx_ioctl_create_bo_args
                 drm_powervr_sgx_ioctl_get_bo_mmap_offset_args

UPLOAD_CODE
-----------
.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :doc: PowerVR SGX IOCTL UPLOAD_CODE interface

.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :identifiers: drm_powervr_sgx_ioctl_upload_code_args

VM CONTEXTS
-----------
.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :doc: PowerVR SGX IOCTL VM context interfaces

.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :identifiers: drm_powervr_sgx_ioctl_create_vm_context_args
                 drm_powervr_sgx_ioctl_destroy_vm_context_args

VM_BIND
-------
.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :doc: VM_BIND operations

.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :doc: Flags for struct drm_powervr_sgx_ioctl_vm_bind_args

.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :identifiers: drm_powervr_sgx_vm_bind_op
                 drm_powervr_sgx_ioctl_vm_bind_args

CONTEXTS
--------
.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :doc: PowerVR SGX IOCTL CREATE_CONTEXT interface

.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :identifiers: drm_powervr_sgx_ctx_type
                 drm_powervr_sgx_ctx_priority
                 drm_powervr_sgx_ioctl_create_context_args
                 drm_powervr_sgx_ioctl_destroy_context_args
                 drm_powervr_sgx_context_state
                 drm_powervr_sgx_ioctl_get_context_state_args

RT DATASETS
-----------
.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :doc: PowerVR SGX IOCTL CREATE_RT_DATASET interface

.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :identifiers: drm_powervr_sgx_ioctl_create_rt_dataset_args
                 drm_powervr_sgx_ioctl_destroy_rt_dataset_args

CLIENT COMMANDS
---------------
.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :doc: PowerVR SGX client commands

.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :identifiers: drm_powervr_sgx_cmd_head
                 drm_powervr_sgx_ext
                 drm_powervr_sgx_cmd_render
                 drm_powervr_sgx_cmd_ta_kick
                 drm_powervr_sgx_cmd_transfer
                 drm_powervr_sgx_ext_sgx545_render
                 drm_powervr_sgx_ext_sgx545_ta_kick

SUBMIT_JOBS
-----------
.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :doc: PowerVR SGX IOCTL SUBMIT_JOBS interface

.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :doc: Flags for struct drm_powervr_sgx_sync_op

.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :doc: Flags for struct drm_powervr_sgx_cache_range

.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :doc: Flags for &drm_powervr_sgx_job.flags

.. kernel-doc:: include/uapi/drm/powervr_sgx_drm.h
   :identifiers: drm_powervr_sgx_sync_op
                 drm_powervr_sgx_cache_range
                 drm_powervr_sgx_job_type
                 drm_powervr_sgx_job
                 drm_powervr_sgx_ioctl_submit_jobs_args
