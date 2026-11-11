# Desktop lag measurements

Where the time goes on the Xfce desktop (Xorg with glamor, xfwm4 compositing
through GLX) while a person uses it, on a CE5300 box.

```
BOX=10.0.0.110 BOX_PASS=... lag-run.sh ../../../../../mg-stage/x86_64-gl-HASH/usr OUT \
    default slots3 nocomp xpresent vbloff software
```

Hold the box's lock, RAM-boot powervr_sgx with the monitor attached, and have
the desktop root at `/opt/desk` (`smoke.sh` unpacks it). One variant takes about
four minutes.

| file | runs on | what |
|---|---|---|
| `lag-run.sh` | host | pushes the Mesa under test, runs each variant, fetches the results, summarizes |
| `lag-box.sh` | box | one variant: X, a new xfwm4 with the variant's options, the tracing, the workloads |
| `lag-wl.sh` | box, as `desk` | one workload, driven by xdotool at 60 Hz |
| `lag-analyze.py` | host | the summary table |

Workloads: `idle` (15 s), `drag` (a terminal dragged by its title bar for
10 s), `term` (60,000 lines into a terminal, in lines a second), `menus` (the
panel's and the desktop's menus, 10 times each), `drag-gl` (the drag while
glmark2 draws in a window: "laggy while a game runs"), and `firefox` (a long
page scrolled by key and by wheel).

Variants: `default`; `slots3` (`SGX_SLOTS=3` for X and the clients);
`nolevers` (`SGX_DEBUG=nolazyclear,nozsdiscard`: the A/B of lazy clears and
depth discards); `flatten` (`SGX_COMPILER_DEBUG=flattenuniform`: uniform
conditions flattened into selects again, the A/B of branching on them);
`nocomp` (xfwm4 without its compositor); `xpresent` and `vbloff` (xfwm4
synced to vblank through Present, or not at all); `software` (no GPU: Xorg
without glamor and llvmpipe from the root's own Mesa, the baseline);
`glamordebug` (`GLAMOR_DEBUG=1`: glamor's CPU fallbacks, not for timing);
`shaderdump` (`MESA_SHADER_DUMP_PATH`: every program X, xfwm4 and the clients
link, as `.shader_test` files in `OUT/shaderdump/shaders`, for the compiler).

For each workload the summary has:

- **GPU busy per process.** Each job's service time (from the later of its
  kick and the previous job's completion, to its completion), from the
  kernel's `pvrsgx_kick`/`pvrsgx_job_done` tracepoints. Jobs are attributed
  to the submitting process through drm_sched's `drm_sched_job` and
  `drm_run_job` events. Also renders a second, and the render target sizes
  (a full-screen size each frame means full redraws, not damage).
- **CPU per process** (a share of one core) and of the whole machine, from
  `/proc` every 0.5 s.
- **The driver's own view** (`SGX_DEBUG=stats` in X and xfwm4): frames a
  second, draw and flush+wait time, slot waits, CPU blits, render latency.
- The workload's own number.
- **perf**, system-wide while each workload runs, from the root's linux-perf
  (or the box's): `perf-W-dso.txt` (by process and library),
  `perf-W-sym.txt` (functions), `perf-W-calls.txt` (Xorg's and xfwm4's
  call chains). Use a Mesa build with debug info (`mesa/build.sh` keeps it)
  so the profiles name the driver's functions.

## What the EA1 can't show

- **Cedarview's scanout.** On gma500, lazy scanout moves a window-system
  buffer to the display device, and the CPU copies each frame. The EA1 scans
  out of simpledrm, where the kernel copies the damage at dirtyfb. The copy's
  cost on a tester's machine shows only there: their `SGX_DEBUG=stats` "into
  scanout" latency and Xorg's CPU share.
- **Input to scanout.** That needs a camera or a light sensor. Render latency
  and the frames a second the compositor reaches are the proxies here.
- The testers' resolutions (1366x768 and 1920x1080 laptops and monitors)
  unless the EA1's monitor is set to them, and their CPUs (N2600, D2500:
  the same Saltwell class as the CE5300).
