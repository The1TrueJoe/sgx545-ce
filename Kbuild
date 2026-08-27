# SPDX-License-Identifier: GPL-2.0
#
# PowerVR SGX545 kernel services for the Intel Atom CE5300 family.
#
# Object list is Intel's Cedarview list (staging/cdv/Makefile) with the psb
# DRM driver, the MSVDX video decoder, the mrstlfb display class, the buffer
# class and the PDUMP debug driver removed -- on CE5300 every one of those is
# either a separate driver or a debug-only build. What is left is the DDK's
# own kernel services plus our CE5300 system layer.

SGXSRC   := $(src)/src
ENVDIR   := src/services4/srvkm/env/linux
COMMONDIR:= src/services4/srvkm/common
BRIDGEDIR:= src/services4/srvkm/bridged
SGXDIR   := src/services4/srvkm/devices/sgx
SYSDIR   := src/services4/system/ce5300

ccflags-y += \
	-I$(SGXSRC)/include4 \
	-I$(SGXSRC)/services4/include \
	-I$(SGXSRC)/services4/include/env/linux \
	-I$(SGXSRC)/services4/srvkm/env/linux \
	-I$(SGXSRC)/services4/srvkm/include \
	-I$(SGXSRC)/services4/srvkm/bridged \
	-I$(SGXSRC)/services4/srvkm/bridged/sgx \
	-I$(SGXSRC)/services4/srvkm/devices/sgx \
	-I$(SGXSRC)/services4/srvkm/hwdefs \
	-I$(SGXSRC)/services4/system/include \
	-I$(SGXSRC)/services4/system/ce5300

# Core selection.
#
# SGX_CORE_REV=1014 is what the EA-3 reports. Intel shipped Cedarview as
# 10131. sgxerrata.h gives both revisions the identical erratum set (just
# FIX_HW_BRN_SAMPLE_CACHE), so the two builds differ only in the revision
# check itself. Override on the command line to test the other:
#   make SGX_CORE_REV=10131
SGX_CORE_REV ?= 1014
ccflags-y += -DSGX545 -DSUPPORT_SGX545 -DSGX_CORE_REV=$(SGX_CORE_REV)

# Device node name.
#
# This is the name the kernel registers with DRM and the name userspace passes
# to drmOpen(). It MUST match the userspace blob set you intend to pair with:
#
#   pvrsrvkm  -- Control4's DDK 1.12 blobs, as found on a stock EA-3. Their
#                libsrv_um.so carries this string literally.
#   PowerVR   -- the DDK default, and what an unmodified 1.7 build registers.
#
# Which pairing actually works is an experiment on hardware, not something
# that can be settled by reading, so it is a knob rather than a constant.
PVR_MODNAME ?= pvrsrvkm
ccflags-y += -DPVRSRV_MODNAME=\"$(PVR_MODNAME)\"

ccflags-y += \
	-DLINUX \
	-DPVR_BUILD_DIR=\"ce5300_linux\" \
	-DSERVICES4 \
	-D_XOPEN_SOURCE=600 \
	-DPVR2D_VALIDATE_INPUT_PARAMS \
	-DSUPPORT_SRVINIT \
	-DSUPPORT_SGX \
	-DSUPPORT_LINUX_X86_WRITECOMBINE \
	-DTRANSFER_QUEUE \
	-DSYS_USING_INTERRUPTS \
	-DSUPPORT_HW_RECOVERY \
	-DPVR_SECURE_HANDLES \
	-DUSE_PTHREADS \
	-DSUPPORT_SGX_EVENT_OBJECT \
	-DSUPPORT_SGX_HWPERF \
	-DSUPPORT_LINUX_X86_PAT \
	-DPVR_PROC_USE_SEQ_FILE \
	-DSUPPORT_CACHE_LINE_FLUSH \
	-DSUPPORT_CPU_CACHED_BUFFERS \
	-DDISABLE_PM \
	-DSUPPORT_SGX_NEW_STATUS_VALS \
	-DSUPPORT_PERCONTEXT_PB \
	-DBUILD=\"release\" \
	-DPVR_BUILD_TYPE=\"release\" \
	-DRELEASE

# Attach as a PCI device via DRM.
#
# Not SUPPORT_DRI_DRM_EXT: Cedarview let the psb driver own the DRM device and
# attached PVR to it, but on CE5300 nothing else wants it, so we own our own.
#
# And deliberately not PVR_LDM_MODULE/PVR_LDM_PCI_MODULE: those make module.c
# register its own pci_driver over the same id_table and define a second
# gpsPVRLDMDev. With SUPPORT_DRI_DRM and no _EXT, pvr_drm.c is the PCI driver.
ccflags-y += -DSUPPORT_DRI_DRM

# The DDK predates -Wdeclaration-after-statement being default-on and uses
# IMG_VOID/IMG_UINT32 aliases the kernel's own warnings dislike. Silence only
# what upstream noise requires; do not add to this list to hide our own bugs.
ccflags-y += -Wno-declaration-after-statement -Wno-unused-but-set-variable

pvrsrvkm-y := \
	$(ENVDIR)/osfunc.o \
	$(ENVDIR)/mutils.o \
	$(ENVDIR)/mmap.o \
	$(ENVDIR)/module.o \
	$(ENVDIR)/pdump.o \
	$(ENVDIR)/proc.o \
	$(ENVDIR)/pvr_bridge_k.o \
	$(ENVDIR)/pvr_debug.o \
	$(ENVDIR)/mm.o \
	$(ENVDIR)/event.o \
	$(ENVDIR)/osperproc.o \
	$(ENVDIR)/pvr_drm.o

pvrsrvkm-y += \
	$(COMMONDIR)/buffer_manager.o \
	$(COMMONDIR)/devicemem.o \
	$(COMMONDIR)/deviceclass.o \
	$(COMMONDIR)/handle.o \
	$(COMMONDIR)/hash.o \
	$(COMMONDIR)/metrics.o \
	$(COMMONDIR)/pvrsrv.o \
	$(COMMONDIR)/queue.o \
	$(COMMONDIR)/ra.o \
	$(COMMONDIR)/resman.o \
	$(COMMONDIR)/power.o \
	$(COMMONDIR)/mem.o \
	$(COMMONDIR)/pdump_common.o \
	$(COMMONDIR)/perproc.o \
	$(COMMONDIR)/lists.o \
	$(COMMONDIR)/mem_debug.o \
	$(COMMONDIR)/osfunc_common.o

pvrsrvkm-y += \
	$(BRIDGEDIR)/bridged_support.o \
	$(BRIDGEDIR)/bridged_pvr_bridge.o \
	$(BRIDGEDIR)/sgx/bridged_sgx_bridge.o

pvrsrvkm-y += \
	$(SGXDIR)/sgxinit.o \
	$(SGXDIR)/sgxpower.o \
	$(SGXDIR)/sgxreset.o \
	$(SGXDIR)/sgxutils.o \
	$(SGXDIR)/sgxkick.o \
	$(SGXDIR)/sgxtransfer.o \
	$(SGXDIR)/mmu.o \
	$(SGXDIR)/pb.o

pvrsrvkm-y += $(SYSDIR)/sysconfig.o

obj-m += pvrsrvkm.o
