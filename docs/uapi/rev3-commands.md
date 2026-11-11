# uAPI rev 3: per-command structs (design draft)

Status: draft for review, nothing implemented. The SGX530 mapping (section 6)
waits on the 530 bring-up's field table and is provisional until then.

Rev 3 replaces the client command streams (UAPI.md section 3) with one plain
struct per command. It is independent of the explicit-sync work (M5/B3) and of
VM_BIND: `bo_refs`, `sync_ops` and the VM ioctls stay as they are.

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

struct drm_powervr_sgx_cmd_render {		/* 88 bytes */
	struct drm_powervr_sgx_cmd_head head;	/*  0 */
	__u64 depth_addr;			/* 16 */
	__u64 stencil_addr;			/* 24 */
	__u64 pixel_event_pds_addr;		/* 32 */
	__u64 bg_pds_secondary_addr;		/* 40 */
	__u64 bg_pds_primary_addr;		/* 48 */
	__u32 pixel_event_pds_rows;		/* 56 */
	__u32 bg_pds_secondary_rows;		/* 60 */
	__u32 bg_pds_primary_rows;		/* 64 */
	__u32 zls_control;			/* 68 */
	__u32 flags_3d;				/* 72 */
	__u32 bg_depth;				/* 76: IEEE float bits */
	__s32 depth_bias_units;			/* 80 */
	__u32 depth_bias_factor;		/* 84: IEEE float bits */
};

struct drm_powervr_sgx_cmd_ta_kick {		/* 32 bytes */
	struct drm_powervr_sgx_cmd_head head;	/*  0 */
	__u64 vdm_base;				/* 16 */
	__u32 ta_flags2;			/* 24 */
	__u32 _padding_1c;			/* 28 */
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
	__u32 bg_depth;				/* 52: IEEE float bits */
};
```

The fields, their meanings and checks are rev 1's, one for one (UAPI.md 3.3-3.5).
Which of them are common to every core is settled by section 6; any that are not
move to an extension before this is merged.

`struct drm_powervr_sgx_job` changes in two fields; the rest is as in rev 1:

```c
	__u32 _padding_c;	/* offset 12, was cmd_stream_len: must be 0 */
	__u64 cmd;		/* offset 16, was cmd_stream: user pointer to the type's command struct */
```

The struct keeps its size. For a render job `ta_kicks` holds
`struct drm_powervr_sgx_cmd_ta_kick` elements directly, with `stride` = the
element's `head.size`. A transfer job's `ta_kicks` stays empty.

## 3. Kernel rules

For a command at a user pointer (render, transfer):

1. `get_user(size, &cmd->head.size)`. Refuse with `-EINVAL` if `size` is below
   `sizeof(struct drm_powervr_sgx_cmd_head)` or above 4096.
2. `copy_struct_from_user(&k, sizeof(k), ucmd, size)`:
   - a shorter struct is zero-filled;
   - non-zero bytes past what this kernel knows give `-E2BIG`, returned as `-EINVAL`.
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
  - a known name this core doesn't support is `-EOPNOTSUPP`, so userspace can
    tell "not on this core" from "malformed";
  - a repeated name or a seventeenth extension is `-EINVAL`;
  - an unreadable pointer is `-EFAULT`.
- **Checks.** An extension's fields are checked like base fields, from the same
  tables, keyed by offset within the extension. An extension that wasn't sent
  is all zero for the cross-field checks.

Userspace learns which extensions a core takes from the core id it already
reads (DEV_QUERY GPU_INFO). It needs that id for the ISA anyway. `-EOPNOTSUPP`
covers a kernel older than the userspace.

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
rows16 + extent_field). A `<core>` child overrides any check attribute for
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
extension. This is why the base list above is provisional until the 530's table.

**SGX530 (provisional):** waiting on the 530 bring-up session's per-field
table: which of the fields above exist there with the same meaning and width,
their masks and ranges, and what the 530 needs that the 545 derives in the
kernel. Each answer maps to one of:
- same field, same checks: nothing to do;
- same field, other masks or ranges: a `<core name="sgx530">` override;
- a 545 field the 530 lacks: it moves to a 545 extension before rev 3 merges;
- a 530-only field: a 530 extension.

**SGX535 (the blueprint):** a third core is the test of the scheme. It should
need only steps 1-3 above. If it needs a base field changed, the base list was
wrong.

## 7. Plan

1. This doc, then the field split once the 530 table is in.
2. The XML and `gen_cmd.py`, which emit the header block, the kernel tables,
   the UAPI.md tables and the test.
3. **uapi-stream:** the winsys (`sgx_frame.c` fills the structs; the stream
   writer goes) and the test.
4. **m2:** the kernel (the copy, extensions and table checks in place of
   `pvrsgx_stream_process`), and the uAPI suite's command cases.
5. Hardware: the RC suites on the EA3 (dEQP, ff piglit, the hang test, the uAPI
   suite) against rev 1's numbers.
