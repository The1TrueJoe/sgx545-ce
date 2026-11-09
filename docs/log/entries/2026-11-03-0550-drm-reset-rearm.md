# Contexts that live through a GPU reset render again

M2 item 4, following 0530. EA1 RAM boot of e0bc846, 2026-11-03 ~05:42 UTC, DDK
microkernel, nothing flashed, no power-cycle.

## The fix

In 0530, a context that was idle during a forced-hang reset hung on its next render, and
so did a new context in the same VM. The restart starts a fresh microkernel but left the
kernel-built structures as they were.

Now each HW render context, PB, RT dataset and HW transfer context keeps the device
reset count it was set up for. The first job that uses one after a reset sets it up
again:

| object | after a reset |
|---|---|
| HW render context | bit 0 of word 0 set again (the kernel writes 0x21; the microkernel clears bit 0 when it first loads the context), +0x20/+0x24 zeroed (no render details in flight) |
| PB | free list, block list and descriptor as at creation |
| RT dataset | RT data, render details, background block and entry words as at creation; frame count 0, so the next render is the dataset's first again |
| HW transfer context | bit 0 of word 0 set again |

Nothing runs between the reset and that first job, so nothing races with it.

## Result

`powervr_sgx_hang`, one round, then five:

```
round 0: render right
round 0: mixed render + transfer right
  faulting render: wait 0, B state 1, resets 0 -> 1
round 0: hang recovery right
...
round 4: hang recovery right            (resets 5 -> 6)
all right
```

Each round's hang case checks:
- the faulting context B is `GUILTY`, and its next submit fails with -ECANCELED;
- the reset count went up by exactly one;
- context A (alive since the start), a new context C and the transfer context render or
  blit right;
- a mixed render and transfer submit on A is pixel-exact.

After six resets:
- 20 frames on 2 contexts: 20/20 right;
- 5 rounds of blits: all right;
- the uAPI suite: 31/31;
- `firmware running: 1 lost: 0 resets: 6`.

Which of the four set-ups is actually needed was not separated. A module parameter per
item would split them in one slot.

## Regions for the open microkernel

The 3D dump from 0530 again, now with the region headers (rgn0/rgn1), tail pointers and
the TA's state blocks, before and after.

`render_before` is taken as the last kick is written, after the first kick's TA. Its
region headers already carry the list pointers (`00000000 00001000 00000000`, then
`00001080`, `00001400`, ...). After the frame, the first word of each header holds the
tile coordinates and flags (`04000000`, `00020000`, `00040000`, ..., `00000002`), so
the second kick, which runs the terminate words and then the 3D, writes those.

Data:
- `docs/log/data/2026-11-03-0545-drm-3d-regions.txt`;
- `docs/log/data/2026-11-03-0550-drm-reset-rearm.txt`.
