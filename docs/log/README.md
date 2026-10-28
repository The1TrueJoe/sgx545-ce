# docs/log

The build log of the open SGX545 userspace. [BUILDLOG.md](BUILDLOG.md) is the story;
the rest is its evidence.

- `entries/YYYY-MM-DD-HHMM-<slug>.md`: one file per milestone (UTC): what was tried, what
  worked and what didn't, the numbers, the commit. One file per entry, so work in separate
  clones never conflicts.
- `img/<stem>[-n].png`: frames read back from the EA3, before/after pairs, plots.
- `data/<stem>.csv|json`: the measurements behind an entry.
- `fig.py`: turns frames into PNGs and grids. A frame is a `glReadPixels` capture
  (`.rgba`, 720×480 RGBA, bottom row first) or a back-buffer dump (BGRA, top row first,
  pitch rounded up to 32 pixels).

```
python3 fig.py png  cap/ref.rgba img/ref.png
python3 fig.py grid img/pair.png 2 cap/ref.rgba rp/bb.bin --scale 2
```

The technical write-ups the log draws on:

| Document | Covers |
|---|---|
| `docs/NOTES.md` | tracer, captures, heaps, kicks, the replay procedure |
| `docs/STATE.md` | TA/3D render state: VDM, ISP/TSP words, PBE, textures |
| `docs/PDS.md` | the PDS instruction set and DOUT words |
| `docs/ISA.md` | the USSE instruction set, hardware constants, USP container |
| `docs/SRVINIT.md` | microkernel bring-up without libsrv_init |
| `docs/TEX.md` | texture memory layout, uploads, render to texture, ZLS, presentation |
