# gma500 patches for the Cedarview SGX

The GMA3600/3650 GPU of the Atom D2000/N2000 (Cedarview) is a PowerVR SGX545
behind the display engine's PCI function and register BAR, which mainline
`gma500` drives. These patches against mainline publish the SGX as an
auxiliary device, `gma500.sgx`, for powervr_sgx (`CONFIG_DRM_POWERVR_SGX_GMA500`).

- `0001`: the auxiliary device, the interrupt hand-over, and gma500 keeping its
  hands off the SGX's registers while it has another owner.

They are written to go to the gma500 maintainers as they are. Before sending,
the author adds a `Signed-off-by:`, and an `Assisted-by:` trailer for the tooling.

Untested on hardware: nobody here has a Cedarview machine. CI builds them
(the `cedarview` job), and the tester kit runs them.
