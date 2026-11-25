# M10 hardware notes (docked first)

Console 192.168.1.91, Sphaira by title override, docked (TV output). Stock clocks. 16:9, internal scale 1x.

## route-docked-1 (2026-11-24): the busy route, docked

NRO `af1a582d` (M09's final candidate, taken from the card; `git diff 0f6cc36 HEAD -- src patches CMakeLists.txt
cmake third_party upstream` is empty, so the game code is this branch's). Schedule `tests/switch/m10-route.cfg`.
Log `route-docked-1-gtavcs.txt`, summary `route-docked-1-route.txt` (`tools/switch_m10_session.py route`), CSVs in
`route-docked-1/`. Screenshots private (build/evidence/M10/screenshots).

- Output 1920x1080 (the `[video]` line): docked from launch. The cruise ran: 5,642 m of the 5,736 m loop by 360 s,
  the player driving at every gameinfo from 90 s.
- Window 90-390 s: **7.7 game frames/s**, median 104.7 ms, p95 443.9 ms, p99 712 ms, max 1.09 s; 2,072 of 2,299
  frames over 40 ms. 25+ baseline **not met**.
- Game thread 37.4 ms median (handheld M09 standing: ~22 ms). The main thread's host frame 80.7 ms median, of which
  GL calls 78.9 ms (`gl_ms`): docked, the main thread's GL work is the limit (handheld 16:9 standing, M09: 6.4 ms of
  35 ms). GL_TIME_ELAPSED per present median 2.2 ms, p95 80.7 ms.
- Texture creation up to 635 ms in one frame (`tex_create_us` max), 6.8 MB uploaded in one frame (streaming).
- Heap in use up to 741.8 MiB, obtained 920 MiB at the end. No audio underrun, 0 GL errors, no crash.

## The "shaking" picture (user report, 2026-11-24)

The user recorded 30 s with the console's capture button (`video-sha256.txt`; the video stays private under
build/evidence/M10/video). It shows the S3 intro cutscene in the office (not route-docked-1's window: that run was
driving throughout; the console's clock and the host's differ). Measured on the video (30 fps capture, 886 frames):

- **9.1 new pictures a second**, unevenly spaced: gaps of 2, 3 and 4 capture frames (67-133 ms) alternate, with
  stalls of 5-10 frames (167-333 ms) every few seconds. The camera moves by game time, so each new picture jumps by a
  different amount: the motion judders. The camera position itself shows only small back-and-forth steps of about
  one 8-px cell between new pictures.
- Every picture carries a faint offset copy of itself: the game's own **trails** effect (upstream NATIVE_RENDERER.md:
  a downsample of the frame blended back, warped, at FIX ~8; on the PS2 as well). Not a port defect.

So the shake is the frame rate and its uneven pacing (M10-03/04/06), not a camera or presentation bug. It is
rechecked on video after each performance change.

## stand-aspect-docked (2026-11-24): the wide picture's cost docked, standing

NRO `af1a582d`, `tests/switch/m10-stand-aspect.cfg`: the player standing at the S4 spot (M09-06's view), the aspect
switched live every 50 s (the `[present] display aspect` lines confirm each switch). `tools/switch_m10_session.py
route ... --from A --to B` per window:

| Window | Aspect | Game fps | Median / p95 ms | game_ms | host_ms | gl_ms | wait_ms |
|---|---|---|---|---|---|---|---|
| 80-130 | 16:9 | 11.3 | 67.2 / 155.8 | 33.7 | 56.6 | 19.4 | 27.9 |
| 130-180 | 4:3 | 21.0 | 39.0 / 68.2 | 35.1 | 23.0 | 15.6 | 0.0 |
| 180-230 | 16:9 | 12.2 | 67.8 / 135.3 | 36.1 | 58.7 | 19.2 | 26.7 |
| 230-280 | 4:3 | 18.5 | 41.1 / 102.2 | 35.6 | 40.8 | 35.9 | 0.0 |

M09-06 handheld, same spot: 4:3 median 23.9 ms (game_ms 21.7), 16:9 48.8 ms. Docked:

- 16:9 costs about half the frame rate docked too: the main thread's host frame doubles (23 -> 57 ms) and the game
  waits on it (wait_ms 0 -> 27 ms).
- The **game thread itself takes 34-36 ms docked against ~22 ms handheld** at the same spot and stock CPU clock (the
  same in both modes), at either aspect. Something takes CPU from it docked. libnx gives every std::thread the
  process's default core as its ideal core (`__syscall_thread_create`: threadCreate(..., -2), then the process core
  mask as affinity): the game thread, the render worker, the GS stage and the main thread may share a core. Next run:
  each thread's CPU share and core mask (`[threads]` result lines, src/switch/thread_stack.c).

## threads-docked (2026-11-24): where the CPU and the GPU go, docked

NRO `ae99f34c` (commit 4b75456, clean): the `[threads]` lines. Same schedule as stand-aspect-docked.

- libnx's threads have **no ideal core and the mask 0-2** (`[threads] created tN: ideal core -1, mask 7`): the kernel
  spreads them over the three application cores. They are not confined to one core.
- **16:9 windows: the CPU is mostly idle.** Sum of all threads ~110-118% of one core (main ~28%, game thread t4
  ~44%, the render worker / GS stage t5 / t6 ~20% each). 4:3 windows: ~200-220%, the game thread ~87%.
- **16:9 docked is GPU-bound**: the `[rhi-stats] gpu` lines give ~41-50 ms of GPU time per present at 16:9
  (61-73 presents per 5 s line) against ~10-18 ms at 4:3. M09-06 handheld: 21.3 ms (16:9) / 11.4 ms (4:3); with
  640-wide targets at 16:9 34.2 ms - so the target width alone is not the cause; 4:3 costs the same docked and
  handheld, 16:9 docked costs twice handheld.
- At 4:3 the game thread is the limit (~35 ms per game frame, 87% busy). Why it takes 35 ms docked against ~22 ms in
  M09's handheld run is open (clocks not read yet).

Next: the GPU time of every pass (`PS2X_GL_PASS_TIME`, `tests/switch/m10-stand-passes.cfg`).

## passes-docked (2026-11-24): the GPU time by pass, docked

NRO `b7737f40` (commit 3a87fc4, clean), `tests/switch/m10-stand-passes.cfg` (stand-aspect with `PS2X_GL_PASS_TIME`).
`[rhi-stats] passes per present` lines (GPU time per present by pass name and target size):

- **16:9**: the passes into the picture's **853x512** targets take **~20-35 ms per present** in ~5-10 passes (~3.5 ms
  a pass); everything else ~4 ms (1920x1080 output passes ~1.1 ms, the window ~0.8 ms, 341x256 / 341x512 ~2 ms).
- **4:3**: the **640x512** picture passes take ~10-12 ms per present in ~25-37 passes (~0.35 ms a pass).

So the cost is the picture's 3D drawing, 2-3x more at 16:9 than its 1.33x pixels. And docked should be the faster
mode (GPU 768 MHz stock against 384 / 460 MHz handheld), yet 4:3 costs the same GPU time docked as handheld (M09-06)
and 16:9 twice, and the game thread takes ~35 ms docked against ~22 ms handheld. That fits a console below stock
clocks when docked (a supply that cannot give full power reduces them). Next run reads the clocks (`[nxrun] clocks`
lines: clkrst CPU / GPU / EMC, apm mode, psm charger; src/switch/clocks.c).

## clocks-docked (2026-11-24): docked runs at stock clocks

NRO `46c1c4fc` (commit 3fb6a66, clean), the stand-passes schedule. Every `[nxrun] clocks` line: `cpu_mhz=1020
gpu_mhz=768 emc_mhz=1600 perf_mode=1 perf_config=00010001 charger=enough-power battery=100% operation_mode=docked`.
The docked numbers above are **stock** (the reduced-clock idea is wrong). Windows as before: 16:9 11.2 / 11.8 fps,
4:3 21.9 / 17.9 fps; game thread 32-36 ms.

**sys-clk is installed and enabled on this console** (`/atmosphere/contents/00FF0000636C6BFF/flags/boot2.flag`,
`/config/sys-clk/config.ini`): per-title profiles up to CPU 1785 / GPU 921 MHz, several for handheld while charging.
A homebrew run takes the overridden title's ID, so a profile may apply to it. This docked run was at stock. M09-06's
handheld numbers (game thread ~22 ms) recorded no clocks: they may have run overclocked (1785 / 1020 = 1.75, close
to 35 / 22). M10 comparisons use only runs with clock lines; every M10 run records them.

## wide0-docked crashed the console (2026-11-24): the clock reading removed

NRO `46c1c4fc`, `tests/switch/m10-stand-wide0.cfg` (`PS2X_HW_WIDE_SCALE=0`). The game log (`crash-wide0/gtavcs.log`)
ends in the first report at t=0: the `progress` and `threads` lines, then nothing from the main thread (the movie
thread's swscaler line follows). The next call there was the first `clocks` line, which opened three clkrst sessions to
**pcv** (and apm, psm). Atmosphère crash reports of the same moment: **pcv** aborted (User Break, 0xCC0B), then
**nvservices** (0xE401); the fatal service itself then aborted (the user's photo: Title ID 0100000000000034,
std::abort, `report_681e6ed0983.bin`); the user pressed POWER. The run before with the same NRO (clocks-docked) read
the clocks without trouble; sys-clk on this console also holds pcv sessions.

Fix (src/switch/clocks.c): no clkrst and no apm: the line keeps the performance mode (applet), the charger and battery
(psm), the operation mode, and adds the program ID the run has, against sys-clk's profiles. The clock rates of
clocks-docked (stock 1020 / 768 / 1600) stay the record that docked runs are at stock; wide0-docked is repeated.

## align64-docked, first try (2026-11-24): the run file refused, not a crash

NRO `02f7553c`. The game log: `[core] nxrun.cfg:8: env needs NAME=value with NAME starting VCS_ or PS2X_` - the new
sampler's variables were named `NX_SAMPLE_*`; the run-file check (src/core/run_config.cpp) allows only `VCS_` and
`PS2X_` names. The core boot failed into the system error dialog, which waited for a button; the session timed out
after 900 s. No crash report (the ones on the card are pcv's / nvservices' from wide0-docked). Renamed to
`VCS_NX_SAMPLE_*` and `VCS_NX_THREAD_CORES` (the port's prefix, as `VCS_NX_OUTPUT`).

## align64-docked (2026-11-25): the width is not it; the game thread's first profile

NRO `318205e7` (commit e75c7e6, clean): `PS2X_HW_WIDE_ALIGN=64` (patch 0014) makes the picture's targets 896x512; the
sampler (`VCS_NX_SAMPLE_THREAD=4`, 500 Hz, 80-280 s) on the game thread.

- 16:9 standing: 11.4 game fps, median 68.9 ms (as at 853 wide); the **896x512** passes take 26.7-28.1 ms per present in
  ~8 passes - the same as 853x512. The width's alignment is not the cost.
- Game-thread profile (`align64-docked-profile.txt`, 99,395 samples, 0 failed): **55.4% waiting** on a condition
  variable (16:9 is GPU-bound: the game waits for the render path), 3.2% blocked in a contended mutex. Busy time has no
  single hotspot: the runtime's native VU0 helpers `matrixTimes3From` 3.8%, `boxClip` 3.7%, collision
  (`sphereTriangle`, `CollisionVu`) ~3%, `dispatchGuestBranch` 2.3%, `free` 1.2%, SPU2 mixing ~1.1%, generated game
  code 16.8% spread over thousands of functions; perf.csv's SD writes 0.7% (measurement overhead).

## postfx-*-docked (2026-11-25): upstream's native trails cost half the frame rate at 16:9

NRO `318205e7`, `tests/switch/m10-postfx-{bt,bc,tc,0}.cfg`: 16:9 docked, standing at the S4 spot, window 70-130 s,
one native post effect at a time back to the game's recorded GS draws (`PS2X_HW_POSTFX`: b bloom, t trails,
c frame copies / motion blur native; default btc).

| PS2X_HW_POSTFX | Game fps | Median / p95 ms | game_ms | GPU per present |
|---|---|---|---|---|
| bt (copies recorded) | 11.1 | 68.2 / 185.7 | 37.4 | ~40 ms |
| tc (bloom recorded) | 11.5 | 66.3 / 145.4 | 33.6 | ~41 ms |
| **bc (trails recorded)** | **20.8** | **40.0 / 70.0** | 35.5 | **~24 ms** |
| 0 (all recorded) | 21.4 | 39.5 / 61.7 | 35.1 | ~16 ms |

Upstream's native trails (`gs_hw.cpp` nativeTrails: the frame downsampled into a buffer widened to 683 wide at 16:9,
blended back through warped strips) take ~16 ms of GPU time a present on the Switch; with them recorded 16:9 docked
runs as 4:3 does, and the game thread (~35 ms) is the limit. Not the filtered render-target read (no `filtered copy`
line in these logs). The screenshots at 100 s (`postfx-screenshots-sha256.txt`, private) look alike at 16:9 with
native and recorded trails; the recorded trails' warp is a third wider on screen at 16:9 (upstream's reason for the
native chain), the same at 4:3.

**Change (M10-03):** src/core/core.cpp sets `PS2X_HW_POSTFX=bc` unless the run sets it: trails recorded, bloom and
frame copies native (`[core] post effects native: bc` in the log).
