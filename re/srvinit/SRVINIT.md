# srvinit: the SGX545 brought up without libsrv_init

`srvinit` is a static x86_64 program (musl, built on `re/replay/pvr.c`)
that does what the DDK's `SrvInit()` does. It talks to the kernel driver
through its native 64-bit bridge, so no compat layer is involved, and no
Imagination library is loaded. The only Imagination bytes it uses are the
microkernel firmware file, which `fetch-firmware.sh` pulls from your own
copy of the Cedarview DDK 1.7.862890.

    re/srvinit/fetch-firmware.sh [libsrv_init.so | DDK dir | Cedarview tarball] [out]
    re/srvinit/build.sh
    srvinit [-f /lib/firmware/sgx545-ukernel-1.7.862890.bin]     # as root, once per boot

Every value below is checked against the blob: a pvrtrace of the real
`SrvInit` on an EA3 (`re/trace`, now with an `init` snapshot before
`SGX_DEVINITPART2`) gave the bytes the init process wrote and the bridge it
sent. `srvinit -n plan outdir` builds the same thing offline from the traced
addresses, and `check.py diff` compares the two. Result: all 15 buffers and
every `SGX_BRIDGE_INIT_INFO` field are identical.

    python3 re/srvinit/check.py plan trace.bin > plan.txt
    srvinit -n plan.txt -f fw.bin out/          # on any x86_64 Linux, no GPU touched
    python3 re/srvinit/check.py diff trace.bin out/

## On hardware (EA3, 2026-10-28)

Run from a cold boot, before anything else touched the GPU, in place of
`run.sh sgxinit`:

- `srvinit` returned 0. dmesg shows the same `SysFinalise: SGX revision =
  1.0.14` and `SGXInit: HW core rev (1000e)…` lines as a blob boot, and no
  errors.
- The blob-free `re/replay` frame ran next. Its back buffer was identical to
  the reference (md5 `56f8466a…`, the frame that matches the probe pixel for
  pixel).
- The i386 blob client `gles2tri` then drew its triangle: the centre pixel is
  `rgba(0,255,0,255)`, the shader's green, and the swap went to the panel.
- The replay again, after the blob client: still identical.

The only process involved was the static musl binary. `libsrv_init` and
`libsrv_um` were never loaded.

One thing the offline check could not catch, and the first hardware run hit:
the driver's dispatch table registers `SGX_DEVINITPART2` with an OUT size of
`sizeof(PVRSRV_BRIDGE_RETURN)`. That is 16 bytes on x86_64, while the handler
only writes the 8-byte `PVRSRV_BRIDGE_OUT_SGXDEVINITPART2`. i386 never
noticed, because both are 8 there. A native caller must pass 16 bytes, or
the call fails with -22 and leaves the driver unusable until a reboot. srvinit
passes a union of the two. The cleaner fix is in the driver's table.

Where things come from:
- **firmware**: bytes from the firmware file;
- **computed**: our own code works it out from what the kernel returned, such
  as allocation addresses, heap bases or the page directory;
- **constant**: a fixed number that libsrv_init 1.7.862890 hard-codes, read
  from its disassembly. Most of these describe this microkernel build, such
  as its entry points and struct sizes. A different microkernel would need
  new ones.

Sources: `libsrv_init.so.1.7.862890` decompiled with Ghidra (`FUN_000108a0`,
the per-device init, plus `SrvInit` around it). The register and value
arguments of its script helper (regparm) come from the disassembly. The
values were then confirmed against the trace.

## The firmware file

`sgx545-ukernel-1.7.862890.bin` is libsrv_init's `.rodata` from 0x1e20 to its
end at 0xef68, byte for byte: 53576 bytes, sha256
`d7e550e9199e3e4107dc440ce2676ffd1dd3aaf7af4f51a1002e02686bd12861`. It comes
from `libsrv_init.so.1.7.862890` (sha256 `4d3df850…53b6d`), which ships in
`cedarview-graphics-drivers_20120717.orig.tar.gz` (sha256 `c23e7261…8fd83`,
the same pin as openHC's `sgx545-um` package). The extraction script checks
all three hashes and gets the same output from the library, from a DDK
directory, and from the downloaded tarball.

| offset | size | what |
|---|---|---|
| 0x000 | 0x100 | PDS program `pds3` (template) |
| 0x100 | 0x068 | PDS program `pds4` (template) |
| 0x168 | 0x018 | PDS program `pds0` (template) |
| 0x180 | 0x038 | PDS program `pds1` (template) |
| 0x1b8 | 0x018 | PDS program `pds2` (template) |
| 0x1d0 | 0x010 | padding |
| 0x1e0 | 0xcf68 | USSE microkernel |

The rest of `.rodata` is the app-hint name `HWPerfFlags` and a switch jump
table, neither of which is firmware.

## The bridge sequence

The order is the same as the blob's. The trace shows libsrv_um's own calls in
between: one `MHANDLE_TO_MMAP_DATA` and mmap per allocation, and the
releases.

1. `CONNECT_SERVICES`, then `INITSRV_CONNECT`, which marks us as the init
   process and only works once per boot.
2. `ENUM_DEVICES`: pick the device with type SGX (7) and class 3D, then
   `ACQUIRE_DEVICEINFO` (its index, type UNKNOWN, as SrvInit does).
3. `SGXINFO_FOR_SRVINIT`: returns the page directory's device-physical
   address and the kernel memory context's heaps. No memory context is
   created; every allocation goes into these heaps.
4. 15 × `ALLOC_DEVICEMEM` + map, in the order below. The device addresses
   depend on that order, and `re/replay` relies on them matching the blob's.
5. Write the buffers.
6. Per buffer, `RELEASE_MMAP_DATA` + munmap, as libsrv_um does before the
   kernel takes ownership.
7. `SGX_DEVINITPART2` with `SGX_BRIDGE_INIT_INFO`. The kernel looks up and
   releases every handle, then keeps the scripts and values.
8. `INITSRV_DISCONNECT(bInitSuccesful = 1)`: this is where the kernel runs
   `SGXInitialise`, which resets the core, runs the scripts, and starts the
   microkernel. Then `DISCONNECT_SERVICES`.

On any failure, srvinit sends `INITSRV_DISCONNECT(0)`, as SrvInit does.

## Allocations

Attribute flags: `RW` = READ|WRITE|CACHE_CONSISTENT|NO_SYNCOBJ|EDM_PROTECT
(0x4f); `MP` adds HAP_MULTI_PROCESS (0x2004f); `RO` = READ|NO_SYNCOBJ (9).
Sizes, alignments and flags are all **constant**. The addresses shown are the
ones the EA3 gave, the blob and srvinit alike; srvinit itself always uses
whatever the kernel returns.

| # | buffer | heap | flags | size | align | address | handle field |
|---|---|---|---|---|---|---|---|
| 0 | kernel CCB | kernel data | RW | 0x2000 | 0x1000 | 0x32000000 | hKernelCCBMemInfo |
| 1 | CCB control | kernel data | RW | 0x8 | 0x1000 | 0x32002000 | hKernelCCBCtlMemInfo |
| 2 | event kicker | kernel data | RW | 0x4 | 0x10 | 0x32002010 | hKernelCCBEventKickerMemInfo |
| 3 | TA3D control | kernel data | MP | 0xec | 0x20 | 0x32003000 | hKernelSGXTA3DCtlMemInfo |
| 4 | host control | kernel data | MP | 0x44 | 0x20 | 0x32003100 | hKernelSGXHostCtlMemInfo |
| 5 | misc info | kernel data | MP | 0x54 | 0x20 | 0x32003160 | hKernelSGXMiscMemInfo |
| 6 | HWPerf buffer | kernel data | RW | 0x640c | 0x20 | 0x32004000 | hKernelHWPerfCBMemInfo |
| 7 | TA signature regs | kernel data | RW | 0x620 | 0x20 | 0x3200a420 | hKernelTASigBufferMemInfo |
| 8 | 3D signature regs | kernel data | RW | 0x2088 | 0x20 | 0x3200b000 | hKernel3DSigBufferMemInfo |
| 9 | microkernel | kernel code | RO | 0xcf68 | 0x8 | 0x30000000 | asInitMemHandles[0] |
| 10 | pds0 | kernel code | RO | 0x18 | 0x10 | 0x3000cf70 | asInitMemHandles[1] |
| 11 | pds1 | kernel code | RO | 0x38 | 0x10 | 0x3000cf90 | asInitMemHandles[2] |
| 12 | pds2 | kernel code | RO | 0x18 | 0x10 | 0x3000cfd0 | asInitMemHandles[3] |
| 13 | pds3 | kernel code | RO | 0x100 | 0x10 | 0x3000d000 | asInitMemHandles[4] |
| 14 | pds4 | kernel code | RO | 0x68 | 0x10 | 0x3000d100 | asInitMemHandles[5] |

Note that the 0xec buffer is the one that goes to `hKernelSGXTA3DCtlMemInfo`,
and the 0x44 one is the host control block. 0x44 is the microkernel's
`sizeof(SGXMKIF_HOST_CTL)` (the struct sizes below say so), while the GPL
header's struct is 0x48 because it carries a later `ui32OpenCLDelayCount`.
Buffers are zeroed first; the blob zeroes the same ones, and the rest come
back from the kernel already zero.

## Buffer contents

**TA3D control** (word index: value). The buffer is otherwise zero.

| word | value | source |
|---|---|---|
| 0x00 | its own address | computed |
| 0x01 | host control address | computed |
| 0x02 | CCB control address | computed |
| 0x13 | app hint `HWPerfFlags` from /etc/powervr.ini, default 0 | computed (0 on the EA3) |
| 0x16 | kernel CCB address | computed |
| 0x17 | page directory, device physical (`SGXINFO_FOR_SRVINIT`) | computed |
| 0x2b | HWPerf buffer address | computed |
| 0x2c | TA signature buffer address | computed |
| 0x2d | 3D signature buffer address | computed |

**TA signature buffer**: count 6, a zero word, then the registers 0x304,
0x26c, 0x270, 0x2a0, 0x2a4, 0x378. **3D signature buffer**: count 0x20, a zero
word, then 0x438, 0x4cc, 0x4d0, 0x4d4, 0x4d8, 0x510–0x51c, 0x524–0x544,
0x550–0x55c, 0x84c–0x858, 0x560–0x56c, 0x308, 0x30c. Both lists are
**constant**. They are the registers the microkernel samples to tell a hung
core from a busy one. The sampled values go in the space after the list.

**Microkernel**: **firmware**, with nine `limm` placeholders filled in. In
the firmware each placeholder is word 0 `0x?dbeef` and word 1 `0xfc237150`.
A 32-bit immediate is split: bits 0–20 go to word 0, bits 21–25 to word 1
bits 4–8, and bits 26–31 to word 1 bits 12–17. srvinit checks that each
placeholder is there before patching.

| offset | value | source |
|---|---|---|
| 0xbaf8 | 0x1800100 | constant |
| 0xbb10 | pds4 address >> 4 | computed |
| 0x0d60 | 0x1001000 | constant |
| 0x0d70 | pds1 address >> 4 | computed |
| 0x0d78 | TA3D control address | computed |
| 0x0db0 | 0x800100 | constant |
| 0x0dc0 | pds2 address >> 4 | computed |
| 0x0e40 | pds3 address & ~0xf | computed |
| 0x0e50 | 6 | constant |

**PDS programs**: **firmware** templates, with their data segments filled
in. `doutu(e)` is a DOUTU word 0 for microkernel offset `e`: with `off` =
microkernel address + e − kernel code heap base and `seg = off >> 19`, the
word is `((seg + 15) & 15) | 0x8000000 | ((off − seg·2^19)·2 & 0x3ffff0)`.
That is USE code base 15 (the kernel code heap, set by the script below) plus
the offset in 8-byte instructions << 4, as in `re/pds/PDS.md`. The offsets
are all **constant** (microkernel entry points); the result is **computed**.

| program | words written | code (re/pds/dis.py) |
|---|---|---|
| pds0 | [0] doutu(0xd60), [1] 2 | `movs doutu` (one row), halt |
| pds1 | [0] TA3D control address, [1] 0x3c0003f, [4] `(off·2 & 0xffff0) \| ((seg+15) & 15)` for 0xdf0 (no bit 27), [5] 2 | if in0 == 0: `movs doutd` (the TA3D address) then `movs doutu`; else halt |
| pds2 | [0] doutu(0xe20), [1] 2 | as pds0 |
| pds3 | [0] 2, [3] 0x100, [6] 0x200, [7] doutu(0x7050), [10] doutu(0x2cd0), [11] doutu(0x5670), [14] doutu(0x30) | `douta`s, then one of four DOUTUs picked by input bits |
| pds4 | [0] doutu(0x73a0), [1] 2, [3] 0x200, [4] doutu(0x2ea0), [5] 2, [7] 0x4000f, [8] doutu(0x5740), [9] 2, [10] 0x120080 | two `douta`, then one of three DOUTUs picked by `in0 & ds1[3]`, `in0 & ds1[4]` |

The words of value 2 sit next to each DOUTU word 0, so they are probably
DOUTU word 1 (**guess**). Other constants in these programs (0x100, 0x200,
0x4000f, 0x120080, 0x3c0003f) are not decoded.

Who points at which program: `pds0` is the event program (script register
0xa68 below); `pds1`, `pds2`, `pds3` and `pds4` are referenced only from the
microkernel's own immediates.

## SGX_BRIDGE_INIT_INFO

| field | value | source |
|---|---|---|
| 6 control handles, HWPerf/TA/3D signature handles | the allocations above | computed |
| `aui32HostKickAddr[11]` | TA 0x11d, TRANSFER 0x103, 2D 0, POWER 0xd4, CONTEXTSUSPEND 0xeb, CLEANUP 0x13a, GETMISCINFO 0x173, PROCESS_QUEUES 0x156, [8] 0, [9] 0x157, [10] 0 | constant (microkernel entry points, in instructions) |
| `ui32ClientBuildOptions` | 0x60218 (the kernel returned the same in `ui32KMBuildOptions`) | constant |
| `sSGXStructSizes` | CMDTA 0x2b0, CMDTA_SHARED 0x1e0, TRANSFERCMD 0x4c8, TRANSFERCMD_SHARED 0xdc, 3DREGISTERS 0xd0, HWPBDESC 0x3c, HWRENDERCONTEXT 0x44, HWRENDERDETAILS 0x1ec, HWRTDATA 0x38, HWRTDATASET 0x50, HWTRANSFERCONTEXT 0xb4, HOST_CTL 0x44, COMMAND 0x20 | constant (the microkernel's struct sizes; the kernel compares them with what the microkernel itself reports) |
| `ui32EDMTaskReg0` / `1` | 0xb14 / 0xb20 | constant (registers the kernel reads, and XORs together, to see whether the EDM is still making progress) |
| `ui32ClkGateCtl` / `2` | 0xa6aa / 0xaaaaaa | constant |
| `ui32ClkGateStatusReg` / `Mask` | 0x8 / 0xffffffdf | constant |
| `ui32CacheControl` | 2 | constant |
| `asInitDevData[8]`, `[10]`, `[11]` | 2, 0x300, 0x940 | constant (meaning not decoded) |
| `asInitDevData[12]` | PDS pixel code/data heap base (0x2e000000) | computed |
| `asInitDevData[13]` | pixel shader heap base (0x37000000) | computed |
| other `asInitDevData` | 0 | |
| `asInitMemHandles[0..5]` | microkernel, pds0..pds4 | computed |
| `asInitMemHandles[6..17]` | 0 (unused) | |

## Init scripts

Each entry is `WRITE_HW_REG offset, value`, and each script ends with `HALT`
(op 2 in this build, which has no PDUMP). Registers whose names aren't in the
GPL `sgx545defs.h` are given by offset only. Values are **constant** unless
marked.

`SGXInitialise` (at `INITSRV_DISCONNECT`, and again after every power-up or
reset) runs part 1, zeroes the event kicker, then runs part 2. After that it
kicks `EUR_CR_EVENT_KICK`. The EDM runs the event PDS program (pds0), which
starts the microkernel at offset 0xd60, and the kernel then polls
`HostCtl.ui32InitStatus` for `PVRSRV_USSE_EDM_INIT_COMPLETE`. A timeout
there only logs "Wait for uKernel initialisation failed" and carries on, so
`INITSRV_DISCONNECT` succeeding does not prove the microkernel runs. Check
dmesg, and have a client render.

**Part 1**: `0xa58 = 0`, `EUR_CR_EVENT_TIMER (0xacc) = 0`.

**Part 2**:

| register | value |
|---|---|
| 0x818, 0x810, 0x814 | 0x7ff, 1, 0xffff |
| EUR_CR_EVENT_HOST_ENABLE (0x130) | 0x4000 |
| EUR_CR_EVENT_HOST_ENABLE2 (0x110) | 0 |
| 0x13c, 0xa7c, 0xa80 | 0 |
| 0xa74 | 0x208200 |
| 0x4e8 | 0x100 |
| 0xa78, 0xaac, 0xabc | 0x2c, 0x45, 0xe |
| 0x800, 0x804 | 0x30, 0xffff |
| 0xa00 | 0x7c000 |
| EUR_CR_USE_CODE_BASE_0..9 (0xa0c–0xa30) | (pixel shader heap + i·0x80000) >> 7 \| 0x2000000: **computed** |
| EUR_CR_USE_CODE_BASE_10..13 (0xa34–0xa40) | (vertex shader heap + i·0x80000) >> 7: **computed** |
| EUR_CR_USE_CODE_BASE_15 (0xa48) | kernel code heap >> 7 \| 0x6000000: **computed** |
| EUR_CR_PDS_EXEC_BASE (0xab8) | PDS pixel heap base: **computed** |
| 0xb30 | 0x100 |
| EUR_CR_EVENT_KICKER (0xac4) | event kicker buffer address: **computed** |
| 0xa68 | pds0 address & ~0xf: **computed** (the event PDS program) |
| 0xa6c, 0xa70 | 1, 6 (pds0's control words, **guess**) |
| 0xa58 | 0x2204b00f |

(USE code base 14 is never set. Code bases 0–9 cover 5 MB of pixel shader
heap and 10–13 cover 2 MB of vertex shader heap, 512 KB each.)

**Deinit**: zero 0xa58, 0xa68, 0xa6c, 0xa70, USE_CODE_BASE_15,
EVENT_HOST_ENABLE, EVENT_HOST_ENABLE2, CLKGATECTL, CLKGATECTL2.

## What still comes from Imagination

- The USSE microkernel (0xcf68 bytes) and the five PDS programs (0x1d0
  bytes), from the firmware file.
- The constants above that describe this microkernel build: its entry points
  (host kicks, the PDS DOUTU targets), its limm patch
  sites and their fixed values, its struct sizes and build options, the
  signature register lists, and the script values for registers the GPL
  headers don't name.

The PDS programs are short and decode fully with `re/pds/dis.py`, so they
are the obvious next thing to write ourselves. That would leave only the
microkernel in the firmware file.
