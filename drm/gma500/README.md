# gma500 patches for the Cedarview SGX

The GMA3600/3650 GPU of the Atom D2000/N2000 (Cedarview) is a PowerVR SGX545
behind the display engine's PCI function and register BAR, which mainline
`gma500` drives. These patches against mainline publish the SGX as an
auxiliary device, `gma500_gfx.sgx`, for powervr_sgx (`CONFIG_DRM_POWERVR_SGX_GMA500`).

- `0001`: the auxiliary device, the interrupt hand-over, and gma500 keeping its
  hands off the SGX's registers while it has another owner.
- `0002`: PRIME export of gma500's GEM objects, so Mesa (kmsro) can render into
  scanout buffers allocated on the display device. gma500 had no PRIME at all.
- `0003`: the graphics power island gets 10 ms to come up instead of 50 us, which
  stops a "GPU: power management timed out" at every Cedarview boot.

They are written to go to the gma500 maintainers as they are. Before sending,
the author adds a `Signed-off-by:`, and an `Assisted-by:` trailer for the tooling.

On hardware: `0001` is proven on an Atom N2600 (the auxiliary device appears,
powervr_sgx binds and identifies the core). `0002` is untested. CI builds both
(the `cedarview` and Debian package jobs).
