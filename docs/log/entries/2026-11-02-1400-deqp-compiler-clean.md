# dEQP-GLES2: no compiler-side failures left

On the morning of 2026-11-02 the first full dEQP-GLES2 run of the open driver on the EA3 passed
15,440 of 17,165 tests. By the afternoon it passed 16,333, with no failures left on the
compiler's side; the remaining 367 belong to the driver (fragment operations, cube maps, FBOs,
polygon offset, wide lines). Each fix was found on the box and checked offline with `sgxc` over
every corpus and test set (now about 7,700 programs), which must stay byte-identical except where
the change is meant to apply.

What was wrong, roughly in the order the runs exposed it:

- **Loops.** `res = res.yzwx` in a loop that doesn't unroll wrote the first register before the
  last component read it. Copies whose later components read registers written earlier now go
  through a temporary.
- **Source modifiers.** A negate or abs that NIR folded into a 32-bit view of F16 halves was
  dropped, so `x + y >= 0` tested `x - y`.
- **Repeats over F16 halves.** A scalar op (`frcp`, `fexp2`, ...) steps its F16 source by
  register under a repeat, with the half fixed. Runs over consecutive halves read every other
  component. The DDK never repeats them either.
- **Builtins from the frontend.** atan's copysign arrived as integer bit masks, and mediump
  sin/cos built 32-bit constants against a 16-bit argument. Both are now rewritten in float form.
- **F16 uniforms.** A whole fmad16 pair named its SA register by constant `2m`, which has no
  register when only `2m + 1` is read (`sa8`, below the SA base). F16 constants past `sa31`
  couldn't be encoded under format select and are now copied first.
- **Values that never reached their register.** An `f2f32` view and a texture's unpack both
  left their result in a place of their own while NIR said the register had been written.
  Colours went out as raw F16 bits or stale data.
- **F16 constants converted twice.** `f32_to_h` read an F16 constant's bits as an f32.

Two methods did most of the work. First, `sgxc -L -D -F` compiles what the driver compiles; it
needed uniform offsets folded the way st folds them before it matched. Second, where offline
reading found nothing, the driver's own NIR and code dumps from the box (`SGX545_DEBUG=nir,draw`,
`SGX545_DUMP_CODE`) pointed straight at the fault. Several guesses made without them (an abs bit
on F16 operands, a two-source `pck.f16.f32`) were wrong and were reverted.

`tools/usc/tests/loops`, `ops`, `rnd` and `deqp` hold the shapes. `SGX_F16_ITER=0` and
`SGX_F16_CONST=0` switch off one part of the F16 path each, for bisecting on hardware.
