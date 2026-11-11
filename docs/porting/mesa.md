# Adding an SGX core to Mesa

The Mesa side of a new core is one entry in `mesa/src/sgx/common/sgx_core.c`,
plus whatever the core's generation needs from the compiler. The kernel side
(powervr_sgx's core table and firmware) comes first; see the kernel driver's
porting notes. Mesa finds a core by the kernel's core id and revision, so a
core without an entry is refused at screen creation ("has no description").

## 1. The entry

Copy the SGX545 entry and change what differs. Say how you know each value in
the field's comment: **[HW]** measured on the core with the open stack, **[D]**
taken from the vendor driver's behaviour, **[G]** a working guess.

| field | how to establish it |
|---|---|
| `name`, `id` | the kernel's `DEV_QUERY` core id, bits 16-31 (`dmesg` prints it) |
| `rev_min`, `rev_max` | the revisions you tested; widen only with a test on each |
| `usse` | `SGX_USSE1` for SGX520-545, `SGX_USSE2` for 543/544/554 |
| `verified_on` | the machines your dEQP run and desktop session used |
| `max_texture_size`, `max_cube_levels`, `samplers` | the vendor driver's `glGetIntegerv`; then piglit `max-texture-size` |
| `vs_inputs`, `vs_outputs`, `fs_inputs`, `coord_sets` | the vendor driver's limits; dEQP `functional.shaders.*` at the limit |
| `gl_max_temps`, `temps` | GL's limit, and the USSE register file the compiler allocates |
| `rt_max_*` | only for kernels that don't report them |
| `line_width_max`, `point_size_max` | the vendor driver's ranges |
| `vertex_task_dwords`, `vertex_task_max` | a capture of the vendor driver's VDM words at several output sizes |
| `dma_chunk_dwords` | the largest PDS DMA that copies correctly (sgx_frame_test) |
| `f16`, `f16_sample_return` | F16 maths and F16 texel return; leave false until a dEQP run with them is clean |
| `sample_texture_heap_only`, `npot_mip_hangs` | true until shown safe: these steer around hangs |

Everything else that differs between cores goes into `struct sgx_core` as a
new field read in one place, never as a test of the core's id.

## 2. The compiler

- **USSE1 cores** (SGX530, 531, 535, 540): the same instruction families as
  the SGX545. Expect differences in register-file sizes and in which
  instructions exist (F16/C10 arithmetic, efo co-issue, fdual, PDS samples,
  constant memory). They become fields the compiler reads through
  `sgx_compile_options`. The instruction encodings stay in the shared ISA
  description (`mesa/src/sgx/isa`), so find out first whether your core
  encodes any instruction differently.
- **USSE2 cores** (543, 544, 554) have a different instruction set: a
  separate ISA description, and selection and scheduling of their own in the
  compiler. That is a larger port: start with the ISA description and its
  assembler/disassembler tests.
- **State words** (`mesa/src/sgx/csbgen`) and **PDS programs**
  (`mesa/src/sgx/pds`) are per family too. Compare a vendor capture of a
  simple frame against `docs/STATE.md` before trusting them.

## 3. Evidence for an entry

A new entry is merged with:

1. `glxinfo -B` (or `eglinfo`) showing `GL_RENDERER: PowerVR <name>`;
2. a dEQP-GLES2 mustpass run on the core, with the results file attached:
   no GPU recoveries, and the failures listed;
3. a desktop session (X with glamor, a GL client, a window moved) with a
   screenshot;
4. the machines in `verified_on`, and the kernel and firmware versions used.
