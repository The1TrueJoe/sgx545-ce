# docs

What the SGX545 does, as learned by tracing the DDK and proving each finding on an EA-3.
Each document tags its claims by evidence: encoder or DDK code, capture diffs, or hardware.

| Document | Covers |
|---|---|
| [NOTES.md](NOTES.md) | Services bridge, heaps, the two kicks per frame, capture and replay |
| [STATE.md](STATE.md) | TA/3D render state: VDM stream, ISP/TSP words, PBE, textures, ZLS |
| [PDS.md](PDS.md) | The PDS instruction set and the DOUT words that start USSE tasks |
| [ISA.md](ISA.md) | The USSE shader instruction set, hardware constants, the USP container |
| [SRVINIT.md](SRVINIT.md) | Microkernel bring-up without `libsrv_init` |
| [GEN.md](GEN.md) | Generating a whole frame from code (`tools/gen`) |
| [log/](log/BUILDLOG.md) | The build log: how all of the above was found, with frames and data |
