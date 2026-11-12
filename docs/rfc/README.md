# RFC draft: powervr_sgx

A draft for Joseph to read. **Not sent, and not to be sent from here.** Joseph
signs and sends it himself (`b4 send` or `git send-email`).

- `0000-cover-letter.txt`: the cover letter, with To and Cc.
- The patches don't exist yet as a series. The cover letter lists the v1
  split (8 patches, the core only) and the follow-ups to come;
  `docs/UPSTREAM-READINESS.md` (Kernel, "Minimal first series") has the plan.
- Refreshed 2026-11-09 for m2-drm since a8dff09: the shared parameter
  buffer (and question 4 on it), hang blame, the Cedarview results, and
  testing on drm-misc-next. The runtime-PM stopgap (a render context
  keeps the GPU awake) is in F3, not v1, so the letter doesn't mention it.

## Standards checklist (2026-11-08)

Checked against the 7.1.8 tree's `Documentation/` and a trial patch: the
driver laid out as upstream would carry it (`drivers/gpu/drm/powervr_sgx`,
`include/uapi/drm/powervr_sgx_drm.h`, `Documentation/gpu/powervr_sgx`,
MAINTAINERS, Kconfig), committed as one patch and run through
`checkpatch.pl --strict` and `get_maintainer.pl`.

### submitting-patches.rst

| item | status |
|---|---|
| Subject `[RFC PATCH 0/8] drm/powervr_sgx: ...` (v1 core only; the follow-ups listed as to follow), imperative patch subjects with the subsystem prefix | pass |
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
| No AI attribution trailers on any patch (Joseph's rule) | pass |
| AI-assistance disclosure in the cover letter | **left for Joseph**: his wording, if any |

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
- Added by hand: the drm/imagination maintainers **as of mainline after
  June 2026**, Alessio Belle and Luigi Santivetti, plus
  imagination@lists.freedesktop.org (the 7.1.8 tree still lists Frank Binns and
  Matt Coster). Frank Binns, a reviewer, stays on Cc. Patrik Jakobsson for the
  gma500 patches.
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
  - Tone: the cover letter is kept short, and it says up front that an AI
    assistant was used, with the tags on each patch.
- **drm/imagination** is busy: async VM_BIND with sparse mappings
  (2026-11-08), GPUVM immediate mode (October), runtime-PM fixes, and new
  maintainers since June (Alessio Belle, Luigi Santivetti; Matt Coster left).
  The Rogue driver shares nothing with ours. Imagination does maintain the
  Series5 SGX binding, so placement next to it is a fair question for them
  (question 1). Our synchronous-only VM_BIND will be compared with their
  async one (question 3).
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
