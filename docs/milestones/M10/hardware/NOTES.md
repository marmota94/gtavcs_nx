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
