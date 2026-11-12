# uAPI rev 3: per-command structs (design draft)

Status: approved. The uAPI header, the XML and generator, the reference checker
(`docs/uapi/cmd/`, tested on the host) and the winsys are done. The kernel half
(section 7, step 4) is next. The base/extension split follows the SGX530 field
table (shared notes, sgx530-job-uapi-deltas-2026-11-11).

Rev 3 replaces the client command streams (UAPI.md section 3) with one plain
struct per command. It is independent of the explicit-sync work (M5/B3) and of
VM_BIND: `bo_refs`, `sync_ops` and the VM ioctls stay as they are. The job gains
`cache_ranges` for cache maintenance of imports (section 8, from B3), which is pending
Joseph's decision.

## 1. What changes

| | rev 1 (streams) | rev 3 |
|---|---|---|
| render command | `CMD_RENDER` stream at `job.cmd_stream` | `struct drm_powervr_sgx_cmd_render` at `job.cmd` |
| TA kick | `ta_kick.cmd_stream` (a stream per kick) | the `ta_kicks` array holds `struct drm_powervr_sgx_cmd_ta_kick` directly |
| transfer command | `CMD_TRANSFER` stream | `struct drm_powervr_sgx_cmd_transfer` at `job.cmd` |
| growth | extension streams behind EXTHDR bits | a larger `size`, or an extension in the chain |
| core-specific fields | quirk bits on extension streams | extensions, allowed per core |
| kernel decoder | framing, EXTHDR, musthave, quirks | none: `copy_struct_from_user`, then table checks |
| Mesa | csbgen reads the XML, plus a stream writer | fills the structs from the uAPI header |

Removed: `HDR`, `EXTHDR_*`, `PVRSGX_STREAM_MAX_SIZE` framing, `musthave`, the
quirk hooks, `docs/uapi/stream/pvrsgx_stream.c`'s decoder, Mesa's
`src/sgx/kmd_stream` target and the csbgen constraint on the XML.

Kept:
- every per-field check (kind, range, mask, heap, alignment, extent);
- the mapped-extent check (`check_mapped`);
- the per-command cross-field checks (`pvrsgx_check_render`, `pvrsgx_check_transfer`).

## 2. The structs

Every command struct starts with the same 16 bytes, is naturally aligned, and
names its padding (must be zero). Sizes and offsets below are the generator's
static_asserts.

```c
/* The common head of every command struct. */
struct drm_powervr_sgx_cmd_head {
	__u32 size;		/* sizeof the struct as the caller built it */
	__u32 _padding_4;
	__u64 extensions;	/* user pointer to the first extension, or 0 */
};

/* The common head of every extension (Xe's drm_xe_user_extension). */
struct drm_powervr_sgx_ext {
	__u32 name;		/* DRM_POWERVR_SGX_EXT_* */
	__u32 _padding_4;
	__u64 next;		/* user pointer to the next extension, or 0 */
};

struct drm_powervr_sgx_cmd_render {		/* 80 bytes */
	struct drm_powervr_sgx_cmd_head head;	/*  0 */
	__u64 depth_addr;			/* 16 */
	__u64 stencil_addr;			/* 24 */
	__u64 pixel_event_pds_addr;		/* 32 */
	__u64 bg_pds_secondary_addr;		/* 40 */
	__u64 bg_pds_primary_addr;		/* 48 */
	__u32 pixel_event_pds_rows;		/* 56 */
	__u32 bg_pds_secondary_rows;		/* 60 */
	__u32 bg_pds_primary_rows;		/* 64 */
	__u32 bg_depth_f32;			/* 68: IEEE-754 binary32 bits */
	__s32 depth_bias_units;			/* 72 */
	__u32 depth_bias_factor_f32;		/* 76: IEEE-754 binary32 bits */
};

struct drm_powervr_sgx_cmd_ta_kick {		/* 24 bytes */
	struct drm_powervr_sgx_cmd_head head;	/*  0 */
	__u64 vdm_base;				/* 16 */
};

struct drm_powervr_sgx_cmd_transfer {		/* 56 bytes */
	struct drm_powervr_sgx_cmd_head head;	/*  0 */
	__u64 bg_block_addr;			/* 16 */
	__u64 pixel_event_pds_addr;		/* 24 */
	__u32 pixel_event_pds_rows;		/* 32 */
	__u32 first_tile_x;			/* 36 */
	__u32 first_tile_y;			/* 40 */
	__u32 last_tile_x;			/* 44 */
	__u32 last_tile_y;			/* 48 */
	__u32 bg_depth_f32;			/* 52: IEEE-754 binary32 bits */
};
```

The base structs hold only fields that every core has with the same meaning:
addresses (checked against named heaps), counts, the tile range and depth
values. Their meanings are rev 1's (UAPI.md 3.3-3.5). The three rev 1 fields
that are raw SGX545 register images go into the SGX545 extensions:

```c
#define DRM_POWERVR_SGX_EXT_SGX545_RENDER	1
#define DRM_POWERVR_SGX_EXT_SGX545_TA_KICK	2

struct drm_powervr_sgx_ext_sgx545_render {	/* 24 bytes */
	struct drm_powervr_sgx_ext base;	/*  0 */
	__u32 zls_control;			/* 16: mask 0x001ff007; b3-11 are the kernel's */
	__u32 flags_3d;				/* 20: mask 0x00000180; b8 uses the stencil */
};

struct drm_powervr_sgx_ext_sgx545_ta_kick {	/* 24 bytes */
	struct drm_powervr_sgx_ext base;	/*  0 */
	__u32 ta_flags2;			/* 16: mask 0x02c80000 */
	__u32 _padding_14;			/* 20 */
};
```

An extension left out reads as all zero, which is rev 1's "off" for all three
fields: no depth/stencil load or store, no 3D flags, no TA flags. So a 545
render that uses neither ZLS nor those flags needs no extension. Another core
sending them gets `-EOPNOTSUPP`.

`struct drm_powervr_sgx_job` changes in two fields; the rest is as in rev 1:

```c
	__u32 _padding_c;	/* offset 12, was cmd_stream_len: must be 0 */
	__u64 cmd;		/* offset 16, was cmd_stream: user pointer to the type's command struct */
```

The struct keeps its size. For a render job `ta_kicks` holds
`struct drm_powervr_sgx_cmd_ta_kick` elements directly (each with its own
extension chain), with `stride` = the
element's `head.size`. A transfer job's `ta_kicks` stays empty.

Why a kick carries `head.size` when the array has a stride: every command
struct then has the same head, so one copy routine and one size rule serve all
three. The kernel requires the two to be equal. That costs one compare and
catches a caller whose stride came from a different header than its struct.

## 2a. Old userspace on a rev 3 kernel

Rev 3 builds on rev 2 (VM_BIND), whose ioctls shifted down by one from rev 1.
The job struct keeps its layout in rev 3, so an older binary's stream would be
read as a struct: its length word as `head.size`, and its `depth_addr` at
offset 8 as the `extensions` pointer. Rev 3 makes that impossible:

| ioctl | rev 1 | rev 2 | rev 3 |
|---|---|---|---|
| VM_MAP / VM_UNMAP | 0x05 / 0x06 | - | - |
| VM_BIND | - | 0x05 | 0x05 |
| CREATE_CONTEXT .. DESTROY_RT_DATASET | 0x07-0x0b | 0x06-0x0a | 0x06-0x0a |
| SUBMIT_JOBS | 0x0c | 0x0b | **0x0e** |
| UPLOAD_CODE | 0x0d | 0x0c | 0x0c |

- **SUBMIT_JOBS gets a number neither earlier revision used:** 0x0e. 0x0b
  (rev 2's SUBMIT_JOBS) and 0x0d (rev 1's UPLOAD_CODE) stay empty slots, so
  `drm_ioctl()` returns `-EINVAL` before any argument is read. An older
  binary's first submit fails the same way every time.
  - 0x0d is not reused because a rev 1 UPLOAD_CODE (40-byte args) would land
    on it.
  - Retired numbers are never reused, by this revision or a later one.
  - Rev 1's SUBMIT_JOBS (0x0c) already reaches rev 2's UPLOAD_CODE, a rev 1
    vs rev 2 hazard that rev 3 inherits. The minor version below is what tells
    those stacks apart.
- **The DRM minor version becomes the uAPI revision:** 3 for this one.
  - The winsys refuses a powervr_sgx fd whose minor is below the revision it was
    built for, with a log line naming both. A new Mesa on an old kernel then fails
    at screen creation, not at the first draw.
- **Downstream only.** The gaps at 0x0b and 0x0d and the minor-as-revision scheme are
  for our out-of-tree stack, which has shipped revisions 1 and 2. The upstream
  series has never seen them: its first version has dense ioctl numbers and
  DRM version 1.0. Don't copy the gap into it.

## 3. Kernel rules

For a command at a user pointer (render, transfer):

1. `get_user(size, &cmd->head.size)`. Refuse with `-EINVAL` if `size` is below
   `sizeof(struct drm_powervr_sgx_cmd_head)` or above 4096.
2. `copy_struct_from_user(&k, sizeof(k), ucmd, size)`:
   - a shorter struct is zero-filled;
   - non-zero bytes past what this kernel knows give `-E2BIG`, as from every
     `copy_struct_from_user()` user (sched_setattr, openat2, clone3, pvr's
     `PVR_UOBJ_GET`): it tells new userspace "this kernel doesn't know my larger
     struct" apart from "malformed".
3. Refuse a copied `k.head.size` that differs from the size read in step 1, so
   a second thread changing the size between the two reads gets `-EINVAL`.
4. Refuse non-zero padding fields.
5. Walk the extensions (section 4).
6. Run the core's field checks, the mapped extents and the cross-field check,
   all on the kernel's copy.

TA kicks arrive through `pvrsgx_obj_array_get()`, which already copies each
element with `copy_struct_from_user` at the array's stride. Each element's
`head.size` must equal the stride. After that the element goes through steps 4-6.

**Growing a command.** A field appended later must take 0 to mean "absent, as
before". A zero-filled struct from older userspace then means what it always
meant. A check that refuses 0 (a row count of 1..63, say) can only apply to a
field that was there from the start. A later field whose valid values exclude 0
goes into an extension instead.

## 4. Extensions

- **Layout.** An extension is `struct drm_powervr_sgx_ext` followed by its
  fields, naturally aligned and padded to 8 bytes. Its size is fixed by its
  name, so a changed layout means a new name.
- **Names.** One space for all commands: `DRM_POWERVR_SGX_EXT_<CMD>_<WHAT>`,
  numbered from 1. The generator records which command each name belongs to.
- **The chain.** The kernel follows `next` from `head.extensions`:
  - at most 16 extensions;
  - a name may appear only once;
  - each name must belong to this command and be allowed on this core
    (section 5);
  - each extension is copied with `copy_from_user` at its fixed size.
- **Errors:**
  - an unknown name, or one that belongs to another command, is `-EINVAL`;
  - a known name this core doesn't support is `-EOPNOTSUPP`. This is deliberate
    (most DRM drivers return `-EINVAL` here): "valid, but not on this core" and
    "malformed" need different handling in userspace, a fallback versus a bug;
  - a repeated name or a seventeenth extension is `-EINVAL`;
  - an unreadable pointer is `-EFAULT`;
  - a non-zero tail is `-E2BIG`, for commands and query structs alike.
- **Checks.** An extension's fields are checked like base fields, from the same
  tables, keyed by offset within the extension. An extension that wasn't sent
  is all zero for the cross-field checks.

**What this kernel accepts** is a DEV_QUERY of its own, not a table keyed by
core id in userspace. That also covers an extension backported to an older
kernel:

```c
	DRM_POWERVR_SGX_DEV_QUERY_COMMANDS_GET,	/* struct drm_powervr_sgx_dev_query_commands */

struct drm_powervr_sgx_dev_query_commands {
	__u32 job_types;	/* bit n: job type n is accepted on this core */
	__u32 _padding_4;
	__u64 render_exts;	/* bit n: extension name n is accepted in a render command */
	__u64 ta_kick_exts;	/* ... in a TA kick */
	__u64 transfer_exts;	/* ... in a transfer command */
};
```

- Extension names stay below 64 so that a bit per name fits.
- A later command type appends a field; the query is size-versioned like the
  others.
- `-EOPNOTSUPP` remains the fallback for a caller that didn't ask.

**A value unknown for a core refuses the command.** The core table can leave any
value a command depends on unset: PDS bytes per row, rt_max, the transfer tile
limits, an alignment. Then that command is not offered on that core. Its
`job_types` bit is clear, and a submit is `-EOPNOTSUPP`. It never falls back to
another core's value. So until the 530's values are measured, the 530 refuses
RENDER and TRANSFER rather than run them with guessed state.

## 5. One source: the XML and the generator

`powervr_sgx_kmd_stream.xml` becomes `powervr_sgx_cmd.xml`, read only by the
generator. `gen_stream.py` becomes `gen_cmd.py`. Since csbgen no longer reads
the file, the schema is ours:

```xml
<cores>
  <core name="sgx545"/>
  <core name="sgx530"/>
</cores>

<command name="RENDER" type="render">
  <field name="zls_control" type="u32" check="mask" mask="0x001ff007">
    <core name="sgx530" mask="..."/>         <!-- a per-core check value -->
  </field>
  ...
</command>

<extension name="RENDER_EXAMPLE" command="RENDER" id="1" cores="sgx530">
  <field name="..." type="u32" check="range" min="0" max="3"/>
</extension>
```

Field attributes are rev 1's: check none/range/mask/addr, min/max/max_limit,
mask, heap, align, optional, extent (a number, rt_depth, rt_stencil,
pds_rows + extent_field). Two changes for other cores:
- `addr` means "below 2^va_bits and inside the named heap", with `va_bits`
  from the core table (32 on the 545, 28 on the 530), not "below 2^32";
- `pds_rows` replaces `rows16`: bytes per row come from the core table (16 on
  the 545, the two-dword DS0/DS1 interleave; the 530's value is unknown yet). A `<core>` child overrides any check attribute for
that core. A field with a `<core ... absent="true"/>` child is refused at
generation: a field some core lacks belongs in an extension.

The generator emits:
1. **The uAPI header's command section.** It rewrites the block between
   markers in `docs/uapi/powervr_sgx_drm.h`, which stays one self-contained
   file. It covers the structs, the extension structs, the `EXT_*` names and
   kernel-doc for every field.
2. **The kernel's tables (`pvrsgx_cmd_defs.c`/`.h`).** Per core and command,
   an array of `{offset, size, kind, min, max, limit, mask, heap, align,
   optional, extent, extent_field}`, each `offset` an `offsetof()`. One more
   array per extension, and a per-core list of allowed extension names per
   command. `static_assert`s pin every offset and size.
3. **The UAPI.md field tables**, as today.
4. **A test** that a zero-filled shorter struct and a non-zero tail behave as
   section 3 says. It replaces `docs/uapi/stream/test/stream_test.c`.

CI fails if any generated output is stale, as it does for the stream tables today.

## 6. Core variation

**What a new core adds:**
1. a core table entry (`pvrsgx_core.c`) that points to its generated command
   checks: per command, the base table, the allowed extensions and its
   cross-field check (a function, usually the shared one);
2. in the XML, `<core>` overrides on base fields whose valid values differ on
   it, and extensions for fields only it has;
3. a cross-field check of its own, only if its rules differ;
4. nothing in the uAPI header except its new extension structs. The base
   structs never change for a core.

**What decides base vs extension:** a field is base only if every supported
core has it, with the same meaning and width. Its valid values may differ
(per-core checks). A field one core lacks, or means differently, is an
extension.

**SGX530** (from its field table; nothing on it has run past CORE_ID):
- **No base field is missing or extra on the 530.** Every base field exists there
  (deduced: the same firmware path).
- **The register images** (`zls_control`, `flags_3d`, `ta_flags2`) have 545
  encodings, and the 530's are unknown. The 530 also lacks external Z for the ZLS.
  Hence the SGX545 extensions above. The 530 gets `EXT_SGX530_*` extensions once
  its ISP/ZLS/TA registers are decoded. Until then the 530 refuses RENDER and
  TRANSFER (section 4, unknown values), and its `job_types` says so.
- **28-bit VAs:** `va_bits` = 28 in its core entry. The ZLS cross-field rule
  (offset below 2^30 from a 1 MiB base) becomes "below 2^va_bits", which is
  vacuous on both cores today.
- **Heaps:** the 530 has its own layout (pds_pixel at 0x02c00000, 4 MiB).
  Checks name heaps, never addresses, so nothing changes.
- **`*_rows`:** the unit is per core (`pds_rows`); the 1..63 range is unknown on
  the 530, so it gets a `<core>` override once known.
- **Alignments** (PDS 16, VDM 4, bg block 16 and 0x200) are unknown on the 530:
  `<core>` overrides when known. Until then they are unset in its core entry,
  which keeps its commands refused.
- **Single context:** overlap is off and QUIRK_UNISOLATED is set; the BRN_28889
  invalidate split is the kernel's. None of it is a job field.
- **Its 2D unit**, if ever used, is a new job type, not a TRANSFER extension. The
  job-type enum stays open.

**DEV_QUERY additions** (the size-versioned query structs grow at their tails):
- `limits.va_bits`;
- `limits.caps`: bit 0 = backward (overlapping) blits, which the 530 lacks;
- `limits.max_layers` (1 on both today);
- per-core `rt_max_width/height` and transfer tile limits, hardcoded 545 values
  today.

So userspace never infers these from the core id. A future 545 extension for
backward blits is refused on a core without the cap.

**SGX535 (the blueprint):** a third core is the test of the scheme. It should
need only steps 1-3 above. If it needs a base field changed, the base list was
wrong.

## 7. Plan

1. Done: this doc, and the field split from the 530 table.
2. Done: `powervr_sgx_cmd.xml` and `gen_cmd.py`. They emit the header block,
   `docs/uapi/cmd/pvrsgx_cmd_defs.[ch]` (all-structs and per-core tables) and
   the UAPI.md table. `check.sh` fails if any of them is stale.
3. Done:
   - the reference checker, `docs/uapi/cmd/pvrsgx_cmd.c` (copy, extension chain,
     field and mapped checks) and `pvrsgx_cmd_checks.c` (cross-field);
   - its host test, `docs/uapi/cmd/test/cmd_test.c`;
   - the winsys: `sgx_powervr_sgx.c` fills the structs, checks the DRM minor and
     reads COMMANDS_GET;
   - the stream decoder, XML, generator and Mesa's kmd_stream target are deleted.
4. **m2, the kernel.** The kernel's links to the cmd files and its object list
   are in place. Left to do:
   - render and transfer job init call `pvrsgx_cmd_copy()` and `pvrsgx_cmd_check()`;
   - TA kicks go through `pvrsgx_cmd_take_elem()`, and `check_mapped` runs
     under the VM lock;
   - the core table points at `pvrsgx_cmd_core_<core>`;
   - SUBMIT_JOBS moves to 0x0e (0x0b and 0x0d empty), the DRM minor becomes 3, and
     DEV_QUERY_COMMANDS_GET plus the new limits fields are added;
   - the uAPI suite gets the command cases.
5. Hardware: the RC suites on the EA3 (dEQP, ff piglit, the hang test, the uAPI
   suite) against rev 1's numbers.
6. **D6 cache ranges (pending Joseph).** The header, `layout.inc` and UAPI.md are
   done (section 8). m2, on 881d8aeb:
   - the kernel rules in section 8, replacing the per-job whole-import flush;
   - the import-time write-back;
   - KUnit cases for the range math, and uAPI suite cases.
   Then the winsys lists the scanout's damage on X's blit jobs.

## 8. Cache ranges (D6, from B3)

**Why.** B3 (EA1, 720x480, X down) timed simpledrm's DIRTYFB at 4.2 ms/MB from its own
cached dumb buffer and 199 ms/MB (a flat 5 MB/s, uncached reads) from our imported
write-combined buffer. So the scanout stays simpledrm's dumb buffer and the GPU blits
into it. That makes the dumb buffer a cached import in X's VM. Flushing whole imports
around jobs costs about 1.3 ms each way per 720x480 buffer: the 12 ms a frame of
2026-11-09. A job names the bytes it touches instead, so the cost follows the damage.

**The struct**, one per range, in `drm_powervr_sgx_job.cache_ranges` (appended at 0x50; a
job stride of 80 leaves it empty):

```c
struct drm_powervr_sgx_cache_range {	/* 32 bytes */
	__u32 handle;	/* an import */
	__u32 flags;	/* READ (bit 0), WRITE (bit 1), at least one */
	__u64 offset;	/* first row */
	__u64 size;	/* bytes per row, not 0 */
	__u32 stride;	/* bytes between rows, >= size when rows > 1 */
	__u32 rows;	/* 0 or 1: one row */
};
```

The range is 2D because a damage rectangle is rows of a linear buffer. A 64x16 rect at
pitch 2944 is 4 KB as 16 rows of 256 bytes, against 47 KB as one span over the rows.
There's no whole-object form: a client without damage lists `{0, object size}`, which
is today's cost by choice.

**Kernel rules:**

| case | result |
|---|---|
| more than 64 entries | `-EINVAL`, as the other arrays |
| unknown handle | `-ENOENT` |
| not an import (our own objects are write-combined) | `-EINVAL` |
| `flags` 0, or bits other than `READ`/`WRITE` | `-EINVAL` |
| `size` 0; `rows` > 1 with `stride` < `size` | `-EINVAL` |
| `offset + (rows - 1) * stride + size` past the object, or overflowing | `-EINVAL` |

The kernel takes a reference on each listed object at submit and drops it when the job
is freed, as `pvrsgx_prime_imports()` does now. No fence is attached anywhere: sync is
`sync_ops`, and `bo_refs` for implicit sync.

**Flushes.** Each range is widened to whole cache lines. On x86, `clflush` writes back
and drops, so both points run the same loop, with `mfence` around it.

- **Before the job:** every range. A GPU read then sees the CPU's writes, and no dirty
  line can land on a GPU write later.
- **After the job's writes have reached memory, before its out-fence signals:** every
  `WRITE` range. The CPU may have fetched those lines speculatively during the job. The
  µK's end-of-job data-cache flush is what puts the GPU's writes in memory first. Dropping
  the lines any earlier would let a prefetch refill them stale.
- **At import, once:** the whole object, before it can be mapped.

The kernel flushes through the exporter's CPU mapping (`dma_buf_vmap`, held for the
import's life), never the attachment's pages. An importer may not touch those. **So an
import must come from an exporter that supports vmap,** and other imports are refused.
An `is_iomem` mapping needs no flush.

**A job whose fence carries an error leaves its `WRITE` ranges undefined.** The reset and
device-lost paths signal under the fence lock with no after-flush, and a
millisecond-scale `clflush` doesn't belong under a spinlock. Any stale line left is the
object's own earlier bytes.

**An unlisted byte has undefined contents after the job,** for the CPU and the GPU: the
GPU may read stale DRAM, a dirty CPU line may overwrite its output, and the CPU may read
a stale line. **It is never a security issue.** Our own objects' pages are flushed when
they become write-combined, and an import is written back once at import time, so a stale
line or DRAM word is always the same object's own earlier contents. The process could
read those anyway. Without the import-time write-back, a freshly zeroed shmem page whose
zeros are still in the cache would show the GPU the page's previous owner's data. That
write-back is the one new cost: about 1.3 ms once per 720x480 import.

**Not covered:** writes by another process into a cached dma-buf the job reads. That's
the same open problem panthor has, and it isn't solved here.

**Use (the winsys and X):**

- A job that draws into the scanout lists it `WRITE` with the damage: `offset = y0 *
  pitch + x0 * 4`, `size = w * 4`, `stride = pitch`, `rows = h`.
- Jobs that don't touch it list nothing.
- A job that reads an import the CPU wrote through a map lists that range `READ`.

**Alternatives considered:**

- **A coherence flag on the `VM_BIND` mapping:** it flushes the whole flagged buffer on
  every job in the VM, which is the 12 ms again.
- **A panthor-style `BO_SYNC` ioctl** (`{handle, op, offset, size}`, in mainline for
  panthor and panfrost): an established upstream shape. But it's linear, so a rect is one
  op per row, and it's a second ioctl, after waiting for each job. It remains the fallback
  if reviewers object to cache work in the submit path.
