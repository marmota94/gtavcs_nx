# M09 review 4 (a fresh independent reviewer agent, `m09-review4`)

Candidate `f1abca26a82994b63c3dfc8c53c4813b8034d5e3` (NRO `af1a582d`). Verdict: **REQUEST_CHANGES**, three low
findings, all in the records; no source change or hardware run needed. The reviewer edited no file (scratch output only
under build/review4-m09/). Reviews 1-3 resolved except review 3 finding 4 in evidence.json (finding 1).

1. Low - evidence.json M09-07 still says "Edges of every drive frame reach buildings, trees, roads, water and skyline";
   REPORT.md now carries the caveat (drive-105, drive-150 inside a structure, drive-075 a blur in palm trees). Fix: drop
   "every", add the caveat.
2. Low - the drive's speed and time are misstated (TASKS.md M09-07 "at 40 m/s for 360 s", REPORT.md Runs row and drive
   paragraph, evidence.json): the cruise starts at t=44 s, `cruise status` reads 334.3 m at 60 s and 5,335.8 m at 360 s,
   5,002 m in 300 s, about 16.7 m/s; 40 m/s is only the speed the run file asks for. Fix: commanded 40 m/s; 334 m at
   60 s -> 5,336 m at 360 s (about 16.7 m/s average).
3. Low - `cost-169` (launched 11:47:57) and `reference-cost-169` ran the run file before 75a65ac (11:52:46), which
   added TERMINATE to its scope scripts: both log `scm 0601cfa5071e0732` / `scm 0601cfa507270732`, so neither shows a
   scope (the 80-200 s cost window is not affected). REPORT.md notes this for silent2 and the desktop references, not
   for the Switch's cost-169. Fix: one line in the Runs table.

Checks: milestone_gate PASS; validate_project PASS (188 tasks, 17 milestones); unittest 34 OK (1 skipped);
upstream_overlay PASS 13 patches, overlay files byte-identical to build/m04/game-source-00fc91bc; host tests 8/8 PASS,
identical to the committed logs; `gh pr checks 10` on f1abca2: build and test checks pass, milestone-review fails for the
missing review.json (expected); manifest af1a582d from 0f6cc36, root_dirty false; `git diff 0f6cc36 HEAD -- src patches
tests tools cmake CMakeLists.txt` empty; run-to-NRO map; perf / GPU / heap / target figures recomputed; every M09-02
value found in the logs; shapes and scopemask re-runs byte-identical; 795 screenshot hashes verify; reference pairs,
masks, text HUD modes, drive, relaunch / accept2 / presets pictures viewed. Notes, not findings: transcript.txt has no
`text` run (session-text.txt has it); the af1a582d screenshots are hashed in transcript.txt, not screenshots-sha256.txt;
evidence.json M09-06 lacks REPORT's "whole run" qualifier for the target ranges; drive-240's purple texture over water;
cost-169n's GPU mean (M10-03); the zero-width key in the rhi_gl stats map (stats only); the Enhanced preset's help text
still says "in the window's shape" on the Switch (cosmetic); the reference-cost-169 150 s plane sample, as REPORT explains.
