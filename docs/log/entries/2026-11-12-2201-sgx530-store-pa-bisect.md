# SGX530 §9: stores drop, pa doesn't retire, and the minimal retire launch is word2 + word0 bit21

**Board:** our OMAP3630 test board, up 13 h 17 min (the same boot as the whole §9 series), the
owner-reread pinned ED25519 host key, the board lock held 22:01:40-22:03:19 UTC. 75-77 °C. No
hang, no plug cycle.

**What ran:** powervr_sgx from claude/sgx530-gap19-tests (omap3630 CI 43309942028, green), five
loads, `allow_unisolated=1 stage1_vlaunch=1`. Images from sgx530 5d010a9c, hashes verified
against the microkernel session's list. `ctlblk` = TA3DCTL = 0x0f003000; `ctlblk + 0x200` =
0x0f003200 = data-arena offset 0x3200, within the 0xe000 arena, past every control block
(HOSTCTL ends 0x3143, MISC ends 0x31b3), mapped EDMPROTECT under §9 and zeroed at `fw_start`.

| # | Image | param | `BIF_INT_STAT` | `BIF_FAULT` | scan |
|---|---|---|---|---|---|
| 1 | `sgx530_stor5` | `scan=0x0fa93000` | **0** | **none** | 0x0fa93000: 0 hits |
| 2 | `sgx530_xorpa0` | | 0x4002 | **0x0fa93000** | |
| 3 | `sgx530_xorr5` | `vl_w2=0` | 0x4002 | **0x0fa93000** | |
| 4 | `sgx530_xorr5` | `vl_w1=2` | 0x4002 | **0x00a90000** | |
| 5 | `sgx530_xorr5` | `vl_w0=0xf` | 0x4002 | **0x0fa93000** | |

**Facts (C):**
- **The store is silently dropped.** `stor5` stores r5 (0x0fa93000) to the EDMPROTECT page
  ctlblk+0x200 and does **not** fault (BIF 0), but the value is in neither arena (0 scan hits).
  Not rejected, not committed.
- **A pa write does not retire.** `xorpa0` (`xor pa0, r5, r17`) faults at 0x0fa93000 (pa0's
  leftover), not the XOR result 0x00a90000. Temps retire; pa does not.
- **The temp-retire launch needs word 2 = 1 and word-0 bit 21, not word 1.** With `xorr5` the
  decider (0x00a90000 = retires, 0x0fa93000 = doesn't): reverting word 2 to 0 **breaks** the
  retire; reverting word 1 to 2 keeps it; dropping word-0 bit 21 **breaks** it.

**Corrections:**
- **"Word 2 is inert" (log 2026-11-12-2131) is refuted.** That verdict was from `aluwr`, which
  is degenerate under the subset trap (r17 ⊆ r5). With `xorr5`, word 2 = 1 is necessary.

**What this means (G):**
- §9's launch makes **temporaries** retire (word 2 = 1 and word-0 bit 21 together), but **not**
  pa and **not** memory stores. The microkernel needs to persist control state across kicks —
  if that must go through `st` to its control block, §9 alone doesn't enable it; the store
  drops silently. Whether the store-drop is a cache/flush gap, a requestor-identity gap, or the
  microkernel can avoid `st` (keeping state in temporaries/outputs plus host-written
  structures) is the microkernel session's to decode.

**Next:** the microkernel session's decode of the silent store-drop and the pa-no-retire, and
whether the microkernel's state path can live within what retires (temps). Gate 2 (launch) and
the temp-retire (gate 1) stand.
