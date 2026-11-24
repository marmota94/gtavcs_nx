# M09 review 5 (a fresh independent reviewer agent, `m09-review5`)

Candidate `b1c51c925d8b4d9e0084f541ff7af38837455ba6` (NRO `af1a582d`). Verdict: **REQUEST_CHANGES**, three low
findings, all in the records; no source change or hardware run needed. The reviewer edited no file (scratch output only
under build/review5-m09/). All review 1-4 findings resolved; TASKS.md, REPORT.md and evidence.json agree.

1. Low - REPORT.md "Reference scenes" gives the desktop references' "gameinfo mode 4 at their scope times, 395 / 405 s
   and 215 / 227 s"; `reference-cost-169` has no gameinfo at 215 / 227 s (its screenshot times); its samples at
   210 / 220 / 230 s are mode 4. Fix: mode 4 at 395 / 405 s (reference-silent2), at 210 / 220 / 230 s around the
   215 / 227 s shots (reference-cost-169).
2. Low - "each run logs 14 or 15 targets ... (whole run)" (REPORT.md, TASKS.md M09-06, evidence.json M09-06): each cost
   run also logs 1 and 5 targets in its first 10 s (cost-43 2.6 / 11.4 MiB, cost-169 3.5 / 13.4, cost-169n 3.5 / 11.4).
   Fix: in play (from ~10 s) each run logs 14 or 15 targets; the boot logs 1 and 5.
3. Low - M09-07's "driven route" is upstream's cruise rig, a low flight at a fixed ~35 m over the roofs (upstream
   docs/PERFORMANCE.md; `player` z 34.70-35.01 at all 24 samples), mid-air over water 240-345 s, inside a building face
   at drive-210, a stunt-jump result shown at drive-225; the records read as road driving and name only drive-105 /
   drive-150 inside a structure. Fix: say so; not road driving; name drive-210 or write "several frames".

Checks: milestone_gate PASS; validate_project PASS (188 tasks, 17 milestones); unittest 34 OK (1 skipped);
upstream_overlay PASS 13 patches; host tests 8/8 PASS on overlay 00fc91bc; `gh pr checks 10` on b1c51c9: build and test
checks pass, milestone-review fails for the missing review.json (expected); manifest af1a582d from 0f6cc36, root_dirty
false, the private NRO hashes to af1a582d; `git diff 0f6cc36 HEAD -- src patches tests tools cmake CMakeLists.txt`
empty; run-to-NRO map; presets pressed only in accept2 / presets / presets-r1 / presets-env, VCS_ASPECT only in cost-*,
wide, presets-env; perf / GPU / heap / target / desktop figures recomputed; every M09-02 value found; quoted lines found;
shapes and scopemask re-runs byte-identical; 800 screenshot hashes verify; reference pairs, masks, text HUD modes and all
24 drive frames viewed. Notes, not findings: the stall description (render 240-330 ms, pump ~95 ms) is approximate
(frames over 350 ms: render_ms 197-335, host_max_ms median 97); the shapes judge is partly self-referential (the
runtime's aspect plus a content test); scopemask cannot tell a 4:3 pillarbox from a mask (silent3 was at 16:9 then); the
per-frame `3d` flag is not a reliable 3D indicator; earlier notes carried over; GL / D3D11 lighting and colour differ
(outside M09).
