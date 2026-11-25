# M10 — Stability, lifecycle and measured performance: task record

Branch `milestone/m10-stability-performance`, cut from main `f4f1c77` (M09 merged). Upstream pin unchanged (`ecf150e`);
root patches 0001-0013 and, from this milestone, 0014 (`PS2X_HW_WIDE_ALIGN`, diagnosis) and 0015 (the VU0 natives
through NEON helpers). Scope: [SCOPE_DECISIONS.md](SCOPE_DECISIONS.md) (the dock works again: both modes, docked first;
M16-01's docked work back in M10-13; builds on one core). Hardware records and measurements: [hardware/NOTES.md](hardware/NOTES.md).

**Status: in progress. No task is ticked yet.** This file is the working record; it becomes the proof table when the
tasks close.

## What was built so far

| Piece | Where |
|---|---|
| The busy route (cruise over the city cells, 420 s, window 90-390 s) and its summary (fps, median / p95 / p99, slow intervals, stages, underruns, GPU, heap, the 25+ verdict) | `tests/switch/m10-route.cfg`, `tools/switch_m10_session.py route` |
| Each thread's CPU share and core mask on every report; the run's conditions (operation / performance mode, program ID - sys-clk's profiles are per title) | `src/switch/thread_stack.c`, `src/switch/clocks.c` |
| A sampling profiler for the process's own threads (svcSetThreadActivity + svcGetThreadContext3; never the main thread) and its symbolizer against the run's ELF | `src/switch/sampler.c`, `tools/switch_m10_session.py profile` |
| GPU time per pass (timestamps), CPU time of the GL backend by kind of call | `src/rhi/gl/rhi_gl.cpp` (`PS2X_GL_PASS_TIME`, `PS2X_GL_CPU_TIME`) |
| Trails drawn as recorded on the Switch (upstream's native trails cost ~16 ms of GPU a present at 16:9) | `src/core/core.cpp` (`PS2X_HW_POSTFX=bc` default) |
| Constant and vertex buffers through persistently mapped stream rings; released textures reused from a pool | `src/rhi/gl/rhi_gl.cpp` (`PS2X_GL_UBO_RING`, `PS2X_GL_VB_RING`, `PS2X_GL_TEX_RECYCLE`) |
| The VU0 natives through NEON helpers with the AVX path's rules (bit-identical: `vu0verify-docked`, 0 of ~2 million differ) | patch 0015 |
| Injected faults: reads, texture creation, a save cut like a power cut | `src/vfs/folder_disc.cpp`, `src/rhi/gl/rhi_gl.cpp`, `src/switch/open_audit.c` (`VCS_NX_FAULT_*`) |
| Runs: soak, lifecycle, faults | `tests/switch/m10-*.cfg` |
| Builds on one core | `tools/build_game.py --jobs`, `NX_BUILD_JOBS` |

## The busy route, docked, at stock clocks

| Run | NRO | Game fps | Median / p95 | Game thread | Main thread (GL) |
|---|---|---|---|---|---|
| route-docked-1 | af1a582d (M09) | 7.7 | 104.7 / 443.9 ms | 37.4 ms | 80.7 (78.9) ms |
| route-docked-2 | 73de4f16 (trails recorded) | 7.9 | 79.6 / 377.1 ms | 43.9 ms | 51.2 (44.3) ms |
| route-docked-3 | ff184544 (+ rings, NEON) | 10.9 | 49.9 / 279.5 ms | 35.9 ms | 23.5 (12.6) ms |
| route-docked-4 | 4dfc629b (+ texture pool) | 14.2 | 43.7 / 196.9 ms | 34.4 ms | 24.0 (14.3) ms |

## Tasks

| Task | Status |
|---|---|
| M10-01 | Busy route defined (`m10-route.cfg`); stock clocks read docked (1020 / 768 / 1600, clocks-docked); sys-clk's per-title profile for our title found (overclocks handheld while charging). Open: the warm / cold procedure, handheld at stock (battery or the profile removed). |
| M10-02 | Instrumented: frame / stage times (perf.csv), threads' CPU, GPU per frame and per pass, GL CPU by kind, heap, underruns, the sampler. Open: streaming time and cache churn as their own counters. |
| M10-03 | Bottlenecks found and changed one layer at a time with before / after runs (table above, NOTES.md). Open: the game thread (~34 ms driving) and the slow-frame spikes. |
| M10-04 | Not met: 14.2 fps on the busy route docked. |
| M10-05 | Not started. |
| M10-06 | Not started. |
| M10-07 | First docked soak run (soak30-docked). |
| M10-08 | Run file ready (`m10-lifecycle.cfg`); needs the person and connected Joy-Cons. |
| M10-09 | Faults implemented, runs ready. |
| M10-10 | Fixed so far: the clkrst read that took pcv down (removed), main-thread sampling refused. |
| M10-11 | Heap recorded on every run (in use up to ~970 MiB driving, obtained ~1.0-1.1 GiB). Open: thermal, the soak. |
| M10-12 | Not started. |
| M10-13 | Not started (docked output, dock changes with a flat heap: with M10-08). |
