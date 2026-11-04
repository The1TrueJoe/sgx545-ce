# Runtime PM on the EA1: the set that passed on the EA3 passes here too, on by default

Follows 0610 (the EA1 froze on the first runtime-PM run, on 2fec9ff) and 0630 (the
cause, a register read of the gated core on the first resume, fixed by 1d170e9). EA1
RAM boot of 1d170e9 (ea1-v1), 2026-11-03 ~17:00 UTC, DDK microkernel, nothing flashed,
no power-cycle.

| run, `runtime_pm=1` | result |
|---|---|
| load, then idle | suspended after 500 ms: power status 0x2c, `powered: 0` |
| the first resume, one NULL job | signalled in 19.5 ms; suspended again within 1.5 s |
| 20 frames, each from suspended | 20/20 right |
| uAPI suite; blits, 3 rounds; 20 frames on 2 contexts | 33/33; all right; 20/20 right |
| `powervr_sgx_hang -n 3`: render, mixed submit, idle, forced hang | all right; render from suspended 13-17 ms; 3 resets, 3 devcoredumps, back to suspended afterwards |
| `rmmod` while suspended, reload, frames, `rmmod` | clean |

The SGX's interrupt (26) is not shared on the EA1 either, so 0610's shared-IRQ suspect
did not apply on either box. The freeze in 0610 was the first resume.

`runtime_pm` is now on by default; `runtime_pm=0` turns it off.

Data: `docs/log/data/2026-11-03-1700-drm-runtime-pm-ea1.txt`.
