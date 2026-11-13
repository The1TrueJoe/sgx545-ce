# Moving the shipped image to features 6 (DONE_SEQ)

The open microkernel can raise a host interrupt per completed job, so the driver
drops its 1-jiffy completion poll (M8, docs/UKERNEL.md). That path is gated behind
a header feature bit, `PVRSGX_UKFW_DONE_SEQ = BIT(2)`. The shipped image is built
`UK_FEATURES=2` (SECURE only) until the kernels that read the bit are the ones we
ship. This is the checklist for flipping it on.

## The two words that must agree

- **Image features word** — `ukernel/build.py`, `UK_FEATURES` (default `2`). mkukfw
  writes it into the file header; `mkukfw --check` prints `features 0x…`. Bits 0-15
  are host-visible (the driver must know them); bits 16-31 are the microkernel's own
  build variants, which the driver accepts and ignores.
- **Kernel accepted mask** — `drm/powervr_sgx/pvrsgx_fw.c`, `UKFW_FEATURES`
  (`CLEANUP_V2 | SECURE` = `0x3`; m2-drm adds `DONE_SEQ` → `0x7`). On load,
  `ukfw_parse` rejects any host-visible bit outside this mask.
- **Rule:** the shipped image's host-visible features must be a subset of the
  shipped kernel's mask. CI enforces it with `drm/tools/ukfw_features_pin.py`
  (workflow `ukfw-features-pin`, and the firmware step of `mesa-debian` and
  `cedarview-kit`). Flipping `UK_FEATURES` to 6 while the in-tree driver mask is
  still `0x3` fails the pin — by design.

## Which kernels accept DONE_SEQ

- A kernel accepts a features-6 image **iff** its `UKFW_FEATURES` includes
  `PVRSGX_UKFW_DONE_SEQ` *and* it reads the seqno at ctlblk `+0x9c`. The reader
  landed in m2-drm `a3b8c8dc`; the mask bump is in `24a2253`. HW-confirmed on the
  CE5300 board: `powervr_sgx … open microkernel ukernel-m8, interface 1.0, features 0x6` /
  `microkernel started`.
- The Linux base stays 7.3.3 across the change. "Kernel version" here means the
  **powervr_sgx driver revision** (the deb's `-sgx` package release), not the
  upstream kernel version — the Linux version alone does not tell you whether a
  kernel reads DONE_SEQ.
- Any earlier driver (mask `0x3`) rejects a features-6 image — see Fallback.

## What deb and kit need first

The `linux-image-*-sgx` deb (`powervr-sgx-deb.yml`) and the kit kernel
(`cedarview-kit.yml`, from m2-drm's `powervr-sgx-cedarview` artifact) are built
from the `drm/` tree. Before the shipped image turns DONE_SEQ on:

1. The release ref's in-tree `drm/powervr_sgx/pvrsgx_fw.{h,c}` carries
   `PVRSGX_UKFW_DONE_SEQ` and the seqno reader (synced from m2-drm). The pin and
   the release deb both build from this copy, so the mask here *is* the shipped
   kernel's mask.
2. The `sgx545-firmware` deb (built in `mesa-debian.yml`) gains a versioned
   relationship on the kernel package — `Depends`/`Breaks` on `linux-image-*-sgx`
   ≥ the DONE_SEQ release — so apt never pairs a features-6 firmware with a
   pre-DONE_SEQ kernel already installed. (The two debs install independently; the
   in-image pin does not cover a user who updates only one.)
3. Flip `ukernel/build.py` `UK_FEATURES` default `2` → `6`. That is the **only**
   firmware-source edit — the pin and the two firmware build steps follow it
   automatically; no hardcoded `0x2` to chase.
4. Re-spin the `upstream/linux-firmware` blob at features 6, but only once a
   DONE_SEQ kernel is in a released / `-next` kernel (see the linux-firmware
   dry-run notes).

Not needed: **the golden hash.** Features live in the mkukfw header, not in the
assembled image, so the flip does not move `ukernel/golden/sgx545.sha256`.

## Fallback: an old kernel must fail clearly, not hang

`ukfw_parse` (`pvrsgx_fw.c`) rejects any header feature bit outside
`UKFW_FEATURES | UKFW_FEATURES_PRIVATE` with `-EINVAL` and
`drm_err("…: interface or features not supported")`. DONE_SEQ is a host-visible
bit (0-15), deliberately **not** private, so an old kernel (mask `0x3`) sees
`BIT(2)` as unknown and refuses the image at probe: the module does not bind, no
GPU comes up, and there is a clear dmesg line — no silent poll fallback, no hang.

- Regression guard: the KUnit `pvrsgx_fw_test` already asserts an unknown feature
  bit → `-EINVAL`.
- The reverse — a features-2 image on a DONE_SEQ kernel — runs via the 100 ms
  backstop poll and is HW-proven; shipping features 2 to new kernels is safe.

## Checklist (the flip, one change)

- [ ] in-tree `drm/powervr_sgx` synced to carry DONE_SEQ (mask `0x7`) — the pin and
      the deb/kit kernels build from it
- [ ] `sgx545-firmware` deb: versioned `Depends`/`Breaks` on `linux-image-*-sgx` ≥
      the DONE_SEQ release
- [ ] `ukernel/build.py` `UK_FEATURES` default `2` → `6`
- [ ] CI green: `ukfw-features-pin` (6 ⊆ `0x7`), and the `mesa-debian` /
      `cedarview-kit` firmware steps
- [ ] `upstream/linux-firmware` blob re-spun at features 6 (after a DONE_SEQ kernel
      ships or lands in `-next`)
- [ ] golden unchanged — confirm `ukernel-golden` still green (image bytes do not move)
