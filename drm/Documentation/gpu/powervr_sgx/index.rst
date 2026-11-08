.. SPDX-License-Identifier: GPL-2.0-only OR MIT

===============================================
drm/powervr_sgx PowerVR SGX545 GPU Driver
===============================================

``powervr_sgx`` is a render-only DRM driver for the PowerVR SGX545. It binds
the GPU where it is a PCI function of its own, as in the Intel CE5300
(``8086:089b``, core revision 1.0.14). Scanout goes through a separate KMS
driver, and buffers move between the two as dma-bufs.

.. toctree::
   :maxdepth: 2
   :caption: Contents

   uapi

Firmware
========

The SGX545 runs a microkernel on its USSE, loaded with request_firmware() from
``powervr_sgx/``. The microkernel takes commands from a kernel-owned command
buffer, kicks the tile accelerator (TA) and the 3D core, and raises an
interrupt when a job is done. The driver builds the microkernel's data
structures: the host control block, the command buffer, render and transfer
contexts, render target datasets and parameter buffers. Userspace never writes
them.

Each supported core has an entry in a table (``pvrsgx_core.c``): its ID and
revision, the features the driver relies on, its clock and its firmware file.
The driver refuses a core that isn't in the table before writing any register.
For the CE5300 the file is ``powervr_sgx/sgx545_ukernel.bin``. It starts
with a header giving the host interface version, feature bits, the image's CRC,
its entry points and the immediates the driver fills with buffer addresses.
The driver only loads an image that declares secure mode, in which no client
page directory maps the firmware's memory. After the start it asks the
microkernel for its version and features.

``CONFIG_DRM_POWERVR_SGX_DDK_COMPAT`` adds the vendor DDK 1.7 microkernel for
A/B testing. That microkernel runs client work under the client's own page
directory, so with it every client directory maps the firmware's heaps.

Memory
======

Buffer objects are shmem GEM objects with pages below 4 GiB, since the GPU's
physical addresses are 32 bits wide. CPU mappings are write-combined. Every
open file creates VM contexts. Each one has its own two-level page directory
and is managed with drm_gpuvm. The client heaps sit at fixed device addresses,
reported by ``DEV_QUERY``. Kernel-owned objects live in ranges a client can't
map.

USSE programs run only from code objects. A code object has no CPU mapping and
can't be exported, and the GPU sees it read-only. Its contents arrive through
``UPLOAD_CODE``, which checks every instruction against the instruction forms
the driver knows. Register-file loads and stores, and branches out of the
object, are refused.

dma-bufs are exported and imported with the shmem helpers. An imported object
maps in any heap a data object maps in. A CPU mapping of an exported dma-buf
needs ``DRM_POWERVR_SGX_BO_ALLOW_CPU_ACCESS``, as a mapping through the DRM
file does.

Jobs
====

One drm_gpu_scheduler feeds the microkernel, and contexts are scheduler
entities. A render job is a scene of one or two TA kicks and the 3D render that
follows them. A transfer job is a blit. Userspace describes either as a command
stream, which the kernel decodes field by field before it writes the
microkernel's command. Jobs wait for and signal binary and timeline syncobjs,
and they take implicit dependencies from the reservation objects of the buffers
they reference.

Recovery
========

A job that doesn't finish within the scheduler's timeout resets the GPU. The
driver takes a devcoredump (registers, the microkernel's control blocks, the
command buffer and the hung job's structures). It then restarts the microkernel
and marks the guilty context. Its later submissions fail, and other contexts
carry on.

Power
=====

With runtime PM, a job on the GPU holds a reference. The device autosuspends
500 ms after the last one. Suspend powers the microkernel off and holds the
cores in reset; resume restarts it. The SGX function has no PCI PM capability,
so it stays in D0. While the core is stopped the driver reads none of its
registers.

Debugging
=========

debugfs, under ``/sys/kernel/debug/dri/<minor>/``, has:

``status``
    firmware state, command buffer offsets, the host control block and the
    core's registers (when powered).

``render``
    the last render job's structures, as the microkernel left them.

``render_before``
    with ``snapshot=1``, the same structures as the driver wrote them before
    the last kick.

Module parameters
=================

``firmware``
    microkernel image under ``/lib/firmware``.

``runtime_pm``
    power the microkernel off when idle (default on).

``core_clock``
    SGX core clock in Hz, for the microkernel's timer.

``share_kernel_heaps``
    map the firmware's heaps in every VM. The default is -1: on only for a
    microkernel that runs a job under the job's own page directory and can't
    reach its data otherwise. When they are shared, a client's tasks can reach
    the firmware's memory, and the driver says so at load.

``cleanup``, ``restart_quiesce``, ``reap_after_render``, ``idle_kernel_pd``,
``single_entry``, ``snapshot``, ``snap_gap_ms``
    debugging only.
