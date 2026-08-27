# tools

## sgxregs.sh
Reads the SGX identity registers on the board. Run this before anything else —
it answers "is the GPU there, clocked, and where we think it is" in a minute.
`test-sgxregs.sh` self-checks its arithmetic against a mocked sysfs/devmem.

## sgxinit.c
Brings up PVR services against the kernel driver: the userspace half of the
bring-up, and the thing that first exercises `open()` on the DRM node.

It exists because neither available userspace can be used as-is:

* Intel's Cedarview package ships **no** `pvrsrvctl` — libraries only.
* Control4's `pvrsrvctl` is DDK 1.12 and links 1.12's `libsrv_um`/`libsrv_init`,
  so it cannot drive a 1.7 userspace.

But all `pvrsrvctl --start` really does is one call. Disassembling Control4's
binary shows `SrvInit` takes no arguments and returns a `PVRSRV_ERROR` in `eax`
(nothing is pushed before the call, and a non-zero return goes straight to
`PVRSRVGetErrorString`). So this is that call, and nothing else. It `dlopen`s
rather than links, so an unresolved symbol prints instead of the loader
refusing to start the process.

### Running it

The DDK blobs are 32-bit glibc; openHC is musl. Rather than fight that during
bring-up, run them against a private glibc sysroot:

```sh
# on a build host: assemble lib/ from Debian i386 libc6, libdrm2, libgcc-s1
# plus the DDK's libsrv_um.so and libsrv_init.so, then
i686-linux-gnu-gcc -O0 -g -o sgxinit tools/sgxinit.c -ldl

# on the board
./lib/ld-linux.so.2 --library-path /opt/pvrtest/lib ./bin/sgxinit
```

Pair the blobs with a driver built from the **same DDK version** — the version
check is exact-match. Cedarview's 1.7 blobs match this tree; Control4's 1.12
blobs will be rejected.

Watch `dmesg` alongside it. The first run of this found the driver's missing
`FOP_UNSIGNED_OFFSET`, which no amount of building would have surfaced.
