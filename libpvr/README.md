# libpvr

A small native client of the GPL PVR services kernel driver (`kernel/`, pvrsrvkm),
used by the tools that drive that kernel port directly (`tools/gen`, `tools/replay`,
`tools/iso`, `tools/tex`). It is built against `kernel/src`'s headers, and `abi/` is
generated from them (`ids.c` and the layout dumps), so libpvr is **GPL-2.0-only**
like those headers. Mesa does not use it.
