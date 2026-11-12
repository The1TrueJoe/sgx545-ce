# SPDX-License-Identifier: GPL-2.0-only
# Copyright © 2026 Joseph Telaak

# Out of tree: make -C <kernel build dir> M=$PWD CONFIG_DRM_POWERVR_SGX=m
ifneq ($(KERNELRELEASE),)
ccflags-y += -I$(src)/include -I$(srctree)/include/uapi/drm
CFLAGS_pvrsgx_trace.o := -I$(src)

powervr_sgx-y := \
	pvrsgx_ccb.o \
	pvrsgx_core.o \
	pvrsgx_device.o \
	pvrsgx_drv.o \
	pvrsgx_dump.o \
	pvrsgx_fw.o \
	pvrsgx_gem.o \
	pvrsgx_host.o \
	pvrsgx_job.o \
	pvrsgx_kobj.o \
	pvrsgx_mmu.o \
	pvrsgx_pds.o \
	pvrsgx_prime.o \
	pvrsgx_render.o \
	pvrsgx_transfer.o \
	pvrsgx_trace.o \
	pvrsgx_usse.o \
	pvrsgx_validate.o \
	pvrsgx_vm.o \
	pvrsgx_cmd_checks.o \
	pvrsgx_cmd.o \
	pvrsgx_cmd_defs.o

powervr_sgx-$(CONFIG_DEBUG_FS) += pvrsgx_debugfs.o
powervr_sgx-$(CONFIG_PM) += pvrsgx_pm.o
powervr_sgx-$(CONFIG_DRM_POWERVR_SGX_GMA500) += pvrsgx_aux.o
powervr_sgx-$(CONFIG_DRM_POWERVR_SGX_OF) += pvrsgx_of.o

obj-$(CONFIG_DRM_POWERVR_SGX) += powervr_sgx.o
else
KDIR ?= /lib/modules/$(shell uname -r)/build

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) CONFIG_DRM_POWERVR_SGX=m modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
endif
