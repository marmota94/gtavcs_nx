# M09 — Widescreen: measurements and comparisons

Switch: handheld, paired Joy-Cons attached, 1280x720 output, internal scale 1x, stock clocks, Mesa nouveau GL. Desktop
reference: upstream's Windows build at the pin (`build/m02/reference`, M02-01) through D3D11 (NVIDIA GeForce RTX 4060),
hidden, started by `tools/m04_reference.py <cfg> <dir> --rhi d3d11 [--setting display.aspect=169]` on the same
schedules. Logs and results: `hardware/<run>-gtavcs.txt`, `hardware/<run>-results.txt`,
`hardware/reference-<run>/reference-*.txt`. Game pictures are kept privately (`hardware/screenshots-sha256.txt`).

## Runs

| Run | NRO | Schedule | What it shows |
|---|---|---|---|
| `presets-r1`, `presets-env` | af1a582d | `m09-presets.cfg`, `m09-presets-env.cfg` (`VCS_ASPECT=43`) | the presets again after review 1; patch 0013's fallback leaving a key locked by its environment variable alone |
| `silent3` | 99472722 | `m09-silent.cfg`, no input, 470 s | S1-S4; view window and planes in S2, S3, S4, at 16:9 and live 4:3; cutscene bars; HUD modes; scope and camera masks |
| `presets` | 99472722 | `m09-presets.cfg` | Enhanced / Widescreen, light / Authentic with the narrowed aspect |
| `shape3` | 99472722 | `m09-shape.cfg` | every presented frame into and out of the pause menu (Map page) |
| `text` | 99472722 | `m09-text.cfg` | the mission subtitle and help box in each HUD mode |
| `silent2`, `shape2` | b3fc1722 | as above | the same, before the preset fix (`silent2`'s scope commands lacked TERMINATE: no scope) |
| `cost-43`, `cost-169`, `cost-169n` | b3fc1722 | `m09-cost-*.cfg`, Cross to S4, 150 s standing | cost of 16:9 against 4:3; `169n` with `PS2X_HW_WIDE_SCALE=0` (diagnosis). `cost-169` and `reference-cost-169` ran `m09-cost-169.cfg` before 75a65ac added TERMINATE to its scope scripts: no scope shown on either; the 80-200 s cost window is not affected |
| `drive` | b3fc1722 | `m09-drive.cfg` | upstream's cruise route (a low flight at ~35 m, not road driving), commanded 40 m/s (about 16.7 m/s average), a screenshot every 15 s from 60 s |
| `accept2`, `relaunch` | b3fc1722 | `m09-accept.cfg`, `m09-relaunch.cfg` | the person switching in the developer menu; the choice loading at the next start |
| `reference-silent2`, `reference-cost-43`, `reference-cost-169` | desktop | the same schedules | numbers and pictures to compare |
| `reference-silent` | desktop | `silent`'s schedule | the S3 view window and planes at 16:9 and 4:3 (M09-02) |

Superseded runs kept as history: `wide-base` (main's NRO, `VCS_ASPECT=169` locked the setting), `silent` (scheduled
for M06's stage times; M07's boot movies now come first), `shape` (Start pressed 150 ms: missed at ~10 game frames a
second), `accept` (nobody pressed a button).

Also kept: `hardware/pause-probe.cfg` with `hardware/reference-pause-probe/` (a desktop run, D3D11, 16:9, that found
how to open the pause menu from a run file: Escape held 400 ms or the pad's Start, `menustate` `open=1 ...
page=MAP_PAGE`; it set the shape runs' Start hold) and `host-tests-pre/` (the host tests on main's 77ddfc7 before any
M09 change, dirty=True, a baseline only, not proof).

## Hor+ view window and entity planes (M09-02)

`peekf 0x742AC0 4` = (view window x = tan(FOV / 2) x r, aspect x r, near, far); right plane TheCamera + 0xB60 = (cos h,
-sin h), left + 0xB70 = (-cos h, -sin h): the plane's edge is sin h / cos h.

| Scene | Switch | Desktop | Edge (Switch) |
|---|---|---|---|
| S2 flyover, 16:9 (120 s) | `0.93361 1.66667 0.1 2000`, `0.730967 -0.682412` (`silent2`, `silent3`) | the same (`reference-silent2`) | 0.93357 |
| S3 cutscene, 16:9 (200 s) | `0.259437 1.66667 0.3 2000`, `0.967957 -0.251116` (`silent2`, `silent3`) | the same | 0.25943 |
| S3, one camera FOV at 16:9 | `0.433374` (`silent3`, 231 s) | `0.433374` (`reference-silent` 240 s) | 0.43336 |
| S3, the same at 4:3 (live switch) | `0.325031 1.25 0.3 2000`, `0.951028 -0.309104` (`silent2` 225 s) | the same (`reference-silent` 259 s); 0.433374 / 0.325031 = 1.33333 | 0.32503 |
| S4, 16:9 | `0.93361 1.77778 0.1125 2000`, `0.730967 -0.682412` (`silent3` 360 / 385 s, `cost-169`) | the same (`reference-silent2`, `reference-cost-169`) | 0.93357 |
| S4, 4:3 (live switch) | `0.700208 1.33333 0.1125 2000`, `0.819162 -0.573562` (`silent3` 379 s, `cost-43`) | the same (`reference-silent2`, `reference-cost-43`) | 0.70018 = tan 35 degrees |

0.93361 / 0.700208 = 1.33331 (r = 16:9 / 4:3). The entity planes follow the view window at both ratios and back. One
desktop sample (`reference-cost-169` 150 s) caught the right plane between the game's rebuild (FUN_0037b2c8, each camera
update) and the widening, which runs just before each sphere test; the next sample matched.

## Reference scenes (M09-08)

`silent3` (Switch, 99472722) against `reference-silent2` (desktop, D3D11, the same schedule at 16:9), and `cost-169`
against `reference-cost-169`, the screenshots of the same name put side by side at half size (private, not frame-locked; no pixel diff claimed):

| Pair | Seen |
|---|---|
| `s4-361-hud43` | the same framing of the player and the barracks; HUD (clock, health, money, fist) and radar inside the middle 4:3 at the same places |
| `s4-366-hudedges` | the HUD at the right edge and the radar at the left edge on both |
| `s4-371-hudstretch` | the HUD and radar spread the same way, the radar widened into an ellipse on both |
| `s4-380-aspect43` | both pillarboxed 4:3 with equal black bars (80 of 640 px each side), the same framing |
| `s4-386-aspect169` | back to the full 16:9 picture, as `s4-361` |
| `s2-121` (flyover) | different flyover frames: both the full 16:9 picture, nothing more comparable |
| `s4-200` (`cost-169` / `reference-cost-169`) | the same framing; HUD (keep 4:3) and radar at the same places (the Switch picture also carries the runtime's small REC mark, top left: the run records perf.csv, `PS2X_PERF_LOG`) |
| `s3-201-bars`, `s3-206-nobars` | different cutscene shots: black bars top and bottom inside 16:9 with the same thickness on both; the whole picture without them |

Differences seen are those of unlocked runs: the clock time, the waypoint arrow's animation, pedestrians. The scope
is not compared: the desktop references showed none (`reference-silent2` and `reference-cost-169` scope shots are
each one picture twice; gameinfo mode 4 at 395 / 405 s (`reference-silent2`) and at 210 / 220 / 230 s around the 215 / 227 s shots
(`reference-cost-169`); scopemask 255 / 63 and 255 / 61); the Switch's scope rests
on M09-05's strip test (`hardware/scopemask-results.txt`).

## Frame shapes (M09-04)

`hardware/shape3-burst-in-shapes.txt`, `-out-shapes.txt`: per presented frame the picture rectangle (lit columns of the
320x180 capture), the shape the runtime showed it at (burst.txt `aspect`) and what it shows (upstream's menushape
test: the game's sky at the top middle). Entering: 31 x (16:9, game), 89 x (4:3, menu). Leaving: 13 x (4:3, menu),
107 x (16:9, game). 0 mismatched of 240. `shape2` (b3fc1722) first failed: the session's judge (`session-shape2.txt`: burst-in 97, burst-out 12 mismatched, `RESULT game FAIL`) compared each picture with the 16:9 `frame` rectangle it is placed in, so every 4:3 menu frame failed (96), and its rule that a menu-flagged frame without 3D must be 4:3 failed frame 0011, the menu's first batch, which still showed the game at 16:9 (the 97th; shape3's frame 0018 is the same case); the judge was changed in 92357a4 to compare it with the `aspect` it is shown at, and the committed shape2 shapes files are that judge's re-check of the same PNGs (hashes in the session log): 24 game frames then 96 menu frames entering, 12 menu then 108 game leaving, 0 mismatched.

## Cost of 16:9 against 4:3 (M09-06)

Handheld, 1x, the player standing at the S4 spot; game frames from `perf.csv` over 80-200 s, GPU from the
`[rhi-stats] gpu` lines over the same span (GL_TIME_ELAPSED from one present to the next), targets from
`[rhi-stats] targets` in play (from ~10 s each run logs 14 or 15 targets, the two values in the table; the boot logs
1 and 5; inside
80-200 s `cost-43` logged only its 17.6 MiB), heap from the `progress` lines.

| | 4:3 (`cost-43`) | 16:9 (`cost-169`) | 16:9, 640-wide targets (`cost-169n`) |
|---|---|---|---|
| Largest target (picture) | 640x1472 | 853x1472 | 640x1472 |
| Render targets + depth (in play) | 17.6-21.1 MiB | 23.0-26.5 MiB | 19.9-23.4 MiB |
| GPU per presented frame, mean | 11.43 ms | 21.28 ms | 34.20 ms |
| GPU time per second | ~548 ms | ~537 ms | ~611 ms |
| Game frame median / p95 | 23.9 / 57.5 ms | 48.8 / 366.1 ms | 49.1 / 103.3 ms |
| Game frames over 250 ms | 6 of 4,169 (to 405 ms) | 131 of 1,288 (to 456 ms) | 9 of 1,928 (to 407 ms) |
| Heap in use at 200 s | 590.0 MB | 638.0 MB | 632.1 MB |

At 16:9 the game waits on the render pipeline (`wait_ms` median 0 -> 23.7 ms); the GS stage goes from ~12 to ~70 ms
(`perf` lines), and with the wide targets about once a second a game frame takes 350-450 ms (render stage 240-330 ms,
the game waiting its 100 ms limit, the main thread's longest pump ~95 ms). The desktop reference moves the same way at
its scale: render worker 1.7 -> 3.6 ms, GS stage 1.25 -> 2.4 ms (upstream: "the render worker roughly doubles with the
pixels and the extra draws"). The heap figures include the texture cache, which differs between the runs (more of the
world in view at 16:9); they are not a controlled memory cost. The user kept 16:9 the default at this cost
(SCOPE_DECISIONS.md); the stall is M10-03's.

## Frame edges along the cruise route (M09-07)

`drive`: upstream's cruise rig (`cruise`, upstream docs/PERFORMANCE.md): its city route is a low flight at a fixed ~35 m over the roofs (`player` z 34.70-35.01 at all 24 samples) that streams the cells along it - not road driving; mid-air over water from 240 to 345 s. The cruise commanded 40 m/s from 44 s; `cruise status` 334 m at 60 s -> 5,336 m at 360 s of the 5,736 m
loop (about 16.7 m/s average); the player's positions every 15 s cover both
sides of the bay (574,509 ... -675,1162 ... 574,236). 24 screenshots: buildings, trees, roads, water and the skyline
reach both outer eighths of the 16:9 picture; the rig's cruise moves the car through geometry, so some frames are inside
a structure or a building face (drive-105, drive-150, drive-210) or a blur in palm trees (drive-075); at drive-225 the
game shows a stunt-jump result for the flight. No hole or missing cell at the edges seen.
Stills 15 s apart show what is drawn at the edges, not whether it appeared late: pop-in is not claimed.
