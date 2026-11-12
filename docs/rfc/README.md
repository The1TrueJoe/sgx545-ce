# RFC draft: powervr_sgx

A draft for Joseph to read. **Not sent, and not to be sent from here.** Joseph
signs and sends it himself (`b4 send` or `git send-email`).

- `0000-cover-letter.txt`: the cover letter, with To and Cc.
- `series/`: the per-patch commit messages `drm/ci/rfc-series.sh` uses. The
  patch list and diffstat in the letter come from `git format-patch` when the
  series is cut.
- Rewritten 2026-11-12 from the reviewer research
  (`sgx545-work/upstream/rfc-research-2026-11-12.md`) and the Kernel items
  K10-K18 in `docs/UPSTREAM-READINESS.md`. The letter describes the driver as
  it will be when it is sent, so some of its sentences depend on work still
  open:

| sentence | needs |
|---|---|
| "Each client has its own parameter buffer" | K11, option A (open-ukernel `wip-pb-switch` and the per-VM PB) |
| "Completion comes from the microkernel's interrupt" | K7 (the DONE_SEQ firmware required) |
| "sends a wedged uevent" | K5 |
| "the uAPI, frame, hang, PRIME and unbind tests, also under lockdep, KASAN, ..." | K8 clean |
| "CI builds every patch" | K9 |
| "transfer jobs" listed as a follow-up | K12 (transfer left out of the series) |
| the bracketed placeholders | Joseph: the AI disclosure. Links still to fill: the draft Mesa MR and the IGT branch. [1] and [2] are lore links built from Message-IDs read on mirrors; open each once in a browser before sending |


## Standards checklist (2026-11-08)

Checked against the 7.1.8 tree's `Documentation/` and a trial patch: the
driver laid out as upstream would carry it (`drivers/gpu/drm/powervr_sgx`,
`include/uapi/drm/powervr_sgx_drm.h`, `Documentation/gpu/powervr_sgx`,
MAINTAINERS, Kconfig), committed as one patch and run through
`checkpatch.pl --strict` and `get_maintainer.pl`.

### submitting-patches.rst

| item | status |
|---|---|
| Subject `[RFC PATCH 0/N] drm/powervr_sgx: ...`, imperative patch subjects with the subsystem prefix; N and the patch list from the cut series | open until the cut |
| Cover letter says what, why and what is asked; RFC status explicit | pass |
| Plain text, wrapped under 75 columns, no attachments | pass |
| One logical change per patch, each one building | **open**: the series isn't cut yet |
| `Signed-off-by` | **left for Joseph**: checkpatch reports it missing until he signs |
| `base-commit:` (`git format-patch --base`) | at send time |
| To/Cc from `get_maintainer.pl` | pass, with the edits below |

### coding-assistants.rst

| item | status |
|---|---|
| No `Signed-off-by` from the AI | pass |
| No per-patch AI tags. The kernel dropped `Assisted-by:` in v7.4-rc1 (1e035297ffda); disclosure belongs in the cover letter | pass |
| No `Assisted-by:` anywhere: not on kernel patches, not in Mesa, and not on the linux-firmware commit, although linux-firmware's README asks for one there | **Joseph's decision (2026-11-12)**. The disclosure is his paragraph in the cover letter |
| AI-assistance disclosure in the cover letter (generated-content.rst) | **left for Joseph**: a placeholder in the letter, his wording |

### drm-uapi.rst (new uAPI)

| item | status |
|---|---|
| Open-source userspace exists, real driver not a test (Gallium + NIR compiler, dEQP-GLES2 16,587/17,165) | pass |
| Userspace reviewed and ready in canonical upstream Mesa | **fail**: in our fork; why this is an RFC, not a merge request |
| Userspace reviewer's Acked-by on the uAPI patch | **fail**: needs the Mesa review first |
| uAPI documented (kernel-doc, `Documentation/gpu/powervr_sgx/uapi.rst`) | pass |
| Render node only, `DRM_RENDER_ALLOW` on every ioctl | pass |
| Device reset reporting ("Device reset" section: guilty/innocent, refused contexts) | pass |
| One layout on 32- and 64-bit (i386/x86_64 check in CI) | pass |
| IGT tests for the new uAPI | **open**: ours are standalone C in IGT style; port to IGT before merge |

### checkpatch

| item | status |
|---|---|
| `--strict -f` on every driver file, the tests and the uAPI header | clean |
| Patch mode on the trial patch | clean except: the missing `Signed-off-by`; 12 CHECKs in `pvrsgx_trace.h` (the `TRACE_EVENT()` macro layout every driver's trace header has); two Kconfig help texts under 4 lines, since fixed |
| The cover letter | not a patch; checkpatch has nothing to say on it |

### To and Cc

`get_maintainer.pl` on the trial patch, against the 7.1.8 MAINTAINERS:

- Keep: the drm-misc maintainers (Maarten Lankhorst, Maxime Ripard, Thomas
  Zimmermann), David Airlie, Simona Vetter, dri-devel, linux-kernel.
- Added by hand: the drm/imagination maintainers (IMGTEC POWERVR DRM DRIVER
  since v7.3), Alessio Belle and Luigi Santivetti, plus
  imagination@lists.freedesktop.org. Frank Binns is no longer listed, not even
  as a reviewer; he stays on Cc as a courtesy because he wrote Imagination's
  2023 statement that SGX should have its own driver. Patrik Jakobsson for the
  gma500 patches.
- The DRM GPU SCHEDULER maintainers (Matthew Brost, Danilo Krummrich, Philipp
  Stanner; Christian König as reviewer) and Thomas Hellström (DRM GPUVM
  reviewer, and the x86 write-combining question). get_maintainer decides the
  addresses on the day: König is `R:` for the scheduler under a different
  address than his dma-buf one.
- Matched by keyword, to decide at send time: dma-buf (Sumit Semwal,
  Christian König, linux-media, linaro-mm-sig: the driver imports dma-bufs) and
  Documentation (Jonathan Corbet, linux-doc).
- **Wrong match, leave out:** Jarkko Sakkinen, Dave Hansen and
  linux-sgx@vger.kernel.org. "sgx" in our file names matches Intel Software
  Guard Extensions' keyword.
- Worth adding: the author of the SGX535 RFC (below), and Andrew Davis and
  H. Nikolaus Schaller, who did the Series5 SGX device-tree binding
  (`img,powervr-sgx.yaml`) and the out-of-tree SGX kernel ports.

## What's on dri-devel now, and what it means for ours

Read through the public mirror at ratatoskr.run (lore and the freedesktop.org
archive refuse automated reads; not bypassed).

- **"[RFC] drm/gma500: SGX535 render ownership, MMU and failure handling"**
  (2026-11-01 to 11-08). The reMESA author (SGX535-reMESA, Poulsbo) asks
  whether SGX535 rendering belongs in gma500 or in a separate driver. Patrik
  Jakobsson: keep it in gma500 if only gma500 hardware is targeted; a separate
  driver "may be better" if other devices will be supported. He also asked for
  short mails, saying he assumed much of the text was AI-written. The author
  settled on gma500 and plans a small patch, then asked about the SGX544
  (Clover Trail+), leaning towards a separate driver. That question had no
  answer when last read.
  - Ours is the separate, multi-device driver Patrik described. The Poulsbo
    SGX535 fits it as a core-table entry behind the same gma500 auxiliary
    device as Cedarview. The cover letter offers that, and asks rather than
    competes (question 2).
  - Timing: if the SGX535 render code lands inside gma500 first, there will be
    two SGX render stacks. Writing to the reMESA author before posting may be
    the friendlier first step. That's Joseph's call.
  - Tone: the cover letter is kept short. Its AI disclosure is Joseph's
    paragraph (a placeholder until then); there are no per-patch tags.
- **drm/imagination** is busy: async VM_BIND with sparse mappings
  (2026-11-08), GPUVM immediate mode (October), runtime-PM fixes, and new
  maintainers since June (Alessio Belle, Luigi Santivetti; Matt Coster left).
  The Rogue driver shares nothing with ours. Imagination does maintain the
  Series5 SGX binding, so placement next to it is a fair question for them
  (question 1). Their async VM_BIND makes ours look odd only if ours takes
  fences: a synchronous VM_BIND takes none (drm-vm-bind-async.rst; K14), and
  an async mode is a later flag, as theirs was.
- **Series5 SGX device-tree work** (Andrew Davis, 2023-24; binding comments
  from H. Nikolaus Schaller, Tony Lindgren and Krzysztof Kozlowski, some
  still active in October 2026) added `img,powervr-sgx.yaml` for TI and
  other ARM SoCs. A family driver would eventually bind those nodes, so they
  should be on Cc.
- **How other new drivers framed their RFCs.** Tyr (Rust Mali, 2025) was one
  short block: what it is, what doesn't work yet, where it was tested, and an
  IGT branch to test with. Review went straight to concrete code points.
  Ours follows that shape. Adding links to the Mesa branch and an IGT port
  would match it fully.

## Before it could be sent

1. Cut the series (`docs/UPSTREAM-READINESS.md`, Kernel, "The series"),
   without DDK compat. Before that, give the follow-ups' call sites in v1
   code stubs, so CI can build v1 alone; it builds only the whole driver
   today.
2. Mesa merge request open, and the uAPI questions above settled.
3. Port the uAPI tests to IGT.
4. Recheck To/Cc against MAINTAINERS and `get_maintainer.pl` on the day
   (the drm/imagination maintainers changed twice in 2026), then Joseph
   signs off and sends.

## Decisions (2026-11-12, Joseph: "follow convention")

- **Gama's gma500 3D RFC:** not written to first. Question 2 names it in one
  neutral line, and gma500 0001 stays as it is.
- **MAINTAINERS:** `M:` Joseph alone, no `R:` line. The cover letter offers a
  reviewer slot in one sentence.
- **Driver license:** the drm/imagination convention, `GPL-2.0-only OR MIT` on
  the driver files (`MODULE_LICENSE("Dual MIT/GPL")`). The uAPI header keeps
  `(GPL-2.0-only WITH Linux-syscall-note) OR MIT`, as `pvr_drm.h` does, and
  the files shared with Mesa stay MIT. To do after the per-core restructure,
  since it touches every file. The register header is derived from the GPL
  `sgx545defs.h`; it stays `GPL-2.0-only` unless Joseph decides otherwise.
- **Firmware license:** linux-firmware's LICENSE-CRITERIA, section 3.1 item 3:
  a license must "include a patent grant adequate for device operation", and
  "if your standard license text is silent on patents, expect this to be
  raised in review"; "an explicit grant avoids the question". MIT is silent
  on patents, so the microkernel becomes `MIT OR Apache-2.0` (Apache-2.0
  carries the explicit grant). Section 2.2 names both as welcome.
- **Firmware name:** `powervr_sgx/sgx545_v1.fw`, after drm/imagination's
  `powervr/rogue_<bvnc>_v1.fw`. One name everywhere: the core table,
  `MODULE_FIRMWARE`, the packages, the WHENCE draft and the docs. It ships
  with the features-6 move in v0.2.0.
