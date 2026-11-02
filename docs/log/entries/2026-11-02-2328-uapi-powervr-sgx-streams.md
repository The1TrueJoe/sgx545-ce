# The uAPI becomes powervr_sgx, with pvr_stream-style command streams

Applies decisions D1 and D2 (UAPI.md section 10) to the M2 uAPI draft. No hardware was
touched, and D3 to D10 are unchanged.

## What changed

- **D1, the name.** The header is now `docs/uapi/powervr_sgx_drm.h`, named like
  upstream's `<driver>_drm.h` headers. Every identifier is `drm_powervr_sgx_*`,
  `DRM_IOCTL_POWERVR_SGX_*` or `DRM_POWERVR_SGX_*`. Upstream's Rogue driver has the module
  `powervr` but the prefix `pvr`, so shortening ours the same way (`drm_pvr_sgx_*`) would
  look like part of the Rogue uAPI. The name stays in full.
- **D2, the streams.** The three command structs are gone. `CMD_RENDER`, `CMD_TA_KICK` and
  `CMD_TRANSFER` are scalar streams framed as in `pvr_stream.c`: an 8-byte header, a
  frozen main stream, then extension streams behind `__u32` headers. The decoder is
  stricter than pvr's in four ways: the main stream must be exactly the known length,
  alignment bytes must be zero, nothing may trail the last stream, and no header type may
  repeat. It also checks every field, which pvr doesn't.
- `ta_kicks` is now an array of `drm_powervr_sgx_ta_kick` `{cmd_stream, cmd_stream_len,
  flags}`, one `CMD_TA_KICK` stream per kick, so D3's whole-scene job is unchanged. The
  transfer's `DST_TWIDDLED` moved from the stream to `drm_powervr_sgx_job.flags`, where pvr
  keeps its per-command flags.
- The fields and their checks are the ones the previous draft had.

## One source for both sides

`docs/uapi/powervr_sgx_kmd_stream.xml` is written in the format of Mesa's csbgen, the same
shape as Rogue's `kmd_stream.xml`. Each field carries its validation as attributes that
csbgen ignores, the way it already ignores Rogue's `feature`: `check`, `min`/`max`/
`max_limit`, `mask`, `align`, `heap`, `optional`, `extent`. `gen_stream.py` generates three
things from it:

- the kernel's decoded structs and stream tables (`docs/uapi/stream/powervr_sgx_stream_defs.{h,c}`,
  committed);
- a userspace writer per stream, for Mesa;
- the stream table in UAPI.md section 3.2.

`docs/uapi/stream/` also holds the reference decoder (`powervr_sgx_stream.c`) and the
cross-field rules the XML can't express (`powervr_sgx_cmd_checks.c`). Both are written in
kernel style for the M2 driver to take.

## Checks

`docs/uapi/check.sh` is green. The old checks all pass: struct layouts on i386 and x86_64,
C++, the negative control and the layout table. The new ones:

- the committed kernel tables match the XML;
- the decoder compiles with `zig cc` for `x86-linux-gnu` and `x86_64-linux-gnu`;
- the host stream test passes;
- the stream table in UAPI.md matches the XML.

When `MESA_SRC` is set, `check.sh` also runs Mesa 26.2.5's own `gen_pack_header.py` on the
XML and compiles what it emits. That passed here.

The stream test writes each stream with the generated writer and checks that it decodes
to the same values. It then breaks the framing, every field rule and every cross-field rule
in turn, and checks each one fails. A fixture XML covers extension streams: two header
types, a continuation, a quirk-gated stream, a must-have bit, and a 64-bit field after a
32-bit header, which forces an alignment gap. Mutating the decoder kills every mutant on
a reachable path. Two checks survive mutation because they can't be reached: a field
outside the decoded struct, and a main stream that ends early. The generator rules both
out, and they stay in as defensive checks.

## What D2 forced elsewhere

Two new gaps in UAPI.md section 11; no decision was reopened.

- **G26, Mesa tooling.** csbgen parses `<stream>` but emits nothing for it (Rogue writes
  its streams by hand), so our writer comes from `gen_stream.py`. Whether Mesa takes that
  script, or a stream emitter in csbgen, plus the validation attributes, is up to Mesa's
  maintainers. Kernel reviewers may also want the source of the generated tables in the
  kernel tree.
- **G27, cross-field rules.** csbgen refuses unknown tags, so the rules that join fields
  live in hand-written code and in UAPI.md: depth and stencil required by the ZLS bits, the
  ZLS offset limit, and the tile order. They are the only part of the validation with two
  copies.
