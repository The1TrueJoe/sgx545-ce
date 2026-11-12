# SGX530 vendor-launch gate 1: the output retires, the temporary doesn't; word 2 is inert

**Board:** our OMAP3630 test board, up 12 h 48 min (the same boot as attempts 40-47, T1, T2
and §9), the owner-reread pinned ED25519 host key, the board lock held 21:31:20-21:32:35 UTC.
75-77 °C. No hang, no plug cycle.

**What ran:** powervr_sgx from claude/sgx530-gap19-tests (CI: omap3630 43309942028, green; it
adds the `vl_w0/w1/w2/a70` §9 bisect params). Three loads with `allow_unisolated=1
stage1_vlaunch=1`. Images: `sgx530_aluwr_o0` (writes output o0 and loads through it) and
`sgx530_aluwr` (writes temporary r5 and loads through it), both matching the microkernel
session's hashes. EDMPROTECT on, as §9.

Every load: `stage1_vlaunch` confirmed (`0xa6c` = 3, `0xa70` = 0x00013d01), the task launches
and runs (no code-base-0 fault), then faults with `BIF_INT_STAT` 0x4002 (a load):

| Load | word 2 | `BIF_FAULT` |
|---|---|---|
| `aluwr_o0` (output) | 1 (§9) | **0x0f373000** |
| `aluwr` (temporary) | 0 (`vl_w2=0`) | **0x0fa93000** |
| `aluwr_o0` (output) | 0 (`vl_w2=0`) | **0x0f373000** |

With §9's own word 2 = 1 (the previous run, log 2026-11-12-2115): `aluwr` 0x0fa93000,
`aluwr_pa0w` 0x0fa93000.

**Facts (C):**
- **The output's retired OR under §9 is 0x0f373000.** `aluwr_o0` writes o0 and reads it back;
  outputs retire (stage 1, C), so this is the true `r5 | r17` under §9's register seed.
- **The temporary and the primary attribute show 0x0fa93000, not 0x0f373000.** `aluwr`
  (temporary) and `aluwr_pa0w` (pa0) both fault at 0x0fa93000, which differs from the output's
  0x0f373000.
- **Word 2 (1 vs 0) changes neither.** `aluwr` stays 0x0fa93000 and `aluwr_o0` stays
  0x0f373000 with `vl_w2=0`.

**What it suggests (G):**
- **§9 does not make a temporary or pa write retire.** By the microkernel session's test — if
  the output's value differs from the temporary's, the temporary didn't retire — 0x0fa93000 is
  a coincident leftover (r5's and pa0's §9-seeded leftovers happen to match), not a retired OR.
  Only the **output** retires, as in stage 1.
- **§9's gain stands at gate 2:** the task launches and runs under the vendor's 8-word
  interleave and launch words, repeatably. That is real and is what the open microkernel's
  launch words could not do. But the write-retire blocker for temporaries and pa is not solved
  by §9.
- **Word 2 is not the write-context lever.** The remaining §9 candidates are word 1
  (0x04000000) and word-0 bit 21; the microkernel session directs the next bisect step. If
  none grants a temporary write context, the output-only retire means the attribute/output
  restructure (outputs as scratch) remains the path, with §9 fixing the launch.

**Next:** the microkernel session's decode and the next bisect word (word 1, then word-0 bit
21), on the next slot.
