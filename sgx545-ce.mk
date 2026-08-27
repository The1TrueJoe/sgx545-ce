################################################################################
#
# sgx545-ce — PowerVR SGX545 kernel services for the Intel Atom CE5300
#
# Installed into the kernel tree and built there, as a MODULE. It was obj-y
# while openHC built CONFIG_MODULES=n; the EA family turned modules on
# precisely so the DRM core this driver pulls in (~1.25 MiB of bzImage) lands
# in the rootfs instead of the kernel image, which is the resource CEFDK's
# bootlinux copy window actually constrains.
#
# The source still goes in through a LINUX_POST_PATCH_HOOK, the same shape as
# board/external.mk's OHC_KERNEL_DRIVERS_HOOK, because it has to be in the tree
# before the kernel configures.
#
# The driver lands at drivers/gpu/drm/sgx545ce/ and is registered by appending
# one line to drivers/gpu/drm/Makefile. Its own Kbuild carries the include
# paths and the ~30 DDK build-option defines, so nothing about the build is
# duplicated here.
#
# SGX_CORE_REV is the one knob worth knowing about, and it is NOT simply what
# the silicon reports. An EA-3 reads back core revision 1.0.14, but the value
# here has to match the microkernel the driver is paired with, because it also
# selects the layout of the SGXMKIF structures they share. Intel's Cedarview
# DDK 1.7 userspace is built for 1.0.13, so this is 1013. The full reasoning is
# in the Kbuild next to the SGX_CORE_REV definition.
#
################################################################################

SGX545_CE_VERSION = local
SGX545_CE_SITE = $(BR2_EXTERNAL_OPENHC_PATH)/../packages/sgx545-ce
SGX545_CE_SITE_METHOD = local
SGX545_CE_LICENSE = GPL-2.0
SGX545_CE_LICENSE_FILES = src/COPYING
SGX545_CE_DEPENDENCIES = linux

SGX545_CE_KDIR = drivers/gpu/drm/sgx545ce
SGX545_CE_CORE_REV = 1013

# Nothing to build or install here -- the kernel build does both. Buildroot
# still wants the package to exist so it can be selected and so licensing is
# recorded.
define SGX545_CE_BUILD_CMDS
endef

define SGX545_CE_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/tools/sgxregs.sh \
		$(TARGET_DIR)/opt/ohc/bin/sgxregs
endef

# Installed WITHOUT an S-prefix, so busybox rcS does not run it at boot.
# Nothing needs the GPU until there is a graphics application, and loading it
# costs boot time and resident DRM for nothing. The splash is unaffected: it
# draws through /dev/fb0 from ce5300-fb, which is builtin and independent of
# DRM. Symlink it to /etc/init.d/S02sgx to make it automatic.
define SGX545_CE_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(@D)/sgx-init \
		$(TARGET_DIR)/etc/init.d/sgx
endef

$(eval $(generic-package))

# ── install into the kernel tree ───────────────────────────────────────────
#
# A post-patch hook rather than a package build step, because the source has to
# be in place before the kernel configures, not after it builds.
SGX545_CE_SRCDIR = $(BR2_EXTERNAL_OPENHC_PATH)/../packages/sgx545-ce

define SGX545_CE_KERNEL_HOOK
	mkdir -p $(LINUX_DIR)/$(SGX545_CE_KDIR)
	cp -a $(SGX545_CE_SRCDIR)/src $(LINUX_DIR)/$(SGX545_CE_KDIR)/
	cp -a $(SGX545_CE_SRCDIR)/Kbuild $(LINUX_DIR)/$(SGX545_CE_KDIR)/Kbuild
	sed -i 's/^SGX_CORE_REV ?= .*/SGX_CORE_REV ?= $(SGX545_CE_CORE_REV)/' \
		$(LINUX_DIR)/$(SGX545_CE_KDIR)/Kbuild
	if ! grep -q 'sgx545ce' $(LINUX_DIR)/drivers/gpu/drm/Makefile; then \
		echo '' >> $(LINUX_DIR)/drivers/gpu/drm/Makefile; \
		echo '# openHC: PowerVR SGX545 (Intel CE5300), installed by sgx545-ce.mk.' \
			>> $(LINUX_DIR)/drivers/gpu/drm/Makefile; \
		echo 'obj-y += sgx545ce/' >> $(LINUX_DIR)/drivers/gpu/drm/Makefile; \
		echo 'openHC: registered drivers/gpu/drm/sgx545ce'; \
	fi
endef

ifeq ($(BR2_PACKAGE_SGX545_CE),y)
LINUX_POST_PATCH_HOOKS += SGX545_CE_KERNEL_HOOK
endif
