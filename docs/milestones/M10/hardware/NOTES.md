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

## route-docked-2 (2026-11-25): the busy route with trails recorded

NRO `73de4f16` (commit 8439ef1, clean; `[core] post effects native: bc`). `route-docked-2-route.txt`, window 90-390 s
driving: **7.9 game fps, median 79.6 ms** (route-docked-1: 7.7 / 104.7), p95 377 ms. Driving, two limits the standing
runs did not show: the game thread 43.9 ms median (streaming, more of the world), and the main thread's GL calls
44.3 ms median per host frame (`gl_ms`; texture creation `tex_create_us` p95 35 ms, max 738 ms: the streamed
textures are made on the main thread, M05-09). Heap in use up to 857 MiB, obtained 1.09 GiB at the end. 0 underruns,
0 GL errors.

## route-prof-docked (2026-11-25): sampling the main thread took the console down

NRO `1d572faa`, `VCS_NX_SAMPLE_THREAD=main,4`. The game log (`route-prof-crash/gtavcs.log`) ends after the report at
351 s of 420; the console went to Atmosphere's fatal screen (the user: "It crashed atmosphere"). The newest crash
report is the system's **controller applet** (0100000000001003, User Break in its "Prepare Thread",
`route-prof-crash/controller-crash-head.txt`); no report of the game. The run's only new factor was pausing the main
thread (SDL, window, GL, applet messages) 500 times a second; sampling the game thread alone (align64-docked) ran
clean. The samples were lost (written at shutdown). The sampler now refuses `main`.

Also in these logs:
- **No controller connected** in the docked runs (`gamepad=0`, `[platform] controller physical: ... nothing connected
  for player 1`): scripted runs need none; M10-08's controller loss needs the Joy-Cons connected.
- psm read `charger=none battery=0%` throughout (it read `enough-power 100%` in clocks-docked); psm is no longer asked.
- `program_id=05446530aca7e000`: the title the homebrew runs under. sys-clk's config has a profile for it:
  `handheld_charging_official_cpu=1785 gpu=768 mem=1600`, nothing docked. So **docked runs are at stock** (as
  clocks-docked read), but **handheld while charging on the official charger runs the CPU at 1785 MHz** (stock 1020):
  M09-06's handheld game thread (~22 ms against ~35 ms docked; 1785 / 1020 = 1.75) was very likely overclocked.
  Handheld M10 runs must be on battery or with that profile removed (user's decision).

## route-prof-docked, second try (2026-11-25): the console slept

NRO `099b133b` (commit 6407666), `VCS_NX_SAMPLE_THREAD=4,6`. Not a crash: `route-prof-sleep-gtavcs.log` shows a focus
loss at 34 s and at ~48 s `[lifecycle] confirmed suspension_ns=32012882405937` - the console slept for 8.9 hours
(auto-sleep in TV mode), resumed the next morning (game 31 fps standing in the car, 0 GL errors after resume), and the
game was closed at 75 s (`[nxrun] shutdown t=75.029`, an orderly shutdown). The sampler had not begun (from 90 s).
A real suspend / resume across 8.9 hours with the game running, for M10-08. Auto-sleep is to be off for the runs.

## route-prof-docked / route-prof2-docked (2026-11-25): where the main thread's GL time goes

NRO `a4800647` (route-prof-docked, third try, clean; `PS2X_GL_CPU_TIME`, sampler on t4 / t6) and `1029f7bf`
(route-prof2-docked: the constant ring with a map per update). The busy route, driving, CPU time of the main thread
inside the GL backend per present (`[rhi-stats] cpu per present`):

| | buffer updates | draws | texture creation | passes / copies / swap / deletes |
|---|---|---|---|---|
| orphaning (a4800647) | 6.9-22.8 ms in 190-554 updates (~40 us each) | 2.6-8.0 ms in 219-702 (~11 us) | 0-12.9 ms in 0-5 | ~1-5 ms |
| ring, map per update (1029f7bf) | 6.5-20.1 ms in 276-616 (~33 us each) | 3.9-10.2 ms | 2.3-12.3 ms | ~1-8 ms |

The updates are the GS renderer's per-draw constants (`gs_hw.cpp` updateConstants: draw / world / quad constants,
dynamic constant buffers). An unsynchronized map / unmap per update costs on Mesa nouveau about what orphaning did.
Profiles (`route-prof-docked-profile.txt`): the game thread t4 waits 58.6%, busy in the VU0 natives (boxClip 5.0%,
matrixTimes3From 4.5%), dispatch 1.9%, generated code 13.8%; the **GS stage t6** waits 48%, then **`free` 11.7% and
a contended lock 9.7%** (newlib's allocator lock), gsRun 4.6%, texture upload deferral ~4.4%.

Next: the ring mapped once, persistent and coherent (GL 4.4 / ARB_buffer_storage through getProcAddress): an update
is a memcpy. Host conformance with it (`host-tests/rhi-host-ring-persistent.txt`, Mesa iris): 127/127 within
tolerance of upstream's Vulkan backend, 91 byte-identical, as before.

## route-prof3/4/5-docked (2026-11-25): the stream rings take the main thread from ~60 to ~25 ms

Main thread's CPU in the GL backend per present while driving (`[rhi-stats] cpu per present`):

| NRO | constants (updates) | other buffer updates | draws | main thread host_ms / gl_ms median |
|---|---|---|---|---|
| 8a86737c, persistent constant ring | (together with the others) 11.5-19.0 ms in 420-710 | | 5.5-10.1 ms | 58.5 / 54.3 ms |
| ed6adebf, split timing | **0.3-0.8 ms in 300-580** | **7.0-19.1 ms in 4-5 (250-500 KiB)** | 4.9-8.4 ms | - |
| **35b0375e, vertex ring too** | 0.3-0.7 ms | **0.3-0.5 ms in 4** | 4.2-6.4 ms | **24.5 / 12.3 ms** |

The constants were cheap once the ring was persistently mapped; the cost left was the frame's few vertex buffer
updates, each orphaning the buffer's whole capacity (2-4 ms each on nouveau). With both rings (`[rhi] constant ring:
8192 KiB ... persistent map`, `vertex ring: 32768 KiB ...`) the main thread's GL time while driving is 12.3 ms median
(from 44-62), its host frame 24.5 ms (from ~60). 0 GL errors. Host conformance with the rings: 127/127 within
tolerance, 91 byte-identical (`host-tests/rhi-host-stream-rings.txt`).

route-prof5-docked (the route with the sampler, so not the baseline): 10.0 game fps, median 70.4 ms; the **game
thread** (46 ms median) is the limit now. Its profile (t4): 45% waiting; busy time led by the VU0 natives `boxClip`
6.3% / `matrixTimes3From` 6.0% (of all samples), a contended lock 4.9%, dispatch 2.2%. The GS stage (t6): `free`
16.5%, mutex 12.2% (newlib's allocator lock, likely), 48% waiting. Heap in use up to 946 MiB, obtained 1.14 GiB.
