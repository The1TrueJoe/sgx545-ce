# docs

What the SGX545 does, as learned by tracing the DDK and proving each finding on an EA-3.
Each document tags its claims by evidence: encoder or DDK code, capture diffs, or hardware.

| Document | Covers |
|---|---|
| [CLEANROOM.md](CLEANROOM.md) | How spec and implementation are kept apart, and which roles may read what |
| [CLEANROOM-COVERAGE.md](CLEANROOM-COVERAGE.md) | Which doc section states each hardware fact the shipped code uses, and the facts the docs still need |
| [NOTES.md](NOTES.md) | Services bridge, heaps, the two kicks per frame, capture and replay |
| [STATE.md](STATE.md) | TA/3D render state: VDM stream, ISP/TSP words, PBE, textures, ZLS |
| [PDS.md](PDS.md) | The PDS instruction set and the DOUT words that start USSE tasks |
| [ISA.md](ISA.md) | The USSE shader instruction set, hardware constants, the USP container |
| [SRVINIT.md](SRVINIT.md) | Microkernel bring-up without `libsrv_init` |
| [TEX.md](TEX.md) | Texture memory layout, uploads, render to texture, depth/stencil (ZLS), presentation to fb0 |
| [GEN.md](GEN.md) | Generating a whole frame from code (`tools/gen`) |
| [UAPI.md](UAPI.md) | Draft of the `powervr_sgx` DRM driver's uAPI: ioctls, client commands, heaps, threat model ([header](uapi/powervr_sgx_drm.h), [commands](uapi/powervr_sgx_cmd.xml), [rev 3](uapi/rev3-commands.md)) |
| [porting/kernel.md](porting/kernel.md) | Bringing `powervr_sgx` up on a new SGX core or board: the stages, the tests that show each, the traps |
| [log/](log/BUILDLOG.md) | The build log: how all of the above was found, with frames and data |
