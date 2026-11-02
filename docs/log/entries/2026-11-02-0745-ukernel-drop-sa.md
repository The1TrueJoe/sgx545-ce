# Microkernel dropped secondary attributes (the init-stall fix, offline)

Fixes the EA3 init stall (previous entry) offline. The microkernel no longer
uses secondary attributes at all: the event entry loads the loader-patched
control-block base into a reserved temp `r17`, and every handler derives the
host-control / CCB / kernel-CCB pointers from `*(r17+n)` instead of reading them
through `sa`. The 4 `mov sa` self-writes (which stalled in the bare-`DOUTU`
context) and all `ld`/`st` through `sa` are gone; `sa{SA_SELF}` became `r17`,
and `ld_sa`/`st_sa` derive from `r17`.

Why this and not the DDK's emit/DMA (option A): it is self-contained in the
microkernel, needs no srvinit change, and does not depend on the `sa`-setup
context at all. `run_ours` (fake/v1/v2/xfer/xfer-rect) and `test_emu` all still
match the DDK — the core programming is unchanged, only the pointer plumbing.

Open hardware question: the base now lives in `r17`, so the bare-`DOUTU` event
context must allocate at least that many temps (the microkernel also uses r16
and r23). A one-word sentinel on the next EA3 slot confirms it; if the context
is short on temps, bump the register count in `srvinit`'s `pds_ours` DOUTU.

Note on the emulator guard: the earlier plan was to make the emulator refuse
`mov sa`. Dropped — the DDK microkernel itself writes `sa` (`mov sa9`) and runs,
so a blanket refusal is wrong and breaks the DDK A/B reference. The real limit
is narrower (no `sa` write in the bare-`DOUTU` context before the emit/DMA),
which the emulator does not model; the microkernel simply avoids `sa` now.
