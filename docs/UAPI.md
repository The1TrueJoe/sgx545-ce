# powervr_sgx: the SGX545 DRM uAPI, design draft

The interface between `powervr_sgx`, a new Linux DRM render driver for the PowerVR SGX545,
and its Mesa driver. This is M2 step 1 of `AGENTS.md` section 5. It follows the rules in
`AGENTS.md` sections 3.1 to 3.3, and `include/uapi/drm/pvr_drm.h` is the model for its
shape.

The header is [`uapi/powervr_sgx_drm.h`](uapi/powervr_sgx_drm.h), named as
`include/uapi/drm/<driver>_drm.h` headers are. The driver (`drm/powervr_sgx`, through a
symlink) and the tests build against it. `uapi/check.sh` compiles it for i386 and x86_64 against the
literal layouts in `uapi/layout.inc`. The command streams are defined in
[`uapi/powervr_sgx_kmd_stream.xml`](uapi/powervr_sgx_kmd_stream.xml) (section 3).

Identifiers spell the driver name in full: `drm_powervr_sgx_*` and
`DRM_IOCTL_POWERVR_SGX_*`. `pvr` is the prefix of upstream's Rogue driver, whose module is
`powervr`, so a shorter `drm_pvr_sgx_*` would read as part of the Rogue uAPI (D1).

Joseph decided D1 to D10 on 2026-11-02 (section 10); this draft follows those decisions.

**Read section 7.2 first.** The whole isolation design rests on four properties of the
hardware that `docs/` does not establish.

## Evidence tags

| tag | meaning |
|---|---|
| **hardware** | Run on an EA: a replay with one field changed, or a frame of our own, compared with a prediction. STATE.md's [C]. |
| **capture** | Seen in DDK captures, or a formula checked against them. STATE.md's [D]. Includes UKERNEL.md's offline runs against captured and generated frames, which `docs/UKERNEL.md` section 10 says are not confirmed on hardware yet. |
| **gap** | `docs/` does not establish it. The text says what the draft assumes, and section 11 lists the gap. STATE.md's [G] is a gap. |
| **design** | A choice this document makes, not a fact about the GPU. |

## 1. The model

- **The kernel owns everything the microkernel reads or writes.** That covers the host
  control block, the kernel CCB, HW render and transfer contexts, their command rings,
  RT data, render details, PB descriptors and sync words. Nothing of this has a GEM
  handle or a CPU mapping in userspace. (design; `AGENTS.md` section 3.3)
- **Userspace owns what the shaders and fixed-function hardware read as data.** That
  covers VDM streams, TA state blocks, PDS and USSE programs, vertices, textures and
  surfaces. These live in its VM and are read under its page directory. (capture for
  where they live, gap for what isolates them)
- **A job is a validated copy, not a pointer.** Userspace passes the client's part of a
  command as a stream of scalars. The kernel copies it, checks it field by field and
  builds the firmware command in kernel memory. It never reads a firmware-visible structure
  back from memory the client can write. (design)
- **A word nobody can name is not client-controlled.** Where STATE.md tags a command
  word [G], the kernel writes the value the captures show and there is no uAPI field.
  When a word gets a name and a test, an extension stream carries it (section 3.1).
  (design, D5)
- **The uAPI does not expose the host interface.** Streams carry meanings, not SGXMKIF
  offsets, so the firmware interface can change without touching this header. (design,
  D8)

What this replaces on the client side (`docs/DRIVER-UPSTREAM.md` section 4):

| today, in the winsys | with this uAPI |
|---|---|
| services heaps and allocation attributes | GEM objects, `VM_MAP` |
| CCB ring, `SGXMKIF_CMDTA` | `SUBMIT_JOBS` with `CMD_TA_KICK` and `CMD_RENDER` streams |
| render details, RT data, region headers, tail pointers, RT numbers | `CREATE_RT_DATASET`, kernel-built |
| parameter buffer, descriptor, block list, free list, thresholds | `CREATE_CONTEXT.param_buffer_pages`, kernel-built |
| HW render context | `CREATE_CONTEXT`, kernel-built |
| allocations that reproduce the DDK's kernel-data layout | gone: kernel-owned memory is never reused while the microkernel can still reach it (section 7.5) |
| sync objects polled on the CPU, HW device sync lists, pending-value counts | syncobjs and out-fences |

What Mesa keeps writing, because the hardware reads it as data: every word in
`docs/DRIVER-UPSTREAM.md` section 3.

## 2. Ioctls

All are `DRM_RENDER_ALLOW`. The driver has no global or modeset ioctls. Numbers are
offsets from `DRM_COMMAND_BASE`. Sizes are in section 6. The header carries full
kerneldoc; `Documentation/gpu/powervr_sgx/uapi.rst` pulls it in the way
`Documentation/gpu/imagination/uapi.rst` does.

| # | ioctl | dir | purpose | rests on |
|---|---|---|---|---|
| 0x00 | `DEV_QUERY` | RW | core rev, limits, quirks, heaps | design, pvr shape |
| 0x01 | `CREATE_BO` | RW | shmem GEM object | design, pvr shape |
| 0x02 | `GET_BO_MMAP_OFFSET` | RW | fake mmap offset | design, pvr shape |
| 0x03 | `CREATE_VM_CONTEXT` | RW | one page directory | capture: per-context PD |
| 0x04 | `DESTROY_VM_CONTEXT` | W | | |
| 0x05 | `VM_MAP` | W | map at a client-chosen address in a heap | design |
| 0x06 | `VM_UNMAP` | W | remove a mapping | design |
| 0x07 | `CREATE_CONTEXT` | RW | render or transfer queue, with its firmware state and PB | capture: contexts and PB per STATE.md 6, TEX.md 10 |
| 0x08 | `DESTROY_CONTEXT` | W | | |
| 0x09 | `GET_CONTEXT_STATE` | RW | guilty, innocent or OK after a reset | design; `drm-uapi.rst` "Device reset" |
| 0x0a | `CREATE_RT_DATASET` | RW | everything tiling a target of a given size needs | capture: STATE.md 6 |
| 0x0b | `DESTROY_RT_DATASET` | W | | |
| 0x0c | `SUBMIT_JOBS` | RW | render scenes and transfers, syncobj in/out, BO refs | capture: TEX.md 3, STATE.md 1 |
| 0x0d | `UPLOAD_CODE` | RW | copy USSE code into a code object and validate it (D9, D12) | hardware: P1 (7.2); design |

### DEV_QUERY

Same mechanism as pvr: `{type, size, pointer}`, with the kernel writing the smaller of
the two sizes. A NULL pointer returns the size.

| query | returns | evidence |
|---|---|---|
| `GPU_INFO_GET` | `core_id` and `core_revision`, raw from `EUR_CR_CORE_ID` and `EUR_CR_CORE_REVISION`; `fw_interface_version` | capture: rev `0x1000e` on the CE5300 (SRVINIT.md). gap: Cedarview's value (G25) |
| `LIMITS_GET` | RT size and samples limits, PB min/default/max in pages, largest TA kick count | capture: default PB of 0x400 pages (STATE.md 6). gap: RT limits (G14) |
| `QUIRKS_GET` | quirk numbers, mandatory ones first, as pvr | design. The list is empty on rev 1014 until an erratum that reaches userspace is documented (G25) |
| `HEAP_INFO_GET` | array of `drm_powervr_sgx_heap`, see section 5 | capture, section 5 |

### CREATE_BO, GET_BO_MMAP_OFFSET

GEM on `drm_gem_shmem`. Objects are zeroed at allocation, and PRIME goes through the DRM
core. `DRM_POWERVR_SGX_BO_ALLOW_CPU_ACCESS` is required before `GET_BO_MMAP_OFFSET` succeeds,
and before an mmap of the object's exported dma-buf does.
The CPU mapping's cache mode is a kernel decision and not part of the uAPI. It defaults
to write-combined until coherency is established (G19).

An imported dma-buf is a data object. It maps in every heap except the USSE ones.
`GET_BO_MMAP_OFFSET` refuses it, because its CPU mapping is the dma-buf's. The GPU
addresses 32 bits of physical memory, so without an IOMMU `VM_MAP` refuses an import
with a page above 4 GiB (`-EFAULT`): the DMA layer would have bounced that page, and the
GPU would write the bounce copy. The GPU doesn't snoop the CPU caches, not even through
cache-consistent PTEs (EA1, 2026-11-09), and an import's pages may be cached on the CPU
side. **So a job must list every import it reads or writes in its BO list** (`READ` or
`WRITE`), on explicit sync too: the kernel flushes each listed import before the job and
again once it is done, and only those. An unlisted import gets no coherence: the GPU may
read stale data, and dirty CPU lines may later overwrite its output. Flushing every import
mapped in the VM instead (the first version) cost Minecraft about 12 ms a frame, most of
it for X jobs that never touched the scanout (2026-11-09); one 720x480 buffer is about
1.4 MB, about 1.3 ms to flush on the CE5300. Our own objects, write-combined on the CPU,
need no flush; they need listing only when shared, for implicit sync. An object in a job's BO list gets the job's fence
in its reservation object (written: `WRITE`, else `READ`), and the job waits for the
fences already there. That shared reservation is what makes a KMS commit of an imported
buffer wait for the render (`powervr_sgx_prime`, the implicit frames).

`DRM_POWERVR_SGX_BO_CODE` makes a code object (D12): it holds USSE programs, it is the
only kind of object the USSE heaps accept, and it can go nowhere else. It has no CPU
mapping (so `ALLOW_CPU_ACCESS` with it is `-EINVAL`), PRIME export refuses it, and its
contents arrive only through `UPLOAD_CODE`.

### UPLOAD_CODE

`{handle, flags, offset, size, pointer, bad_offset}`. The kernel copies `size` bytes from
`pointer` into the code object at `offset`, both multiples of 8, and scans every 8-byte
word of them (an instruction, low word first) before anything is copied. A word fails if:

| rule | why |
|---|---|
| it matches none of the forms in the kernel's table | a later ISA finding can't open a hole: an unknown encoding is refused, not trusted |
| it is `ldr` or `str` | the register file (P1, D9) |
| it is `setl` or `savl` | they set the link address from a register, so a following `lapc` could jump anywhere. The GLES compiler never emits them; only the microkernel does |
| it is `br` and its target (this instruction + the signed 18-bit field) is outside the object | the target is exact for a relative branch, so it is checked. `ba`, `phas.i` and `lapc` targets are absolute from the code base the task runs from, which depends on where the object is mapped: they can only reach the USSE heaps or the unassigned gaps after them (section 5), and a jump into a gap faults |

On a failure nothing is copied, the ioctl returns `-EINVAL`, and `bad_offset` is the
object offset of the first bad instruction, for the compiler's debug output.

The table is generated from the clean-room isaspec description,
`mesa/src/sgx/isa/sgx.xml`, by `drm/tools/gen_usse_table.py`: one (mask, match)
pair per form, the bits the form's patterns fix. The generator refuses two forms that
could match the same word.

The GPU must not run code that changes after the scan. Code objects are mapped with the
PTE `READONLY` bit, which stops a client task's `st` (E1, DRM-DRIVER.md section 6).
While a VM maps the object, an upload may only write past the end of every earlier
upload (`-EBUSY` otherwise): new code lands only in bytes the GPU sees as zeros, which
is a harmless instruction. Once no VM maps it, the object can be uploaded anywhere
again.

PDS programs are not scanned. A PDS program reaches USSE code only through DOUTU, which
picks a code base: 0-13 cover exactly the two USSE heaps, and 14 and 15 point at nothing
in a client VM (with the open microkernel; under the DDK microkernel base 15 is the
kernel code heap, P3). Per-draw PDS programs stay in ordinary objects.

### VM contexts, VM_MAP, VM_UNMAP

Same shape as pvr: the client manages the address space and picks each mapping's
address, and the kernel tracks mappings with `drm_gpuvm`.

Rules the kernel enforces:

- Range page-aligned, not empty, entirely inside one heap from `HEAP_INFO_GET`, and
  inside the object. Otherwise `-EINVAL`.
- No overlap with an existing mapping, or `-EEXIST`.
- `device_addr` below 4 GiB. The GPU's address words are 32 bits (capture: every address
  in STATE.md, PDS.md and TEX.md).
- `VM_UNMAP` takes effect before it returns for new jobs. The object's pages stay
  allocated until the translation caches are invalidated (UKERNEL.md section 6,
  "BIF cache invalidate") and every job submitted before the unmap has finished. This
  is what stops a stale translation reaching a page the system has given to someone
  else.

`VM_MAP.flags` is reserved. The USSE heaps take only code objects, a code object maps
only into a USSE heap, and code objects are mapped read-only on the GPU (D12). A
read-only flag for other objects can come later.

### CREATE_CONTEXT

`type` is render or transfer. For a render context the kernel allocates and builds:

| structure | size | formula |
|---|---|---|
| HW render context | 68 bytes | STATE.md 6; PD address at byte 8, CCB, CCB control, PB descriptor, scratch block |
| command ring and control | 0x10420 + 8 bytes | STATE.md 6 |
| PB | `(param_buffer_pages or default) + 0x22` pages | STATE.md 6 |
| free list | 0x40000 bytes | STATE.md 6 |
| PB descriptor and block list | 0x3c + 0x10 bytes | STATE.md 6 |
| scratch block | 0x40 bytes in the 3D params range | STATE.md 6 |

For a transfer context: the 0xb4-byte HW transfer context and a 0x104c8-byte ring with
its 8-byte control (TEX.md section 10).

`priority` maps onto `drm_sched` entity priority. The SGX has no documented preemption,
so priority orders jobs and does not interrupt them (gap, G16).

`param_buffer_pages` is the requested size, the number STATE.md 6 calls "requested": the
kernel adds the 0x22 pages and derives the thresholds. Because the free list's page
indexes are 16 bits with `0xffff` as the end marker (capture), a PB and everything else
in the 3D params range together stay under 0x10000 pages. A second render context in the
same VM takes its PB from the same 256 MiB and gets `-ENOSPC` when it no longer fits.
Whether two PBs in one VM work at all is a gap (G15). The first driver may have to allow
only one render context per VM until that is tested.

`DESTROY_CONTEXT` waits for the context's jobs, then runs the firmware cleanup
(UKERNEL.md section 3, type 5), then frees. If the cleanup never completes, the kernel
keeps the memory and treats the GPU as needing a reset (section 7.5).

### GET_CONTEXT_STATE

See section 8.

### CREATE_RT_DATASET

`{context_handle, width, height, samples, flags}` in, a handle out. Bound to one render
context, because each RT data entry names its HW render context (STATE.md 6, word 0).
That context's VM gets the dataset's GPU-walked memory.

The kernel computes (all capture, STATE.md 6):

- `xt = (w+31)>>5` and `yt = (h+15)>>4`.
- Macrotile mode from `w·h` against `0x4b000`, then `mx`, `my`, `TE_MTILE1`,
  `TE_MTILE2`, `TE_SCREEN` and tiles per macrotile.
- Region headers: two arrays of `(macrotiles per side · mx) · (macrotiles per side · my)
  · 12` bytes. That gives 0x3000 at 720×480 in mode 1 and 0x300 at 128×64 in mode 0.
- Tail pointers: `(padded tiles per side)² · 4` bytes, rounded up to a power of two:
  0x1000 at 720×480. STATE.md 6 writes `max(mx,my)`, which gives 0x100 there, so its
  `mx` means padded tiles in that formula (one data point, G14).
- RT data with two entries, three render-details sets, the TA-data blocks, and the
  0x200-byte background object block with its vertices from `w` and `h`.

`samples` must be 1 in this version. TE AA has multisample bits, but no capture shows a
multisampled frame (G14). `flags` is reserved. `TE_AA`, `RT +0x10`, `+0x14`, `+0x18` and
`+0x68` are kernel constants in this version (G12).

### SUBMIT_JOBS

`{jobs (obj_array), failed_index}`. All-or-nothing: on error no job was queued, and
`failed_index` names the job that caused it.

`drm_powervr_sgx_job`:

| field | meaning |
|---|---|
| `type` | `RENDER` or `TRANSFER` |
| `context_handle` | context of the matching type |
| `flags` | per type: transfer has `DST_TWIDDLED`, render has none yet |
| `cmd_stream`, `cmd_stream_len` | a `CMD_RENDER` or `CMD_TRANSFER` stream (section 3) |
| `sync_ops` | array of `{handle, flags, value}`, wait or signal, binary or timeline, as pvr |
| `bo_refs` | array of `{handle, flags}`, read or write. Only shared objects need one (D6) |
| `ta_kicks` | render only: array of `drm_powervr_sgx_ta_kick` `{cmd_stream, cmd_stream_len, flags}`, each a `CMD_TA_KICK` stream; at least one |
| `rt_dataset_handle` | render only |

**A render job is a whole scene** (D3). The first element of `ta_kicks` is the kick with
`bFirstKickOrResume` and the last is the kick with `bLastInScene`, which starts the 3D.
The kernel writes one TA command per element, in one go. A single-draw frame is two
elements: the VDM stream, then the terminate words (capture, STATE.md "A frame, end to
end"). Whether one element carrying both works is untested (G9).

Fences and errors:

- One out-fence per job. It signals from the completion interrupt when the 3D has
  finished, or when the transfer has finished.
- Reservation objects: the out-fence goes on the VM's reservation object as bookkeeping,
  and on each `bo_refs` entry as read or write. In-fences of `bo_refs` become scheduler
  dependencies.
- `-ECANCELED`: the context is not OK (section 8). `-EIO`: the device is lost. `-ENOENT`:
  a handle is wrong. `-EINVAL`: a stream is malformed, or a stream or job field failed
  validation (section 3). `-EFAULT`: a pointer is bad.

Kernel side, for orientation: one `drm_sched` scheduler for the GPU, one entity per
context, and a hardware limit of one job at a time in the first version, since the open
microkernel's render-in-flight list is one slot (UKERNEL.md section 5). The job timeout
feeds section 8. Scene-level jobs mean the scheduler's unit equals the microkernel's
unit, so no half-submitted scene can sit in the firmware waiting for userspace.

## 3. Client streams, field by field

### 3.1 Stream format

The client's part of each command is a `pvr_stream`-style scalar stream (D2), the way
upstream's Rogue driver passes its geometry, fragment and transfer commands
(`drivers/gpu/drm/imagination/pvr_stream.c`). There are three streams:

| stream | where | per |
|---|---|---|
| `CMD_RENDER` | `drm_powervr_sgx_job.cmd_stream` of a render job | scene: the 3D values |
| `CMD_TA_KICK` | `drm_powervr_sgx_ta_kick.cmd_stream`, one per element of `ta_kicks` | kick |
| `CMD_TRANSFER` | `drm_powervr_sgx_job.cmd_stream` of a transfer job | transfer |

A stream is little-endian and laid out as follows (design; framing as `pvr_stream`):

1. A header of two `__u32`: the main stream's length in bytes, header included, then a
   word that must be 0.
2. The main stream: the fields in definition order, each naturally aligned from the start
   of the stream (a 64-bit field on 8 bytes).
3. Optionally, extension streams. Each group is announced by a `__u32` header: bits 29-31
   are the header type, bit 28 says another header follows, and bits 0-27 hold one bit per
   extension stream. The streams whose bits are set follow in bit order.

The kernel copies the stream (at most 4 KiB) into kernel memory once, then decodes it.
Where this is stricter than `pvr_stream.c`, the reason is that our fields get checked and
pvr's do not:

| rule | `pvr_stream.c` | here |
|---|---|---|
| main stream length | at least the header and at most the stream | **exactly** the length the kernel knows |
| bytes skipped to align a field | ignored | must be 0 |
| bytes after the last extension stream | ignored | `-EINVAL` |
| the same header type twice | accepted | `-EINVAL` |
| field values | copied into the firmware command | checked one by one (section 3.2), then across fields (sections 3.3 to 3.5) |

**Growth.** A main stream never changes once released, so an old client's stream always
decodes. A new field goes in an extension stream behind a new header bit. The driver's minor
version (`drmGetVersion`) tells a client which bits a kernel accepts, as for any DRM uAPI
addition; a bit tied to a hardware quirk is also listed by `QUIRKS_GET`. This is how D5 works
with streams: a command
word that gets a name and a test becomes an extension-stream field, and until then the
kernel writes the captured constant. pvr's per-field `feature` flags exist for Rogue's BVNC
variants. Nothing here needs them yet. If Cedarview diverges (G25), an extension stream
behind a quirk carries the difference, and a must-have quirk makes it mandatory, as in pvr.

**Fields carry values, not firmware words.** pvr's stream fields are register images that
Mesa packs. Here a field is a value in the client's units, such as an address or a row
count, and the kernel packs the firmware word. Each field can then be checked on its own,
and the client never does arithmetic on kernel-chosen bases like `EUR_CR_PDS_EXEC_BASE`.
Words that hold real register bit fields (`zls_control`, `flags_3d`, `ta_flags2`) are
passed as register words with an allowed-bit mask.

**Check order** for each stream of a job: copy, decode with the per-field checks, check the
addresses against the job's VM (`mapped`), then run the command's cross-field rules. Any
failure is `-EINVAL` and `failed_index` names the job.

### 3.2 Field layout and kernel checks

Generated from [`uapi/powervr_sgx_kmd_stream.xml`](uapi/powervr_sgx_kmd_stream.xml) by
`uapi/gen_stream.py`; `uapi/check.sh` fails if this table is stale. Offsets are from the
start of the stream.

- **address** fields are device-virtual addresses in the job's VM. "below 2^32" and the
  alignment and heap checks are made at decode. "mapped for" is checked against the VM's
  mappings at submit: `rt_depth` is `pad_w·pad_h·4` and `rt_stencil` is `pad_w·pad_h` of
  the job's RT dataset (section 3.4).
- "reaches a register only" means the value goes into a register or a firmware word that
  the hardware reads as a number, with no address in it. Nothing outside the VM depends on
  it.
- The `tiles_x` and `tiles_y` limits are the last tile column and row of the largest render
  target (`rt_max_width/32 - 1`, `rt_max_height/16 - 1`).

<!-- stream-table begin: generated by docs/uapi/gen_stream.py -->
| stream | offset | field | bits | type | kernel check |
|---|---|---|---|---|---|
| `CMD_RENDER` | 0x00 | header: `length` = 80, then a zero word | 64 | | exact |
| | 0x08 | `depth_addr` | 64 | address | 0 = none; below 2^32; mapped for `rt_depth` bytes |
| | 0x10 | `stencil_addr` | 64 | address | 0 = none; below 2^32; mapped for `rt_stencil` bytes |
| | 0x18 | `pixel_event_pds_addr` | 64 | address | not 0; below 2^32; in `PDS_PIXEL`; 16-byte aligned; mapped for 16·`pixel_event_pds_rows` bytes |
| | 0x20 | `bg_pds_secondary_addr` | 64 | address | not 0; below 2^32; in `PDS_PIXEL`; 16-byte aligned |
| | 0x28 | `bg_pds_primary_addr` | 64 | address | not 0; below 2^32; in `PDS_PIXEL`; 16-byte aligned |
| | 0x30 | `pixel_event_pds_rows` | 32 | uint | 1..63 |
| | 0x34 | `bg_pds_secondary_rows` | 32 | uint | 1..63 |
| | 0x38 | `bg_pds_primary_rows` | 32 | uint | 1..63 |
| | 0x3c | `zls_control` | 32 | uint | bits in `0x001ff007` only |
| | 0x40 | `flags_3d` | 32 | uint | bits in `0x00000180` only |
| | 0x44 | `bg_depth` | 32 | float | none: reaches a register only |
| | 0x48 | `depth_bias_units` | 32 | int | none: reaches a register only |
| | 0x4c | `depth_bias_factor` | 32 | float | none: reaches a register only |
| `CMD_TA_KICK` | 0x00 | header: `length` = 20, then a zero word | 64 | | exact |
| | 0x08 | `vdm_base` | 64 | address | not 0; below 2^32; 4-byte aligned; mapped for 0x4 bytes |
| | 0x10 | `ta_flags2` | 32 | uint | bits in `0x02c80000` only |
| `CMD_TRANSFER` | 0x00 | header: `length` = 48, then a zero word | 64 | | exact |
| | 0x08 | `bg_block_addr` | 64 | address | not 0; below 2^32; 16-byte aligned; mapped for 0x200 bytes |
| | 0x10 | `pixel_event_pds_addr` | 64 | address | not 0; below 2^32; in `PDS_PIXEL`; 16-byte aligned; mapped for 16·`pixel_event_pds_rows` bytes |
| | 0x18 | `pixel_event_pds_rows` | 32 | uint | 1..63 |
| | 0x1c | `first_tile_x` | 32 | uint | 0..`tiles_x` limit |
| | 0x20 | `first_tile_y` | 32 | uint | 0..`tiles_y` limit |
| | 0x24 | `last_tile_x` | 32 | uint | 0..`tiles_x` limit |
| | 0x28 | `last_tile_y` | 32 | uint | 0..`tiles_y` limit |
| | 0x2c | `bg_depth` | 32 | float | none: reaches a register only |
<!-- stream-table end -->

Where a field says "mapped", that is a programming-error check made at submit time. A
client can unmap afterwards, so this check is not a security boundary: section 7.1 says
what is.

### 3.3 `CMD_TA_KICK`

| field | goes to | evidence |
|---|---|---|
| `vdm_base` | cmd+0x250, then register 0x238 | hardware (p7): moving it by 0x20 skips the clear |
| `ta_flags2` | cmd+0x24c, then register 0x218 | capture for the values `0` and `0x2400000`; gap for their meaning (G12) |

The register also takes "a low-bits field" and "DPM bits" from the microkernel
(UKERNEL.md section 7). Both are outside the client's reach. Which kick is first and which
is last comes from the kick's position in `ta_kicks` (D3), not from the stream.

### 3.4 `CMD_RENDER`

| field | goes to | evidence |
|---|---|---|
| `depth_addr` | rd+0x30 and +0x34, as offset from the ZLS base | capture: words; hardware: store at this address (TEX.md 7) |
| `stencil_addr` | rd+0x38 and +0x3c, b31 + offset | capture: words; hardware: a bad stencil address locks up the 3D (TEX.md 7) |
| `pixel_event_pds_addr` | rd+0x5c, then register 0xa5c | capture |
| `pixel_event_pds_rows` | rd+0x60, then register 0xa60 | capture (5 rows) |
| `bg_pds_secondary_addr`, `bg_pds_secondary_rows` | cmd+0x08 | capture |
| `bg_pds_primary_addr`, `bg_pds_primary_rows` | cmd+0x10 | capture |
| `zls_control` | rd+0x2c, then register 0x480; bits 3-11 are the kernel's `(xt-1)<<3` | hardware for store, b19 and b0 (TEX.md 7); gap for load and bit names (G13) |
| `flags_3d` | cmd+0x1c; bit 10 is the kernel's, on the last kick | capture for the value `0x580`; gap for the names (G12) |
| `bg_depth` | rd+0x40, then register 0x4b8 | hardware (p3b) |
| `depth_bias_units` | rd+0x14 | capture (`polyoff`) |
| `depth_bias_factor` | rd+0x18 | capture (`polyoff`) |

Cross-field rules, in the kernel's render check:

| rule | evidence |
|---|---|
| `depth_addr` is not 0 when `zls_control` is not 0 | hardware: ZLS stores to it (TEX.md 7) |
| `stencil_addr` is not 0 when `zls_control` has bit 0 or 1, or `flags_3d` has bit 8 | hardware: a bad stencil address locks up the 3D (TEX.md 7) |
| depth and stencil each lie less than 2^30 above the ZLS request base, `min(depth, stencil) & 0xfff00000` | capture for the base (STATE.md 6); the 2^30 limit is a guess (G13) |

`pad_w` and `pad_h` are the target's size rounded up to whole tiles, 32 and 16 pixels.
That fits the one window capture: 736×480 at 4 bytes (TEX.md 7, `00081000` to `001da000`).
Other sizes are a gap (G13). The draft's limit on the ZLS offsets is a guess (G13): TEX.md 7
attributes its lockups to the placeholder `c1b00000` most likely being out of range, but no
test has found the actual range.

The cmd+0x08 and +0x10 words are packed by the kernel as
`rows<<26 | (addr - PDS_PIXEL base)>>4`, and cmd+0x0c takes the constant `0x00030001`
(capture; meaning gap, G12). Which unit consumes these three words is a gap (G11).

**The stream carries the 3D values once.** STATE.md's deferred render-details updates
exist because later kicks can only send what changed. Here the kernel writes the full
set when it builds the job and uses deferred updates only where its firmware path needs
them.

### 3.5 `CMD_TRANSFER`

| field | goes to | evidence |
|---|---|---|
| `bg_block_addr` | word 0 `1<<25 \| (block - base)>>4`; word 6 base `= block & 0xfff00000` | capture: words; hardware: blits from these words (TEX.md 10) |
| `pixel_event_pds_addr` | word 9, then register 0xa5c | capture |
| `pixel_event_pds_rows` | word 10 | capture (5) |
| `first_tile_x`, `first_tile_y` | word 12 `x<<16 \| y` | capture: word; hardware: sub-rectangles (TEX.md 10) |
| `last_tile_x`, `last_tile_y` | word 13 `x<<16 \| y` | capture: word; hardware: sub-rectangles |
| `bg_depth` | word 2, then register 0x4b8 | capture (1.0) |

Cross-field rule: `first_tile_x <= last_tile_x` and `first_tile_y <= last_tile_y`.

`DRM_POWERVR_SGX_SUBMIT_JOB_TRANSFER_DST_TWIDDLED` in `drm_powervr_sgx_job.flags` sets
cmd+0xa8 = 8 (hardware, TEX.md 10). Like pvr's per-command flags, it is a job flag, not a
stream field.

Transfer words the kernel fills, with no client field (capture, TEX.md 3 and 10):

| word | value | note |
|---|---|---|
| 1 | `0x200` | `0x300` with a depth/stencil surface, not seen (G20) |
| 3 | 3 | meaning gap |
| 4 | 0 | depth/stencil block, not seen (G20) |
| 5 | `0x100` | meaning gap |
| 7, 8 | `0x80000000`, `0x88` | as render details +0x10, +0x50 |
| 11 | 1 | TEX.md: code size; UKERNEL.md: a flag (G20) |
| 14 | `0x80` | meaning gap |
| 15 | PDS pixel heap base | **Patched, never client-supplied:** this is `EUR_CR_PDS_EXEC_BASE` |
| 16 | 0 | 0 in every capture |
| +0xa4 | `0x1c0` | command size |
| +0xac.. | `SGXMKIF_TRANSFERCMD_SHARED` | kernel: sync |
| +0x194 | `0x07c00000` | meaning gap |

Everything else about a blit is client memory, read by the hardware under its page
directory: the background block with its PDS triple, the pixel program with the source's
texture words, and the end-of-tile program with the destination's PBE words (TEX.md 3).

### 3.6 One definition, both sides

| file | what | who uses it |
|---|---|---|
| `uapi/powervr_sgx_kmd_stream.xml` | the streams, in the format of Mesa's `src/imagination/csbgen` (as `rogue/kmd_stream.xml`), plus validation attributes | the source of everything below |
| Mesa's `gen_pack_header.py` | reads the XML; emits the defines and the `HDR` and `EXTHDR_*` packers. It parses `<stream>` and emits nothing for it, as for Rogue | Mesa |
| `uapi/gen_stream.py user` | a writer per main stream (`SGX_KMD_STREAM_CMD_*_pack()`) with each field's offset | Mesa (G26) |
| `uapi/gen_stream.py kernel-h`, `kernel-c` | the decoded structs and the tables: order, size, check, heap, extent | the kernel; generated copies in `uapi/stream/` |
| `uapi/stream/pvrsgx_stream.c` | the decoder: framing, per-field checks, the mapped check | reference for the kernel driver |
| `uapi/stream/pvrsgx_cmd_checks.c` | the cross-field rules of sections 3.4 and 3.5 | reference for the kernel driver (G27) |
| `uapi/stream/test/` | host test: every stream written by the generated writer decodes to the same values, and each rule rejects the stream that breaks it; a fixture exercises extension streams, quirks and must-have bits | `check.sh` |

csbgen ignores attributes it doesn't know, which is how `rogue/kmd_stream.xml` already
carries the kernel-only `feature`. The validation attributes ride the same way: `check`
(`none`, `range`, `mask`, `addr`), `min`, `max`, `max_limit`, `mask`, `align`, `heap`,
`optional`, `extent`, `extent_field`, and `quirk` on an extension stream. The XML's header
comment defines them.

## 4. How every word of the firmware commands is produced

Column **how**: `U` from the stream, `K-obj` a kernel object's address, `K-calc` computed
by the kernel, `K-const` a captured constant.

### 4.1 TA command (STATE.md section 1)

| off | field | how | source or value | evidence |
|---|---|---|---|---|
| 0x00 | command size | K-calc | `0x2b0 + 8·n`, rounded up to 64 | capture |
| 0x04 | TA flags | K-calc | by kick position: `0x71` first, `0x62` last | capture; gap for the bits and for middle kicks (G12) |
| 0x08, 0x10 | background PDS secondary, primary | U | section 3.4 | capture |
| 0x0c | background PDS middle word | K-const | `0x00030001` | capture; gap for meaning |
| 0x14, 0x18 | | K-const | `0x50000000` | capture |
| 0x1c | 3D flags | U + K-calc | `flags_3d`; b10 set on the last kick | capture |
| 0x20 | | K-const | 0 | gap |
| 0x24 | PB descriptor | K-obj | | capture |
| 0x28-0x37 | | K-const | 0; no field in STATE.md | gap (G12) |
| 0x38 | render details | K-obj | | capture |
| 0x3c | render-target dependency block | K-obj | "sync addresses" | gap (G15) |
| 0x40 | RT data | K-obj | | capture |
| 0x44 | RT data entry | K-obj | `+0x18 + idx·0x38` | capture |
| 0x48, 0x4c, 0x50 | | K-const | 0 | gap |
| 0x54 | deferred update count | K-calc | | capture |
| 0x58-0x237 | `CMDTA_SHARED` | K-calc | kernel-filled; sync and status updates, and the ready flag that marks the last kick | capture |
| 0x238 | TE AA | K-calc | `0x20000000`, no multisample bits | capture |
| 0x23c, 0x240 | TE MTILE1, MTILE2 | K-calc | RT dataset | capture |
| 0x244 | TE screen | K-calc | RT dataset | capture |
| 0x248 | tiles per macrotile | K-calc | RT dataset | capture |
| 0x24c | TA flags 2 | U | `ta_flags2` | capture |
| 0x250 | **VDM control stream** | U | `vdm_base` | hardware |
| 0x254 | | K-const | `0xa2` | gap |
| 0x258, 0x25c | region headers, tail pointers | K-obj | | capture |
| 0x260, 0x264 | 1e-6 floats | K-const | `358637bd` | capture |
| 0x268 | screen size | K-calc | `(h-1)<<16 \| (w-1)` | capture |
| 0x26c, 0x270 | | K-const | 0 | gap |
| 0x274 | 3D params base | K-calc | PB base `& 0xfff00000` | capture |
| 0x278-0x298 | | K-const | 0 | gap |
| 0x29c | RT data entries - 1 | K-calc | | capture |
| 0x2a0, 0x2a4 | RT values | K-calc | 0, `0x88` | capture |
| 0x2a8.. | deferred updates | K-calc | addresses inside kernel-owned render details only | capture |

### 4.2 Render details (STATE.md section 6)

| off | field | how | source or value | evidence |
|---|---|---|---|---|
| 0x00 | 3D request base | K-calc | PB base `& 0xfff00000` | capture |
| 0x04 | ZLS request base | K-calc | from `depth_addr`, `stencil_addr` | capture |
| 0x08 | | K-const | `0x80` | gap |
| 0x0c | | K-const | `0x000f0000` | gap |
| 0x10 | | K-const | `0x80000000` | capture |
| 0x14, 0x18 | depth bias | U | | capture |
| 0x1c, 0x24 | | K-const | 0 | gap |
| 0x20, 0x28 | region headers, this and next | K-obj | | capture |
| 0x2c | ZLS control | U + K-calc | `zls_control`, `(xt-1)<<3` | hardware (store) |
| 0x30, 0x34 | depth offset | K-calc | `depth_addr` - base | capture |
| 0x38, 0x3c | stencil offset | K-calc | b31 + `stencil_addr` - base | capture |
| 0x40 | ISP background depth | U | `bg_depth` | hardware |
| 0x44 | | K-const | `0x300` | gap |
| 0x48 | background object | K-obj | `0x2000000 \| (block - PB base)>>4` | capture |
| 0x4c, 0x50 | | K-calc | 0, `0x88` | capture |
| 0x54 | TA dependency | K-const | 0 | gap |
| 0x58 | | K-const | `0x30` | capture |
| 0x5c, 0x60 | pixel event program, rows | U | | capture |
| 0x64 | | K-const | 1 | gap |
| 0x68 | | K-const | 1 | capture |
| 0x6c-0xc8 | | K-const | 0 | gap |
| 0xcc | | K-const | `0x100` | capture for the value |
| 0x13c | | K-const | `0x07c00000` | capture |
| rest, to 0x1ec | microkernel state, syncs | K-calc | destination syncs in words 0x34 and 0x90 (UKERNEL.md 5) | capture |

### 4.3 Structures with no client input

Built whole by the kernel from STATE.md section 6 and SRVINIT.md: HW render context, HW
transfer context, PB descriptor and block list, free list, RT data and its entries,
background object block (render path), region headers and tail pointers (allocated
zeroed), TA3D control block, host control block, kernel CCB and its control, the event
kicker, HWPerf buffer, signature buffers, misc info, and the microkernel with its entry
programs.

## 5. Heap layout

All device addresses fit in 32 bits, and the layout is the same in every VM. It is
fixed device-wide because the code-base registers are programmed once at init
(capture, SRVINIT.md "Init scripts"; UKERNEL.md's wording on per-context code bases is
G7). Page size is 4 KiB (capture: the free list has one
word per 0x1000 of PB).

| range | base | upper bound | who maps | why | evidence |
|---|---|---|---|---|---|
| 3D params | `0x10000000` | `0x20000000` | **kernel** | PB, free list, scratch and background blocks | capture; 0x10000 pages matches the 0x40000-byte free list |
| TA data | `0x20000000` | `0x2d000000` | **kernel** | region headers, tail pointers | capture |
| sync | `0x2d000000` | `0x2e000000` | **kernel** | sync and status words | capture |
| PDS pixel | `0x2e000000` | `0x30000000` | client | `EUR_CR_PDS_EXEC_BASE`; state words are offsets from it | capture |
| kernel code | `0x30000000` | `0x30400000` | **kernel** | microkernel, entry programs, HW contexts | capture |
| PDS vertex | `0x30400000` | `0x32000000` | client | VDM stream programs | capture |
| kernel data | `0x32000000` | `0x37000000` | **kernel** | CCBs, control blocks, render details, RT data | capture |
| USSE pixel | `0x37000000` | `+0x500000` | client | code bases 0-9 | capture and hardware |
| USSE vertex | `0x3c000000` | `+0x200000` | client | code bases 10-13 | capture and hardware |
| general | `0x3e000000` | `0xf0000000` | client | VDM streams, vertices, surfaces | capture |
| texture | `0xf0000000` | `2^32` | client | texels | capture |

The bases are measured (NOTES.md). **Only the USSE sizes are known limits.** Each other
upper bound is just the next base: the DDK's real heap sizes are a gap (G8), and the
kernel can return smaller sizes. Whether the ranges above are a hardware requirement or
only the DDK's convention is also a gap, apart from the constraints below.

Constraints from the hardware, which any layout must meet:

| constraint | applies to | evidence |
|---|---|---|
| USSE code is reached through `EUR_CR_USE_CODE_BASE_n`, one 512 KiB window per register. DOUTU w0 holds `n` and offset/8. Bases 0-9 are pixel, 10-13 vertex, 15 the microkernel, 14 unset. So at most 5 MiB of pixel and 2 MiB of vertex code | USSE heaps | hardware: every program run; capture: SRVINIT.md, PDS.md |
| The register holds `addr>>7`, so a code base is 128-byte aligned; instructions are 8 bytes | USSE heaps | capture |
| `HEAP_INFO` returns `usse_base_index` and `usse_window_log2` so that Mesa computes DOUTU words from the query, not from constants | USSE heaps | design |
| TA state and transfer words reference PDS programs as `(addr - base)>>4` with `rows<<26`. That is 16-byte aligned, within 256 MiB, with a 6-bit row count | PDS pixel | capture |
| VDM words reference PDS programs as `type<<28 \| addr>>4`: an absolute address, 16-byte aligned. Whether programs outside this heap run is a gap (G8) | PDS vertex | capture |
| Request bases are `addr & 0xfff00000`: the PB, ZLS and transfer background block are each addressed relative to a 1 MiB-aligned base | 3D params; ZLS; transfer | capture |
| Free-list entries are `prev<<16 \| next`, with `0xffff` as the end marker and indexes counted from the heap base, so a PB lies within 0x10000 pages of it | 3D params | capture |
| Index buffer: 2 or 4-byte aligned, the low bits being cleared in the word | general | capture |
| T2 and the PBE address are full addresses. The DDK's textures are in the texture heap, but the background object samples the render target at `0x3e265000` in the general heap, so the texture heap is convention | texture, general | capture (PDS.md, DOUTT) |

Placement that every proven frame follows, though no test shows the hardware requires it
(G10): PDS programs on 32 bytes, USSE programs on 128, DMA sources on 32, and rings
starting 0x100 in, so that a PDS state word or DOUTU word is never 0.

## 6. Layouts

Every struct is identical on i386 and x86_64: all members naturally aligned, every struct
a multiple of 8 bytes, no implicit padding. Ioctl numbers are therefore equal too, and
the driver has no `compat_ioctl` handler beyond `drm_compat_ioctl`.

`uapi/check.sh` compiles `uapi/check_layout.c` with `zig cc -target x86-linux-gnu` and
`-target x86_64-linux-gnu`, with `-Wall -Wextra -Wpedantic -Werror -Wpadded`, against the
kernel's `drm/drm.h` as zig ships it. Each line of `uapi/layout.inc` becomes static
asserts with literal numbers, so that both targets passing means the layouts are equal. A
negative control (`-DCHECK_NEGATIVE`, a `__u64` after a lone `__u32`) has to fail on both
targets. It also compiles the header as C++ and checks this table against `layout.inc`.

Streams have no C layout. Their offsets are in section 3.2, generated from the XML. Each
field is naturally aligned from the stream's start, so a stream is the same bytes on both
targets by construction. `check.sh` checks the rest: the committed kernel tables match the
XML, the decoder compiles for both targets, and the stream test passes.

<!-- layout-table begin: generated by docs/uapi/gen_table.py -->
| struct | size | members: `offset name (size)` |
|---|---|---|
| `drm_powervr_sgx_obj_array` | 16 | `0x00 stride (4)`, `0x04 count (4)`, `0x08 array (8)` |
| `drm_powervr_sgx_dev_query_gpu_info` | 16 | `0x00 core_id (4)`, `0x04 core_revision (4)`, `0x08 fw_interface_version (4)`, `0x0c _padding_c (4)` |
| `drm_powervr_sgx_dev_query_limits` | 32 | `0x00 rt_max_width (4)`, `0x04 rt_max_height (4)`, `0x08 rt_max_samples (4)`, `0x0c pb_min_pages (4)`, `0x10 pb_default_pages (4)`, `0x14 pb_max_pages (4)`, `0x18 max_ta_kicks (4)`, `0x1c _padding_1c (4)` |
| `drm_powervr_sgx_dev_query_quirks` | 16 | `0x00 quirks (8)`, `0x08 count (2)`, `0x0a musthave_count (2)`, `0x0c _padding_c (4)` |
| `drm_powervr_sgx_heap` | 32 | `0x00 base (8)`, `0x08 size (8)`, `0x10 flags (4)`, `0x14 page_size_log2 (4)`, `0x18 usse_base_index (4)`, `0x1c usse_window_log2 (4)` |
| `drm_powervr_sgx_dev_query_heap_info` | 16 | `0x00 heaps (16)` |
| `drm_powervr_sgx_ioctl_dev_query_args` | 16 | `0x00 type (4)`, `0x04 size (4)`, `0x08 pointer (8)` |
| `drm_powervr_sgx_ioctl_create_bo_args` | 24 | `0x00 size (8)`, `0x08 handle (4)`, `0x0c _padding_c (4)`, `0x10 flags (8)` |
| `drm_powervr_sgx_ioctl_get_bo_mmap_offset_args` | 16 | `0x00 handle (4)`, `0x04 _padding_4 (4)`, `0x08 offset (8)` |
| `drm_powervr_sgx_ioctl_upload_code_args` | 40 | `0x00 handle (4)`, `0x04 flags (4)`, `0x08 offset (8)`, `0x10 size (8)`, `0x18 pointer (8)`, `0x20 bad_offset (8)` |
| `drm_powervr_sgx_ioctl_create_vm_context_args` | 8 | `0x00 handle (4)`, `0x04 _padding_4 (4)` |
| `drm_powervr_sgx_ioctl_destroy_vm_context_args` | 8 | `0x00 handle (4)`, `0x04 _padding_4 (4)` |
| `drm_powervr_sgx_ioctl_vm_map_args` | 40 | `0x00 vm_context_handle (4)`, `0x04 flags (4)`, `0x08 device_addr (8)`, `0x10 handle (4)`, `0x14 _padding_14 (4)`, `0x18 offset (8)`, `0x20 size (8)` |
| `drm_powervr_sgx_ioctl_vm_unmap_args` | 24 | `0x00 vm_context_handle (4)`, `0x04 _padding_4 (4)`, `0x08 device_addr (8)`, `0x10 size (8)` |
| `drm_powervr_sgx_ioctl_create_context_args` | 24 | `0x00 type (4)`, `0x04 flags (4)`, `0x08 priority (4)`, `0x0c vm_context_handle (4)`, `0x10 param_buffer_pages (4)`, `0x14 handle (4)` |
| `drm_powervr_sgx_ioctl_destroy_context_args` | 8 | `0x00 handle (4)`, `0x04 _padding_4 (4)` |
| `drm_powervr_sgx_ioctl_get_context_state_args` | 16 | `0x00 handle (4)`, `0x04 state (4)`, `0x08 reset_count (4)`, `0x0c _padding_c (4)` |
| `drm_powervr_sgx_ioctl_create_rt_dataset_args` | 24 | `0x00 context_handle (4)`, `0x04 width (4)`, `0x08 height (4)`, `0x0c samples (4)`, `0x10 flags (4)`, `0x14 handle (4)` |
| `drm_powervr_sgx_ioctl_destroy_rt_dataset_args` | 8 | `0x00 handle (4)`, `0x04 _padding_4 (4)` |
| `drm_powervr_sgx_ta_kick` | 16 | `0x00 cmd_stream (8)`, `0x08 cmd_stream_len (4)`, `0x0c flags (4)` |
| `drm_powervr_sgx_sync_op` | 16 | `0x00 handle (4)`, `0x04 flags (4)`, `0x08 value (8)` |
| `drm_powervr_sgx_bo_ref` | 8 | `0x00 handle (4)`, `0x04 flags (4)` |
| `drm_powervr_sgx_job` | 80 | `0x00 type (4)`, `0x04 context_handle (4)`, `0x08 flags (4)`, `0x0c cmd_stream_len (4)`, `0x10 cmd_stream (8)`, `0x18 sync_ops (16)`, `0x28 bo_refs (16)`, `0x38 ta_kicks (16)`, `0x48 rt_dataset_handle (4)`, `0x4c _padding_4c (4)` |
| `drm_powervr_sgx_ioctl_submit_jobs_args` | 24 | `0x00 jobs (16)`, `0x10 failed_index (4)`, `0x14 _padding_14 (4)` |
<!-- layout-table end -->

## 7. Threat model

**Attacker:** an unprivileged process that can open the render node. It controls every
ioctl argument, every byte of its VM, and so every PDS program, USSE program, VDM stream
and TA state block.

**Must not be possible:** reading or writing another client's memory or kernel memory;
reaching physical memory outside what is mapped for it; forging a fence so that another
device or client acts on unfinished work; wedging the GPU beyond what a reset recovers.

**Accepted:** a client can hang the GPU, since any shader can loop or stall (hardware:
atst8 h3, a missing vertex epilogue). This is bounded by the job timeout, costs the client
its context, and costs others a delay. Timing and counter side channels are out of scope;
HWPerf has no uAPI.

### 7.1 What enforces isolation

1. **Per-VM page directory.** TA and 3D translate through the job's PD (capture,
   UKERNEL.md sections 3 and 6). Anything the hardware reads as data can only reach that
   VM. **This is the real boundary for every address in section 7.4 class B.**
2. **Kernel-owned firmware structures.** No GEM handle, no CPU mapping, no client-writable
   VA, so the microkernel never dereferences anything a client wrote.
3. **Copy and check.** Streams are copied once into kernel memory, so there is no
   read-after-check window on firmware commands.
4. **Reserved VA.** `VM_MAP` refuses anything outside the client heaps, so a client can't
   create a mapping at a firmware address.

### 7.2 Four hardware premises this design needs

**Tested on the EA3 (core rev 1.0.14), 2026-11-02 (hardware):** P1, P2 and P3 **fail**;
P4 **holds for our microkernel by construction** (it no longer reads `sa`, f672198), and the read half held on hardware; whether a client can leave `sa` for the DDK microkernel to read is untested. See `docs/log/entries/2026-11-02-0710-isolation-p1p4.md` and
`tools/iso/`. So the items in 7.1 do not deliver isolation on their own, and D9 is in
force. Summary in the table's last column; detail in the log entry.

**P1's open half, on the EA1, 2026-11-02 (hardware):** the register-*write* test Joseph
asked for in D9. A client `str` to `BIF_DIR_LIST_BASE0` (0xc84) **does not change the
register** — host read 0x01d6b000 before and after, and the shader's own read-back
unchanged, in a frame that returned `render done` — so a client **cannot repoint translation**
to its own directory this way; the page-directory boundary survives this attack. That store is
also **not a denial of service**: an isolated re-run (Joseph's go, on a clean cold boot with
WPE off and nothing else on the GPU) rendered a sanity frame, did the store, and left the box
up with **0 SGX recoveries** over ~2.5 min idle — no reboot, no wedge. (The first run's reboot
was a confound: another session's dEQP chunk had wedged and rebooted the box just before; that
is not this store.) **Scope:** three registers were tried from a client task — `BIF_DIR_LIST_BASE0`
and the two `USE_CODE_BASE_14`/`_15` controls — and all three stores were **dropped** (no change,
no escape, no DoS). That a client store is dropped is **not** general: the microkernel is itself a
USSE program that writes registers with `str` (UKERNEL.md §8), so stores land in *some* context,
and which registers or task types drop a client store is unknown — `SOFT_RESET`, `EVENT_KICK` and
the clock/power controls are untested and could be destructive. So a client store grants no escape
and isn't a DoS *for these three*, but the validator still forbids client `str` (below). See
`docs/log/entries/2026-11-02-2021-isolation-p1-register-write.md`.

If any of these fails, the items above don't deliver isolation. Each needed a hardware
experiment before M2's exit test could mean anything; that experiment has now run.

| # | premise | why it is in doubt | experiment | **hardware result (2026-11-02)** |
|---|---|---|---|---|
| **P1** | **A client's USSE task can't write `EUR_CR_*` registers.** | UKERNEL.md section 8 has the microkernel using `ldr`/`str` on the register file. ISA.md lists `ldr`/`str` (names a guess there) as ordinary instructions with a 14-bit immediate address, and neither doc says they are restricted (G1). A task that can write the directory-list base can point itself at any memory. | From a vertex and a pixel task of an ordinary client: `ldr` of `EUR_CR_CORE_ID` and compare with the known value; `str` to a harmless scratch-like register and read it from the host. | **FAILS (reads).** A fragment task's `ldr` returned the true `CORE_ID` (0x01150000), `CORE_REVISION` (0x1000e) and a live per-pixel `TIMER`. `str` to the unused `USE_CODE_BASE_14`/`_15` had no effect. The *effective*-register write (EA1, 2026-11-02): a `str` to `BIF_DIR_LIST_BASE0` **does not change the register** (host and shader read-back unchanged, frame completed), so a client **can't repoint translation** this way, and that store is also **not a DoS** — an isolated re-run (clean cold boot, WPE off, GPU idle) left the box up with 0 SGX recoveries over ~2.5 min. (The first run's reboot was a concurrent dEQP wedge, not this store.) All three client stores tried (`BIF_DIR_LIST_BASE0`, `USE_CODE_BASE_14`/`_15`) were dropped — but not generally: the microkernel writes registers via `str` (UKERNEL.md §8), so stores land in some context, and untested registers (`SOFT_RESET`, `EVENT_KICK`, clock/power) could be destructive. Fix: D9 option 1 — validate/forbid client `ldr` (reads leak) **and** `str` (same scan; relax only once we know why client stores drop). |
| **P2** | **Kernel memory can't be reached from a client's TA or 3D task.** | UKERNEL.md section 6 points the EDM's directory base at the context PD, which suggests the DDK maps kernel heaps in every PD. SRVINIT.md's allocations carry `EDM_PROTECT`, but no doc says what enforces it or whether it is enforced. | From a client shader: load and store an address in the kernel data heap, with the page mapped `EDM_PROTECT`, and watch the memory from the host. Also read the BIF bank registers' layout (G2). | **FAILS.** Pixel *and* vertex tasks `ld` the host control block, the TA3D control block (incl. word 0x16 kernel-CCB base and **word 0x17 the PD physical address**), the kernel CCB, and the microkernel code — all in the client's translation. A `st` into the kernel region wedged the GPU (HWR). A client could also allocate in the kernel-data heap. Fix: the new kernel driver owns the PD and does not map kernel heaps/code into client PDs. **Only with the open microkernel** (EA1, 2026-11-03, log `2026-11-03-0135-drm-first-ta`): the DDK microkernel moves the EDM onto the job's directory during a TA kick (`BIF_BANK0 = 0x11`, list 1 = the context PD) and fetches its own code through it, so under the DDK microkernel every client PD must map the kernel heaps and P2 stays open. The driver does that only when the DDK microkernel is loaded (`share_kernel_heaps`), and warns. |
| **P3** | **A client's PDS program can't enter the microkernel or gain its rights.** | DOUTU w0 picks the code base, and base 15 is the microkernel. w0 b27 is set on microkernel entries, meaning unknown (PDS.md). `USE_CODE_BASE` values carry `0x2000000` and `0x6000000` flags, not decoded (G4). | A client PDS program issues DOUTU with base 15, and with b27, at a test program; watch whether it runs, under which PD, and what P1 and P2 give it there. | **FAILS.** A client DOUTU naming code base 15 was not refused; the task ran from the kernel code base and wedged the GPU (HWR, pixel 0 not the uniform). DOUTU b27 changed nothing on a base-0 task. Fix: keep base 15 / the kernel code heap out of client PDS and PDs. As P2: possible only with the open microkernel; under the DDK microkernel the kernel code heap is in every client PD, so P3 stays open. |
| **P4** | **The microkernel takes nothing from registers a client can write.** | UKERNEL.md section 1: secondary attributes "which the hardware does not clear between tasks" hold the microkernel's live state, the PD's physical address (word 0x17) among them. `sa` is what client shaders use for uniforms (G5). | Client shader reads `sa0`-`sa63` right after a microkernel task; then writes them and observes the next microkernel entry. | **Did not reproduce.** A client task's `sa11`-`sa63` held its own secondary-update results, per-task coefficient data (differs per pixel) and a prior same-client draw's clear color — not the microkernel's named pointers; `sa` is reloaded per task. But P2 hands a client the PD physical address anyway. Cross-client `sa` leak (G6) needs two clients, untested. The write-then-observe half is **untested**: the bring-up (log 2026-11-02-0630) first read as "a USSE task cannot write `sa`", but that was corrected the same night. The DDK microkernel writes `sa` in its PDS-DMA-entered context, and the stall was a write in the bare-`DOUTU` context before any DMA. Since f672198 the open microkernel keeps its pointers in a temporary (r17) and reads no `sa`, so nothing a client leaves in `sa` can steer it. **P4 holds for our microkernel**; for the DDK microkernel it rests on the per-task reload seen above. |

Related but smaller: leftovers. STATE.md (kk1-kk4) shows `k18` reading "state left behind
by earlier tasks", per pixel. Whether that crosses clients is a gap (G6). If it does it is
a low-bandwidth read of another client's colors, which a scrub task could close at a
cost.

### 7.3 Every firmware-visible structure

"Reads": who dereferences it. "EDM" is the microkernel, "HW" fixed-function units under
the job's PD.

| structure | size | owner | range | reads | if a client could write it | enforced by |
|---|---|---|---|---|---|---|
| microkernel | image | kernel | kernel code | EDM | firmware code execution | RO mapping (G3), P2, P3 |
| entry PDS programs | 0x18-0x100 | kernel | kernel code | PDS | as above | as above |
| TA3D control block | 0xec | kernel | kernel data | EDM | device takeover: holds the PD address and every pointer | P2, P4 |
| host control block | 0x44 | kernel | kernel data | EDM, host | forged init, power and cleanup status; host treats the fields as data and range-checks them | P2 |
| kernel CCB + control | 0x2000 + 8 | kernel | kernel data | EDM, host | arbitrary firmware commands | P2 |
| event kicker | 4 | kernel | kernel data | HW | spurious events | P2 |
| misc info | 0x54 | kernel | kernel data | EDM, host | forged version; the kernel reads it only at load | P2 |
| HWPerf, TA and 3D signature buffers | 0x640c, 0x620, 0x2088 | kernel | kernel data | EDM | signature lists name registers the firmware samples | P2 |
| HW render context | 0x44 | kernel | kernel code | EDM | points the firmware at any PD | P2 |
| HW transfer context | 0xb4 | kernel | kernel code | EDM | as above | P2 |
| command rings + control | 0x10420, 0x104c8, +8 | kernel | kernel data | EDM | arbitrary commands in any context | P2 |
| TA command + `CMDTA_SHARED` | 0x2c0+ | kernel | kernel data | EDM | status lists write values to chosen addresses | P2, copy and check |
| transfer command + shared | 0x1c0 | kernel | kernel data | EDM | as above | P2, copy and check |
| render details | 0x1ec | kernel | kernel data | EDM | register values at 3D start, render-in-flight list | P2 |
| RT data + entries | `nbuf·0x38+0x18` | kernel | kernel data | EDM | back-pointers to contexts and render details | P2 |
| dependency block | 0x28 | kernel | kernel data | EDM (G15) | sync addresses | P2 |
| HW device sync list | variable | kernel | kernel data | EDM | writes values to chosen addresses | P2 |
| sync and status words | 4 each | kernel | sync | EDM, host | forged fence signals | P2 |
| PB descriptor + block list | 0x3c, 0x10 | kernel | kernel data | EDM | free-list and threshold registers | P2 |
| TA-data blocks | 0x88, 0x40, 0x50 | kernel | TA data | gap (G15) | **if EDM reads these, they need the kernel data range instead** | G15 |
| scratch block | 0x40 | kernel | 3D params | EDM (G15) | as above | G15 |
| free list | 0x40000 | kernel | 3D params | HW (DPM) | bad page indexes: stays inside this VM's 3D params range; hang at worst, kernel rebuilds it on reset | PD |
| PB | ≥ 0x22 pages | kernel | 3D params | HW | own geometry only | PD |
| region headers | 2 × varies | kernel | TA data | HW | own render only | PD |
| tail pointers | varies | kernel | TA data | HW | own render only | PD |
| background block (render) | 0x200 | kernel | 3D params | HW | own render only | PD |

The bottom five are in the client's PD because the hardware walks them under it. A client
shader can reach them (a load from their VA), and that is self-harm only. The kernel never
reads them back and never trusts their contents.

### 7.4 Every userspace-supplied address

**Class A:** copied by the kernel from a stream into a firmware command. The kernel can
check it.

| address | checks | dereferenced by | if it is wrong |
|---|---|---|---|
| `vdm_base` | below 2^32, aligned, mapped at submit | HW (VDM) | fault or hang in this VM |
| `depth_addr`, `stencil_addr` | below 2^32, extents mapped, ZLS allow-list and pairing (section 3.4) | HW (ZLS) | **hardware:** 3D lockup, and once a failed recovery and a power cycle (TEX.md 7) |
| `pixel_event_pds_addr` | in `PDS_PIXEL`, aligned, rows mapped | HW (PDS) | fault or hang |
| `bg_pds_secondary_addr`, `bg_pds_primary_addr` | in `PDS_PIXEL`, aligned | HW (PDS) | fault or hang |
| `bg_block_addr` (transfer) | below 2^32, aligned, 0x200 bytes mapped | HW (ISP) | fault or hang |
| transfer `pixel_event_pds_addr` | as above | HW (PDS) | fault or hang |
| `VM_MAP.device_addr` | below 2^32, page aligned, inside one heap, no overlap | kernel | `-EINVAL` or `-EEXIST` |
| `sync_ops`, `bo_refs`, `ta_kicks`, `jobs`, `cmd_stream` (job and kick), `DEV_QUERY.pointer`, `quirks`, `heaps` | CPU user pointers: `copy_from_user` | kernel | `-EFAULT` |

**Class B:** inside client memory, read by the hardware at draw time. The kernel doesn't
look at these and can't: the client can change them after submit, and a shader can compute
them.

| address | where | reaches |
|---|---|---|
| index buffer | VDM draw word 1 | this VM (PD) |
| vertex PDS program | VDM draw word 4 | this VM (PD) |
| state programs | VDM type 4 and 6 | this VM (PD) |
| TA state block PDS words | TA block group 0x40 | this VM (PD) |
| DOUTD source | PDS data | this VM (PD) |
| DOUTU code offset and base | PDS data | **P3** |
| DOUTT T2 | PDS data | this VM (PD) |
| PBE address | end-of-tile `limm` | this VM (PD) |
| `ld`, `st` operands | USSE registers | this VM (PD), **P2** |
| `ldr`, `str` operands | USSE registers | **P1** |
| transfer background block words 0-2 | client memory | this VM (PD) |

These all rely on the PD and the premises. No validation in the kernel helps, short of D9.

### 7.5 Other attacks

| attack | defense | evidence or gap |
|---|---|---|
| Stale structure read by the microkernel | The microkernel "reads the previous client's structures at the next TA" (GEN.md). Kernel-owned memory stays allocated and mapped until the cleanup command completes, or the next render has passed. It is never reused for anything a client can write. | capture and hardware for the problem; gap for the cleanup, which timed out in the only test (G15) |
| Unmap during a job | The job keeps its VM alive. Pages stay allocated until the translation caches are invalidated and earlier jobs finish | design |
| Forged fence | Fence words sit in the sync range, kernel-only (P2). Fences are signaled from the interrupt path by the kernel, after reading firmware state it validates as data | design |
| Host reads hostile values | Everything the host reads from GPU-writable memory is range-checked: ring offsets modulo the ring, status words as bitmasks. A compromised firmware is a separate problem, but a shader that reaches kernel memory must not become a kernel bug | design |
| Starvation by holding a scene open | Not possible: a render job is a whole scene (D3) | design |
| Unbounded firmware work per ioctl | `max_ta_kicks`, stream length capped at 4 KiB, ring space accounted with `drm_sched` credits | design |
| Reset loop | A guilty context is refused from then on (section 8). A client can create new contexts, so a hang rate limit per file may be needed | gap for whether it is needed |
| Failed recovery | The open microkernel discards surviving state rather than rescanning it (UKERNEL.md 5a). If re-init fails the device is lost: `-EIO` and a wedged event | capture for the DDK's failure; gap for the open path on hardware |
| Foreign dma-buf outside the MMU's physical reach | `VM_MAP` refuses an import whose pages the DMA layer bounced (no IOMMU) or that lie above 4 GiB | design; the bounced case is untested (the EA has no memory above 4 GiB) |
| Non-zeroed memory | shmem pages are zero. Kernel-owned PB, region headers and tail pointers are zeroed before they are mapped | design |
| Sticky TA state across contexts | "Every group of the TA state block is sticky" (STATE.md). A client's first block must carry every group it relies on. Not a kernel duty, but IGT should exercise it | capture for one client; gap across clients (G6) |

### 7.6 Tests this implies

Each rule in sections 2 to 5 gets an IGT negative test: reserved fields non-zero, flags
outside the allow-lists, addresses above 2^32 and outside heaps, unaligned PDS and VDM
addresses, extents crossing the end of a mapping, `zls_control` b0 with no stencil,
`ta_kicks` empty and over the limit, render jobs on transfer contexts and the reverse,
handles from another file. Malformed streams too: a non-zero header word, a main stream of
the wrong length, non-zero alignment bytes, trailing bytes, an unknown header type or bit,
a header type twice, a stream over 4 KiB. `uapi/stream/test/stream_test.c` already runs
each of these against the reference decoder on the host. M2's exit test, that one client can't corrupt another, needs
P1 to P4 as positive tests from an actual shader. It can't be met by ioctl tests alone.

## 8. Hardware recovery as userspace sees it

The mechanism (UKERNEL.md 5a): a job that passes the `drm_sched` timeout triggers a soft
reset of the cores. Device memory survives. The microkernel is re-initialized, and in the
open microkernel it discards the surviving CCB work and in-flight list. Then the kernel
restarts the scheduler. The kernel rebuilds the affected context's free list and RT data
(what exactly needs rebuilding is G21).

| context had | fence error | `GET_CONTEXT_STATE` | later submit |
|---|---|---|---|
| the job that timed out, or faulted if the MMU reports faults (G3) | `-ETIME` (fault: `-EFAULT`) | `GUILTY` | `-ECANCELED` |
| a job on the hardware when another's job reset it | `-ECANCELED` | `INNOCENT` | `-ECANCELED` |
| jobs queued but not started | none: they run after the reset | `OK` | accepted |
| jobs queued behind a guilty or innocent context's job | `-ECANCELED`, without running | as that context | `-ECANCELED` |
| nothing | | `OK` | accepted |

- Error codes follow `dma_fence_set_error()`'s conventions, and `drm-uapi.rst` asks for
  affected contexts to be refused.
- Mesa maps these to `GL_GUILTY_CONTEXT_RESET`, `GL_INNOCENT_CONTEXT_RESET` and, for
  `-EIO`, `GL_UNKNOWN_CONTEXT_RESET`. It reads them with `GET_CONTEXT_STATE`; fence errors
  are also visible through `SYNC_IOC_FILE_INFO`.
- With one job on the hardware at a time, `INNOCENT` can't occur in the first driver. It
  is in the uAPI for the day TA and 3D of different contexts overlap.
- **Device lost.** Re-init failure means `-EIO` from then on, all fences signaled with
  `-EIO`, `drm_dev_wedged_event()` with a rebind or bus reset hint, and a devcoredump.
  TEX.md section 7 records one such recovery failure that needed a power cycle, so this
  path will be reached.
- A resets counter (`reset_count`) lets a client notice that resets are happening.

Which core hung, and whether detection belongs in the microkernel's timer path or the
kernel's scheduler timeout, are gaps (G17). The draft uses the scheduler timeout alone.

## 9. What is solid and what is a guess

| part | status |
|---|---|
| Struct layouts, on both targets | **Proven** by the compile (`uapi/check.sh`) |
| Stream framing, per-field and cross-field checks | **Tested** on the host against the reference decoder (`uapi/stream/test`); not yet in a kernel |
| Ioctl set and shape | **Follows pvr**; the reasoning is sound, but no code has run against it |
| Heap bases | **capture** |
| USSE reach, PDS address formats, request bases, free-list limit | **capture and hardware** |
| Client stream fields | **capture or hardware** per row; each has been poked or diffed. Whether they are *enough* for Mesa won't be known until M3 |
| Kernel-filled constants | **capture** for the values; the meanings are gaps. A value right for the probe frames may be wrong for some GL state |
| Heap sizes other than USSE | **Guess**: next base as an upper bound |
| Scene-level jobs | **Design**; rests on two kicks per frame as captured. N-kick scenes need a capture of the middle-kick flags |
| One PB per context, more than one in a VM | **Guess**: the single-PB path is all that is proven |
| Isolation | **Unproven**: rests on P1 to P4 |
| HWR behavior | **Design**, on a recovery path known only offline |

## 10. Decisions for Joseph

Joseph went through all ten on 2026-11-02; each section starts with what was decided. Two
changed the draft: the name (D1, `powervr_sgx`) and the stream encoding (D2). Both are now
applied, in sections 2, 3 and 6, `uapi/powervr_sgx_drm.h` and the stream definition
`uapi/powervr_sgx_kmd_stream.xml`. Where D2 needed something the other decisions don't
settle, it is a gap (G26, G27), not a change to them. D9's register-write test has run (D9).

### D1. Driver name (module, DRM name, Mesa loader key)

**Decided (Joseph, 2026-11-02): `powervr_sgx`.** Applied: the module and DRM name are
`powervr_sgx`, the header is `powervr_sgx_drm.h` after upstream's `<driver>_drm.h`, and
identifiers are `drm_powervr_sgx_*`, `DRM_IOCTL_POWERVR_SGX_*` and `DRM_POWERVR_SGX_*`.
pvr's identifiers use `pvr` although its module is `powervr`; the same shortening here
(`drm_pvr_sgx_*`) would read as part of the Rogue uAPI, so the name is spelled in full. The
original recommendation follows.

**Recommendation: `sgx`,** to be confirmed with the Imagination maintainers before any
RFC. Upstream's Rogue driver is `powervr`, so `pvr` and `powervr` are taken. Plain `sgx` is
short and covers later cores, but on Intel hardware it collides with Intel SGX (`/dev/sgx_enclave`,
`arch/x86/kernel/cpu/sgx`) in every search and bug report. `powervr_sgx` is the clearest
alternative and the better one if Imagination wants the family name kept together.
`sgx` is accurate for the only core we can test and doesn't stop another SGX core being
added later. Mesa's directory is the Mesa maintainers' call (`AGENTS.md` section 3.7). The
placeholder is one search-and-replace from any of these, until something merges.

### D2. Stream encoding

**Decided (Joseph, 2026-11-02): a `pvr_stream`-style stream, as `AGENTS.md` section 3.2
describes,** with field layouts held in Mesa's tables as upstream's Rogue driver does.
Applied in section 3: `pvr_stream` framing, made stricter (section 3.1), with the field
tables in one csbgen-format XML that generates the kernel's tables and the userspace
writer (section 3.6). The original recommendation follows.

**Recommendation: plain extensible structs in the uAPI header,** validated by a per-field
kernel table. pvr's `pvr_stream` format (scalar stream, extension headers, formats held
outside the uAPI header, in Mesa's `rogue_kmd_stream.xml` and `pvr_stream_defs.c`) exists
because Rogue's fields vary by BVNC feature. We have two core revs with the same errata as
far as known. A struct gives reviewers one place to read, and the same growth path through
length and zero-extension. `AGENTS.md` section 3.2 describes the stream approach as the
model, so this departs from it in form and not in function. **Closes nothing:** if Cedarview
diverges, a `pvr_stream`-style table can replace it behind a new job type.

### D3. Job granularity: whole scene, or one job per TA kick

**Decided (Joseph, 2026-11-02): whole scene, as recommended.**

**Recommendation: a render job is a whole scene,** with an array of kicks. Alternative: one
job per kick with FIRST and LAST flags, as SGXMKIF has, or separate geometry and fragment
jobs, as pvr has.

- For: no half-open scene can sit in firmware waiting on userspace, so the timeout covers
  all of it, and a client can't stall others by never sending LAST. That matters while the
  open microkernel has one render-in-flight slot. It also gives one fence and one validation
  pass.
- Against: Mesa can't flush TA alone to reclaim ring memory while a scene is open, and FBO
  scenes can't interleave kick by kick as in TEX.md section 4, though flushing the FBO scene
  first gives the same result. PB overflow in mid-scene isn't helped either way (G18).
- **Closes nothing:** a kick-level job type can be added later.

### D4. Parameter buffers and RT data: hidden, or client-allocated

**Decided (Joseph, 2026-11-02): kernel-allocated and hidden, as recommended.**

**Recommendation: kernel-allocated and hidden, behind `CREATE_CONTEXT` and
`CREATE_RT_DATASET`.** pvr has userspace allocate the free-list and HWRT memory in BOs marked
`PM_FW_PROTECT`, and pass their addresses. Here the kernel can compute all of it from the
sizes (STATE.md 6), and it has to rebuild the free list after a reset anyway. A protected-BO
scheme would add validation with nothing to gain, and it would rest on a protection bit whose
enforcement isn't established (P2). Cost: Mesa loses control of placement and of sharing a PB
between contexts. Adding a separate PB object later is additive, through
`CREATE_CONTEXT.flags`.

### D5. Unnamed command words: kernel constant, or client value

**Decided (Joseph, 2026-11-02): kernel constant until named and tested, as recommended.**

**Recommendation: kernel constant until named and tested,** as in section 4. Alternative: a
raw word in the stream with a mask. For: the client surface stays small, and every field in
the uAPI means something reviewers can check. Against: an M6 to M8 feature that needs one of
these words (MSAA, multi-RT) waits for a uAPI addition, which is append-only and cheap.

### D6. BO lists

**Decided (Joseph, 2026-11-02): an optional per-job list of shared objects, as recommended.**

**Recommendation: an optional per-job list of shared objects, for implicit sync only.**
Residency needs no list because `VM_MAP` pins pages, and private objects are covered by the
VM's reservation object. Alternative: no list at all, with userspace using
`DMA_BUF_IOCTL_EXPORT_SYNC_FILE` and `IMPORT_SYNC_FILE` around submits, which is the newer
style and fits Wayland explicit sync. The list is cheaper for X11 and old Wayland paths, and
matches what `AGENTS.md` section 5 asked for. **Closes nothing:** an empty list is valid.
**Amended (2026-11-09):** the list is also how a job names the imports it touches, and
it must name every one (CREATE_BO, GET_BO_MMAP_OFFSET, imports): the kernel flushes only listed imports.

### D7. VM contexts: explicit handles, or one per file

**Decided (Joseph, 2026-11-02): explicit handles, as recommended.**

**Recommendation: explicit handles, as pvr.** One VM per file is simpler and is all Mesa needs
at first, but explicit handles are `AGENTS.md`'s house model and leave room for
per-context VMs. Client-chosen addresses (not kernel-placed) fit a 4 GiB space with fixed
heaps.

### D8. Host interface

**Decided (Joseph, 2026-11-02): SGXMKIF-compatible through M2 bring-up, decide after M1, as
recommended.**

**Recommendation: keep SGXMKIF compatibility through M2 bring-up, as
`docs/UKERNEL-DECISIONS.md` records, and decide after M1.** This uAPI doesn't depend on the
answer. Reasons that may force a change: P4 (reload `sa` on entry), P2 (a kernel-only PD for
EDM), concurrent renders (UKERNEL.md section 9), and per-job completion words. Each is a
firmware-ABI bump, versioned through `fw_interface_version`.

**Consequence found on hardware (EA1, 2026-11-03): the isolation this uAPI promises needs
the open microkernel.** The DDK microkernel runs the EDM under the job's page directory
during a TA kick (P2 row in 7.2), so the kernel heaps have to be mapped in every client
directory while it runs, and P2 and P3 stay open. The DDK microkernel is therefore for
bring-up and A/B testing only; the secure configuration is the open microkernel, which keeps
the EDM on its own directory list. M1 is a prerequisite for it, not just for linux-firmware.

### D9. What to do if P1, P2 or P3 fails

**The experiments ran (7.2, 2026-11-02): P1, P2 and P3 fail, so this is in force.** A client
shader can reach registers (P1) and, as the DDK maps memory, kernel memory (P2), and can
select the kernel code base (P3). P2 and P3 are the new kernel driver's to fix by owning the
page directory (don't map kernel heaps, kernel code or base 15 into a client PD). P1 is not:
`ldr`/`str` reach the registers from any task whatever the PD. The options for P1 are:

1. **Validate shaders in the kernel.** `vc4` does this upstream for a GPU without an MMU
   (`vc4_validate_shaders.c`). It needs code heaps writable only by the kernel, so an upload
   ioctl, and a scan of every USSE and PDS program. Mesa generates PDS programs per draw, so
   the cost is real, and it needs the full ISA in the kernel.
2. **Change the firmware** to run EDM from a protected context, if the BIF allows it.
3. **One client at a time,** with a full scrub between clients, which is not useful on a
   desktop.
4. **Render access restricted to a trusted group.** Not acceptable to upstream as a general
   driver.

This could end the upstream plan as written, so it deserves your attention before M2 code.

**Decided (Joseph, 2026-11-02): run the register-write test first (does a client `str` to an
effective register such as `BIF_DIR_LIST_BASE0` take effect?), on the EA1, then decide.** The
plan is option 1, kernel shader validation, scoped by that result: full coverage of stores if
writes land, a narrower check if only reads do.

**Result (EA1, 2026-11-02, hardware; log 2026-11-02-2021):** the store does **not** take —
`BIF_DIR_LIST_BASE0` read back unchanged from both the host and the shader in a frame that
completed, so a client gets **no translation escape** from a register write (the page-directory
boundary holds against it). That store is also **not a denial of service**: an isolated re-run
(Joseph's go, on a clean cold boot with WPE off and the GPU idle) left the box up with 0 SGX
recoveries over ~2.5 min — no reboot, no wedge. (The first run's reboot was a confound: another
session's dEQP chunk had wedged and rebooted the box just before; not this store.) **Scope:** three
registers were tried from a client task — `BIF_DIR_LIST_BASE0` and the `USE_CODE_BASE_14`/`_15`
controls — all dropped. This is not general: the microkernel is a USSE program that writes registers
with `str` (UKERNEL.md §8), so stores land in *some* context, and untested registers (`SOFT_RESET`,
`EVENT_KICK`, clock/power) could be destructive. So option 1 forbids **both** client `ldr` (reads
leak `EUR_CR_*`, incl. the PD physical base — firm) **and** `str` (the same scan, so cheap; justified
until we know why a client store is dropped, e.g. a per-task-type gate). The cost is as weighed
above.

### D10. PB overflow

**Decided (Joseph, 2026-11-02): fail the job's fence with `-ENOSPC` in the first driver, as
recommended.**

**Recommendation: fail the job's fence with `-ENOSPC` in the first driver, and have Mesa
size the PB and flush conservatively.** Alternative: kernel or firmware grows the PB, or runs
partial renders. Partial renders need ZLS load, which isn't found (TEX.md section 7). This
leaves room for growth later with no uAPI change. It may be wrong for Minecraft-sized scenes
(G18).

### D11. The NULL job type

**Decided (Joseph, 2026-11-03): keep `DRM_POWERVR_SGX_JOB_TYPE_NULL`.** A job with no GPU
work that still goes through the scheduler and signals its fences, for IGT, fence and
scheduler tests without a GPU workload. It can be dropped before the RFC if reviewers object.

### D12. Code objects and the upload ioctl

**Decided (Joseph, 2026-11-08): USSE code goes only through `UPLOAD_CODE` into
`BO_CODE` objects, as section 2 describes.** This is D9 option 1
made concrete, and it closes some options:

- The winsys can no longer write shaders into a mapped buffer. Mesa's code heap becomes
  a code object per pool, filled by `UPLOAD_CODE` (M3).
- An upload into a mapped object can only append. Freeing and reusing shader space
  means unmapping the object first, so a pool that recycles space needs a second pool
  to swap with. Alternative: allow overwrites of ranges no submitted job can still be
  running. The kernel would need to know which jobs could use which code, which it
  doesn't.
- `setl`/`savl` are refused, and so are relative branches out of the object. A future
  compiler that needs indirect calls would need a rule for where `setl`'s register can
  point.

Alternatives weighed in D9 (firmware-protected EDM, one client at a time, a trusted
group) still stand if this is rejected.

## 11. Doc gaps

Facts the design needs that `docs/` doesn't have.

| # | gap | blocks | where it would go |
|---|---|---|---|
| G1 | `ldr`/`str` from a client task (hardware, 7.2). Reads of `EUR_CR_*` work (P1 — leak). A `str` to `BIF_DIR_LIST_BASE0` is **dropped** — **no translation escape** — and **not a DoS** (isolated re-run, clean cold boot, GPU idle: 0 SGX recoveries, no reboot). Same for the `USE_CODE_BASE_14`/`_15` controls — three registers, all dropped. **Open:** not general — the microkernel writes registers via `str` (UKERNEL.md §8), so stores land in some context; which registers/task types drop a client store is unknown (`SOFT_RESET`/`EVENT_KICK`/clock/power untested, possibly destructive). So forbid client `ldr` (leak) **and** `str` (same scan); relax `str` only with a per-task-type gate explanation | isolation | ISA.md, logs 2026-11-02-0710 / 2026-11-02-2021 |
| G2 | BIF requestors and banks: which units translate through which directory, whether EDM can use its own; registers 0xc84 and BANK0 fields. P2 showed the DDK maps kernel memory in the client-visible translation; the per-requestor bank layout (BANK0 EDM/TA/HOST/3D/2D indices, sgx545defs) is the lever a new driver uses to separate them | isolation, kernel MMU | UKERNEL.md, log 2026-11-02-0710 |
| G3 | The MMU: PD and PT entry formats, page sizes, physical address width, read-only and `EDM_PROTECT` bits and whether each is enforced, invalid entries, fault status and interrupts, translation-cache rules | kernel MMU, `VM_MAP`, P2 | new section, UKERNEL.md |
| G4 | DOUTU w0 b27 and base 15 from client programs; `USE_CODE_BASE` flags `0x2000000` and `0x6000000` (P3) | isolation | PDS.md |
| G5 | Secondary attributes (P4): resolved on hardware. A client task's `sa` holds its own PDS-loaded inputs, not the microkernel's `sa` state (log 2026-11-02-0710), and the open microkernel reads no `sa` (f672198), so a client cannot steer it through `sa`. Whether a client write survives into a DDK microkernel entry is untested | isolation, firmware | UKERNEL.md, ISA.md, logs 2026-11-02-0630/0710 |
| G6 | Leftovers across clients: USSE temporaries, `k18`, sticky TA state, and `sa` (within one client, `sa` and a prior draw's clear color leak across draws — hardware 7.2 P4). The cross-*client* case needs two simultaneous clients, untested | threat model | ISA.md, STATE.md, log 2026-11-02-0710 |
| G7 | UKERNEL.md section 3 says the TA kick sets "the PDS and USE code bases for the context"; SRVINIT.md sets them once at init, and transfer word 15 sets 0xab8. Global or per context? | heap layout | UKERNEL.md |
| G8 | Heap sizes, and which ranges are hardware requirements; whether PDS programs can run outside their heaps | heap table, `HEAP_INFO` | NOTES.md |
| G9 | Scene shape: does one kick with both first and last work? Does the VDM stream have a jump? | D3, `ta_kicks` minimum | STATE.md 2 |
| G10 | Minimum alignments: VDM base, PDS, USSE, DMA (also listed in DRIVER-UPSTREAM.md section 3) | validators | GEN.md |
| G11 | Where cmd+0x08 to +0x10 go: the render path's block words 0-2 are 0, and UKERNEL.md section 7 has no register for them | stream | UKERNEL.md |
| G12 | Unnamed words. TA flags: STATE.md section 1 refers to a bit table that isn't there; `0x71`/`0x62` there, `0x63`/`0x10` in DRIVER-UPSTREAM.md; no middle-kick value. TA flags 2, 3D flags, cmd+0x20, +0x28-0x37, +0x48-0x50, +0x254, +0x26c-0x298; rd+0x08, 0x0c, 0x1c, 0x24, 0x44, 0x54, 0x64, 0x6c-0xc8, 0xcc; PDS middle word. GEN.md lists `+0x104` under render details, STATE.md under the TA command | D5, what Mesa can do | STATE.md |
| G13 | ZLS: the load enable, field widths, range of the offsets, which bits touch the stencil address | `zls_control` allow-list | TEX.md 7 |
| G14 | Render target limits: maximum width and height, TE field widths, multisample numbers | `LIMITS`, `CREATE_RT_DATASET` | STATE.md 4, 6 |
| G15 | Lifetime and ownership: purpose of the 0x88, 0x40 and 0x50 TA-data blocks, scratch block and dependency block, and who reads each; `SGX_FLUSH_HW_RENDER_TARGET` timing out (GEN.md) and the DDK microkernel's RT and PB cleanups never completing (UKERNEL.md 3); concurrent contexts (UKERNEL.md section 9). **PB placement:** the DPM numbers PB pages from the 3D-params heap base, and a PB elsewhere hangs (EA1, 2026-11-03, log 2026-11-03-0340-drm-per-vm-pb). So the 3D-params heap is per VM, with one PB per VM at its base, shared by the VM's render contexts (verified with three contexts, log 2026-11-03-0355-drm-shared-pb-restart). Open: whether the DPM state of a shared PB stays right when contexts' renders interleave beyond one job at a time | section 7.3, destroy paths | UKERNEL.md, GEN.md |
| G16 | Preemption or context suspend during a job: does type 4 do anything useful for scheduling? | `priority` | UKERNEL.md |
| G17 | Hang detection: which core hung, EDM timer path, signature registers' use, how the host learns it | section 8 | UKERNEL.md 5a, 9 |
| G18 | PB overflow: DPM out-of-memory path, thresholds, partial renders | D10 | UKERNEL.md, STATE.md 6 |
| G19 | CPU and GPU coherency: `ui32CacheControl` bits, what the BIF snoops, CPU mapping modes | `CREATE_BO`, fence ordering | UKERNEL.md |
| G20 | Transfer: word 1, 3, 4, 5, 14, +0x194 meanings; word 11 (code size in TEX.md, a flag in UKERNEL.md); register 0x408 (region headers on a render, depth/stencil block on a transfer); transfer destination sync in the open microkernel (UKERNEL.md section 10). Completion: the DDK microkernel sets the shared command's destination sync (`asDstSyncs[0]`, +0x134), which the driver's transfer jobs complete on (EA1, log 2026-11-03-0405-drm-transfers); words 0x168/0x16c are `sCtlStatusInfo[0]` | section 3.5, transfer fences | TEX.md, UKERNEL.md |
| G21 | What state needs rebuilding after a soft reset, per structure | section 8 | UKERNEL.md 5a |
| G22 | Interrupts: can the kernel identify which job completed from host control fields alone? | fences | UKERNEL.md 5 |
| G23 | Power: what type 3 needs for runtime PM (states, resume path) | kernel | UKERNEL.md |
| G24 | Per-context firmware state beyond the HW context: words 8-15 and the fields at +0x3c and +0x40 that the commands use | `CREATE_CONTEXT` | STATE.md 6 |
| G25 | Cedarview: `EUR_CR_CORE_REVISION` for 10131, errata, heap layout, microkernel differences; documented errata for 1014 | quirks, M4 | STATE.md, NOTES.md |
| G26 | Stream tooling in Mesa (from D2). csbgen parses `<stream>` but emits nothing for it; Rogue writes its streams by hand in the XML's order. Our writer comes from `gen_stream.py`, and the XML carries validation attributes csbgen ignores. Whether Mesa takes `gen_stream.py` beside csbgen (or a `<stream>` emitter in csbgen), and the attributes in its XML, is the Mesa maintainers' call. The kernel's tables are generated from that file and committed; kernel reviewers may want the source in the kernel tree instead | M3 winsys, M5 | `AGENTS.md` 3.7, DRIVER-UPSTREAM.md |
| G27 | Cross-field rules (from D2). csbgen refuses unknown tags, so rules that join fields (section 3.4's depth and stencil requirements and ZLS offset limit, section 3.5's tile order) are hand-written in the kernel's per-command check, not generated. They are the one part of the validation with two copies, section 3 and `pvrsgx_cmd_checks.c`, so a rule added to one can be missed in the other | validators | UAPI.md 3, `uapi/stream/` |
