# M10 scope decisions

## The dock works again: M10 runs in both modes; M16-01 returns to M10-13 (user decision, 2026-11-24)

Since 2026-11-17 every docked part was deferred because the user's dock was broken (docs/milestones/M06/SCOPE_DECISIONS.md),
and on 2026-11-19 M10-13 (the docked work deferred from M06, M07 and M08) moved to M16-01
(docs/milestones/M08/SCOPE_DECISIONS.md). At the start of M10 the user reported the Switch docked. Asked "Which should
M10 use?" with the options undock and stay handheld, the dock works now and both modes are tested, or the dock only
charges, the user chose **"Dock works now: test both"**.

What changes:

- **M10-04, M10-07, M10-08 and M10-11** are done in handheld **and docked** mode, as written ("both modes").
- **M10-13** is active again and owns everything M16-01 held: M06-06's target and camera recreation on docking, M06-13's
  docking check, M06-20's docked 1920x1080 output and repeated dock changes from both start modes with a flat heap,
  docked S2/S4 timing (M06-11), the docked beta acceptance run (M06-18), M07-11's docked audio drift and cutscene
  synchronization, and M08-02's docked developer-menu layout including a dock change while the menu is open.
- **M16-01** becomes a "moved back to M10-13" record.

What does not change: the Pro Controller (M16-02, the user has none) and M09's docked widescreen acceptance (M16-03)
stay in M16. Nothing docked is claimed until a docked run backs it.

## Builds use one CPU core (user instruction, 2026-11-24)

The build host is the user's NAS: "Use only one core of cpu". tools/build_game.py takes `--jobs` and
tools/build_core_tests.py `NX_BUILD_JOBS`, both 1 by default (compile jobs and the container's CPUs); CI passes 4.
No test result depends on it; builds are slower.

## Docked mode first (user instruction, 2026-11-24)

"Focus test on docked mode." M10's hardware runs are done docked first (the busy-route baseline, the docked work of
M10-13, the soak and lifecycle runs); handheld repeats follow for the tasks that name both modes. It changes the
order, not the scope: M10-04, M10-07, M10-08 and M10-11 still need both modes before they are ticked.

## A lowered, measured target for M10; 25+ fps later (user decision, 2026-11-25)

M10-04's "25+ FPS baseline in both modes" (>= 25 presented gameplay frames/s, p95 <= 40 ms after warmup) is not met:
docked at stock clocks the busy route reached 14.2 game fps (route-docked-4, from 7.7 at the start of M10), the
30-minute soak 15.5 fps; the game thread alone takes ~31-34 ms a frame with no single hotspot left, and the slow frames
come from texture streaming. Asked "How should M10 treat the target?" with the options keep optimizing, record the
baseline and move the target, or lower the target, the user chose **"Lower the target"** ("Set a lower measured target
for M10 (e.g. the current median / p95) and keep 25+ fps as a goal for later").

What changes:
- **M10-04** is met by the busy route docked at stock clocks reaching the lowered target, set from the measured
  baseline of the M10 candidate (route-docked-5, recorded with its numbers in TASKS.md) and held by the candidate.
- **25+ fps** (>= 25 game fps, p95 <= 40 ms on the busy route) stays a goal: **M16-05** (new).

## Docked only for now; M10's handheld parts later (user decision, 2026-11-25)

Handheld runs must be at stock clocks; sys-clk on the console has a profile for the title the game runs under
(05446530ACA7E000: handheld while charging on the official charger, CPU 1785 MHz). Asked how the handheld runs should
be done (on battery, the profile removed, or docked only for now), the user chose **"Docked only for now"**.

What changes: M10-04, M10-07, M10-08 and M10-11 are done docked; their handheld parts move to **M16-06** (new). Nothing
handheld is claimed in M10.
