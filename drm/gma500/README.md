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
- `0004`: a pipe that doesn't start after a DPMS on gets enabled once more.
- `0005`: memory self-refresh is left off with a single pipe. With it on, the
  LVDS panel goes black (backlight on, pipe B's FIFO underruns) on an Atom
  N2600 and a D2500 laptop.

They are written to go to the gma500 maintainers as they are. Before sending,
the author adds a `Signed-off-by:`.

On hardware: `0001` is proven on an Atom N2600 (the auxiliary device appears,
powervr_sgx binds and identifies the core). `0002` hung its first importer (X's front buffer, canteroman's kit v3.4): the PRIME helpers call pin with the reservation lock held and pin took it again. Its importers also couldn't mmap a dumb buffer, which has no fake offset until MAP_DUMB, so export creates one. Both fixed, not yet run on hardware. CI builds both
(the `cedarview` and Debian package jobs).
