# Compile-test environment for the SGX545 kernel driver.
#
# Host is arm64 macOS, target is i686 Linux, so we cross-compile rather than
# emulate: gcc-i686-linux-gnu on a native arm64 base is many times faster than
# an amd64 image under QEMU and produces identical diagnostics.
#
#   make image      # build this
#   make            # build the module against it
#   make shell      # poke around inside
FROM debian:bookworm-slim

ARG KVER=7.1.8

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-i686-linux-gnu \
        build-essential bc bison flex libssl-dev libelf-dev \
        wget xz-utils rsync cpio kmod ca-certificates python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /k
RUN wget -q "https://cdn.kernel.org/pub/linux/kernel/v${KVER%%.*}.x/linux-${KVER}.tar.xz" \
 && tar xf "linux-${KVER}.tar.xz" && rm "linux-${KVER}.tar.xz" \
 && ln -s "linux-${KVER}" linux

ENV ARCH=i386 CROSS_COMPILE=i686-linux-gnu-

# A minimal i386 config that still carries everything the driver touches: PCI,
# PAT/MTRR write-combining, procfs, DRM and loadable modules. openHC itself
# builds the driver in-tree with CONFIG_MODULES=n, but iterating on a module
# is much faster and exercises the identical compile path.
RUN cd linux \
 && make -s ARCH=i386 CROSS_COMPILE=i686-linux-gnu- tinyconfig \
 && ./scripts/config \
      -e MODULES -e MODULE_UNLOAD -e PCI -e MTRR -e X86_PAT \
      -e PROC_FS -e SYSFS -e MMU -e PRINTK -e BUG \
      -e BINFMT_ELF -e SMP -e HIGHMEM4G -e PCI_MSI -e NET \
      -e DRM -e AGP -e FB -e FB_CORE -e I2C \
 && make -s ARCH=i386 CROSS_COMPILE=i686-linux-gnu- olddefconfig \
 && make -s -j"$(nproc)" ARCH=i386 CROSS_COMPILE=i686-linux-gnu- modules_prepare

ENV KERNELDIR=/k/linux
WORKDIR /src
