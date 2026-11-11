.. SPDX-License-Identifier: GPL-2.0-only OR MIT

===============================================
drm/powervr_sgx PowerVR SGX545 GPU Driver
===============================================

``powervr_sgx`` is a render-only DRM driver for the PowerVR SGX545. It binds
the GPU where it is a PCI function of its own, as in the Intel CE5300
(``8086:089b``, core revision 1.0.14), and on Cedarview (GMA3600/3650, core
revision 1.0.13), where the SGX shares a PCI function with the display engine,
as the auxiliary device ``gma500_gfx.sgx`` that gma500 publishes. Scanout goes
through a separate KMS driver, and buffers move between the two as dma-bufs.

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

The supported cores are in a table (``pvrsgx_core.c``). Each core design has
an entry with its ID, the registers whose offsets or fields differ between
SGX5xx cores, its features, instruction set, directory lists, address width
and init script. Each part of it has an entry with its revision, clock and
firmware file. The bus tells the driver which core to expect, since even the
core ID register moves between cores: the PCI ID for the CE5300, the gma500
auxiliary device for Cedarview. The driver refuses a core that isn't in the
table before writing any register.
For the CE5300 the file is ``powervr_sgx/sgx545_ukernel.bin``. It starts
with a header giving the host interface version, feature bits, the image's CRC,
its entry points and the immediates the driver fills with buffer addresses.
The driver only loads an image that declares secure mode, in which no client
page directory maps the firmware's memory. After the start it asks the
microkernel for its version and features.

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
``UPLOAD_CODE``, which validates them (see `Shader validation`_).

dma-bufs are exported and imported with the shmem helpers. An imported object
maps in any heap a data object maps in. A CPU mapping of an exported dma-buf
needs ``DRM_POWERVR_SGX_BO_ALLOW_CPU_ACCESS``, as a mapping through the DRM
file does.

Shader validation
=================

The GPU's page tables keep a client's memory accesses to its own VM, but a USSE
program can also read and write the core's own registers (``ldr``, ``str``),
the page directory bases among them, which the page tables can't stop. So the
kernel decides which USSE code may run for a client.

**Where client code runs from.** A USSE task starts at one of the 16 USE code
bases plus an offset, both taken from a ``DOUTU`` word of a PDS program. PDS
programs and their data are client memory and are not checked, so a client can
name any base and any offset. The offset, like the absolute targets inside
USSE code, reaches at most 2 MiB past the base. The kernel programs bases 0-9
onto the pixel shader heap, 10-13 onto the vertex shader heap, 14 the same as
0, and 15 onto the microkernel's code, which no client page directory maps.
From bases 0-14, everything within reach is a USSE heap or an address range no
client can map. The USSE heaps take only code objects. So a client task runs
code ``UPLOAD_CODE`` checked, or faults.

**What UPLOAD_CODE checks.** Every 8-byte word of an upload, whether or not
the program's branches reach it, against a table generated from the isaspec
description of the instruction set that Mesa's compiler uses
(``pvrsgx_usse_table.h``):

- The word must be exactly one known form. Fields the compilers leave at one
  value are pinned to it.
- Refused outright: register-file access (``ldr``, ``str``); everything that
  sets or uses the link address (``setl``, ``savl``, ``lapc``, a branch with
  ``link``), since together they make a computed jump; the absolute branch
  ``ba``; ``phas`` with a register operand; the mutexes ``lock`` and
  ``release``, which can stall other tasks; the operand-stepping controls
  whose effect on operand numbers is not known (``smbo``, ``smr``, ``smoa``,
  ``imo`` and the register forms of all four with ``smlsi``); dual issue
  (``fdual``), whose second operation's operands aren't checked; and forms
  this core doesn't have or whose meaning is not known.
- ``br``: the target must be inside the object.
- ``phas.i``: a target other than 0 counts from the USSE heap's base and must
  be inside the object where it is mapped. The object must be mapped when such
  code is uploaded, at one place only, and can't be mapped anywhere else
  afterwards.
- Each operand: no writes to constants or globals; no reads of the globals
  past ``g45``, which are not documented; internal registers ``i0``-``i3``
  and index registers ``idx1``-``idx2`` only.

**What is not checked, and why.** Memory accesses (``ld``, ``st``, texture
samples, the pixel back end's tile writes through ``emit``, PDS DMA) go
through the job's page directory, which maps only the client's own objects
and the parameter buffer, and none of the firmware's. The GPU can't modify a
code object once it is checked. Code objects are mapped read-only, and the MMU
faults USSE stores and pixel back end writes to such pages.

**Known gaps.**

- The parameter buffer is one per device and mapped read-write into every
  client VM, so one client's shader can read or corrupt the scenes other
  clients are binning.
- The following are expected but not yet tested on hardware: that a client
  task's instruction fetch from base 15 faults, and that the read-only PTE
  bit also stops the depth/stencil store and the tile accelerator.
- Indexed operands and repeated instructions can step an operand past the
  128 registers its field names. Whether the hardware keeps such accesses
  inside the task's own registers is not documented. The compilers need
  both.

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

A job that doesn't finish within the scheduler's timeout, or that takes a GPU
page fault, resets the GPU. The SGX545 raises no interrupt for a page fault, so
the driver checks the MMU's fault status while jobs are on the GPU. The
driver halts the cores and takes a devcoredump (registers, the microkernel's
control blocks, the command buffer and the hung job's structures). Then it
fails the jobs that were on the GPU: the guilty one with -ETIME, or -EFAULT
after a fault, and the others with -ECANCELED. It restarts the microkernel and
marks the guilty context. Its later submissions fail, and other contexts carry
on.

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
    firmware state, command buffer offsets, the host control block and,
    while the GPU is awake, the core's registers.

``ctlblk``
    the microkernel's TA/3D and host control blocks.

Module parameters
=================

``job_timeout_ms``
    how long a job may run before the GPU is reset (default 2000).
