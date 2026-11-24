# M09 scope decisions

## Docked parts to M16-03 (user decision, 2026-11-19)

M09-06 asks for the memory and GPU cost of 16:9 against 4:3 "in both modes", and M09-08 for hardware acceptance "in
handheld and docked". The dock is broken and the user has no Pro Controller (docs/milestones/M06/SCOPE_DECISIONS.md,
2026-11-17). The user's decision of 2026-11-19 (docs/milestones/M08/SCOPE_DECISIONS.md, "A milestone for the Pro
Controller and docked mode") already places them: "Docked and Pro Controller parts of M09-M14 go to it" (M16;
docs/MILESTONES.md, M16-03 "Repeat in docked mode ... every docked or Pro Controller part that M09-M14 deferred
here, each listed with its source task"). No new question was needed.

What changes:

- **M09-06** is measured in handheld (1280x720 output, internal scale 1x). The docked measurement (1920x1080 output)
  is listed under **M16-03**.
- **M09-08** is accepted in handheld, on paired Joy-Cons. The docked acceptance (the 16:9 picture on a TV, a dock change
  at 16:9 and at 4:3) is listed under **M16-03**.

What does not change: nothing is claimed for docked play. The 16:9 picture is laid out for the output size, which the
renderer sees as its back buffer (src/switch/output.c, M06-20); the docked output's geometry is the same 16:9 shape,
but no docked run backs it. Every other M09 task stays as written.

## 16:9 stays the default at its measured cost; the stall goes to M10 (user decision, 2026-11-21)

M09-06's measurement (handheld, internal scale 1x, the player standing at the S4 spot, game frames from perf.csv over
80-200 s; runs `cost-43`, `cost-169`, `cost-169n` on NRO b3fc1722): 4:3 median 23.9 ms / p95 57.5 ms per game frame;
16:9 median 48.8 / p95 366.1 ms, with a stall of about 350-450 ms roughly once a second (131 of 1,288 game frames over
250 ms; the render stage 240-330 ms, the game waiting its 100 ms limit, the main thread's longest pump ~95 ms); 16:9
with the picture's targets kept 640 wide (`PS2X_HW_WIDE_SCALE=0`, diagnosis only) median 49.1 / p95 103.3 ms, no such
stall. The desktop reference shows the same direction at a smaller scale (its render worker 1.7 -> 3.6 ms, as upstream
documents: "the render worker roughly doubles with the pixels and the extra draws").

Asked "How should M09 handle the default and the stall?" with the options fix the stall first, 16:9 default with
640-wide targets, keep 4:3 default, or 16:9 default as is, the user chose "16:9 default as is" (record it for M10).

What changes: nothing in M09's tasks. 16:9 with upstream's wide targets is the Switch default (M09-01); the cost and
the stall are recorded in M09-06 and added to **M10-03** (find and fix bottlenecks) as a measured starting point.
