# Compile-test environment for the SGX545 kernel driver.
#
# Host is arm64 macOS, target is i686 Linux, so we cross-compile rather than
# emulate: gcc-i686-linux-gnu on a native arm64 Debian is many times faster
# than an amd64 image under QEMU, and produces the same diagnostics.
#
#   docker build -f dev/Dockerfile -t sgx545-dev .
#   docker run --rm -v "$PWD:/src" sgx545-dev make -C /src
FROM debian:bookworm-slim

ARG KVER=7.1.8

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-i686-linux-gnu libc6-dev-i386-cross \
        build-essential bc bison flex libssl-dev libelf-dev \
        wget xz-utils rsync cpio kmod ca-certificates python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /k
RUN wget -q "https://cdn.kernel.org/pub/linux/kernel/v${KVER%%.*}.x/linux-${KVER}.tar.xz" \
 && tar xf "linux-${KVER}.tar.xz" && rm "linux-${KVER}.tar.xz" \
 && ln -s "linux-${KVER}" linux

# A minimal i386 config that still has everything the driver touches: PCI,
# PAT/MTRR write-combining, procfs, and loadable modules. openHC itself builds
# the driver in-tree (CONFIG_MODULES=n), but iterating on a module is much
# faster and exercises the identical compile path.
ENV ARCH=i386 CROSS_COMPILE=i686-linux-gnu-
RUN cd linux && make -s ARCH=i386 CROSS_COMPILE=i686-linux-gnu- tinyconfig \
 && ./scripts/config \
      -e MODULES -e MODULE_UNLOAD -e PCI -e MTRR -e X86_PAT \
      -e PROC_FS -e SYSFS -e DEBUG_FS -e MMU \
      -e PRINTK -e BUG -e ELF_CORE -e BINFMT_ELF \
      -e SMP -e HIGHMEM4G -e PCI_MSI \
 && make -s ARCH=i386 CROSS_COMPILE=i686-linux-gnu- olddefconfig \
 && make -s -j"$(nproc)" ARCH=i386 CROSS_COMPILE=i686-linux-gnu- modules_prepare

ENV KERNELDIR=/k/linux
WORKDIR /src
