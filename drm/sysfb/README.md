# sysfb patch for the CE5300 test kernels

`0001` copies the damage of a framebuffer the GPU is still rendering into
outside the DIRTYFB ioctl, so X's main thread doesn't wait for the render.
Without it, X on simpledrm next to a busy SGX545 answers a pointer query in
seconds. `drm/ci/ce5300-kernel.sh` applies it to the CE5300 test kernels only,
which keeps their desktop representative for latency work. No other kernel
carries it: not the Cedarview packages, not the upstream series.

**Upstream declined it** (RFC v1, 2026-11-11): DIRTYFB is meant to block,
since it is the compositor's feedback path, and the fix belongs in X. It
won't be sent again; v2 was never sent.
