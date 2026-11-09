# PWRLOG: the top-6 power-up CRs are all fine at cold frame-1 — power-up register loss not supported (EA3)

ta3d-state's power-up lead: after srvinit ours goes idle -> ACTIVE_POWER -> the
core powers off; frame 1's kick powers it up via SGXReset's soft-reset-only branch,
which could leave CRs at hardware defaults instead of srvinit's init values. Built
UK_PWRLOG (gated, default binary byte-identical) to log the candidate CRs at do_ta's
FIRST kick (core powered, before do_ta reprograms them) to the T3C words.

## Values at cold frame-1 kick-1 (identical across all 5 lockups)

| T3C word | CR | value | read |
|----------|------|------------|------|
| 0x35 | 0x630 | 0x00000000 | clean (slot 0) |
| 0x37 | 0x080 | 0x00000000 | soft-reset, write-only |
| 0x38 | 0x040 | 0x00001E00 | DDK writes 0x1ff; ours neither |
| 0x39 | 0xe80 | 0x00000000 | DDK pulses; ours 0 |
| 0x3a | 0xa74 | **0x00208200** | srvinit's init value, intact |
| 0x3b | 0x274 | 0x00000000 | TA state |

## Conclusion

- **0xa74 is NOT lost at power-up** — it reads srvinit's 0x00208200 at the cold
  frame-1 kick. So either the core didn't power down, or the re-run init scripts
  restore it; either way it is correct for the terminate.
- **0x630 = 0** — the slot-select is clean. ta3d-state's secondary 0x630 candidate
  is dropped (its own "reg_read 0x630; if 0, skip" test).
- 0x080/0xe80/0x274 are 0 (benign / as expected). The only oddity is 0x040 =
  0x1E00 (the DDK writes 0x1ff; ours wrote 0x1ff once and it REGRESSED, so ours
  leaves it — 0x1E00 is its power-up/default value).

So the frame-1 terminate hang is NOT a lost value among these six config CRs. The
power-up-register-loss hypothesis is not supported (for this set). The hang is on
the TERMINATE kick (kick 2); the PWRLOG captured kick-1 entry. Next candidates to
log, at the terminate-kick entry (what kick 1 left for kick 2): the TA DPM page
counters 0x724/0x72c, the free-list state 0x618/0x61c/0x628, dir-list 0xc84/0xc38,
0x804, and CLKGATESTATUS 0x8 — a TA hang with valid commands is classically a
DPM/parameter-buffer or free-list problem, not a config register. Handed to
ta3d-state to pick the next set / reinterpret. Evidence:
`scratchpad/ea3-pwrlog.out`, `docs/log/data/2026-11-03-1500-ea3-pwrlog.log`.
