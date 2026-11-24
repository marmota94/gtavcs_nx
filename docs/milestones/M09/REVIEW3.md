# M09 review 3 (a fresh independent reviewer agent, `m09-review3`)

Candidate `c01a8c72a305aa1c9e5e7599d1db09468da6a126` (NRO `af1a582d`). Verdict: **REQUEST_CHANGES**, five low
findings, all in the records; no source change or hardware run needed. The reviewer edited no file (scratch output only
under build/review3-m09/). All review 1 and review 2 findings were found fixed in the committed files.

1. Low - TASKS.md M09-08 cites "scripted switches in the flyover, the cutscene and play (`silent2`, `silent3`)", but both
   runs switch only at 222 / 228 s (cutscene) and 376 / 382 s (play); only the superseded `silent` (b3fc1722) switched in
   the flyover (112 / 118 s, peekf at 115 s `0.700208 1.25 0.1 2000`). Fix: drop the flyover or cite `silent` by name.
2. Low - REPORT.md says "S3 4:3 samples on the two machines fell on different cutscene frames (other FOVs)", which is
   false: `silent2` 225 s and `reference-silent` 259 s both read `0.325031 1.25 0.3 2000`, planes `0.951028 -0.309104`.
   The S3 row cites only the desktop at 4:3; `reference-silent` is in neither the Runs table nor the superseded list;
   TASKS.md M09-02 calls 0.433374 and 0.325031 "one frame", but on the desktop they are 240 s and 259 s. Fix: cite the
   identical pair, remove the sentence, list `reference-silent`.
3. Low - TASKS.md M09-08 cites `cost-169` against `reference-cost-169`, but the "Reference scenes" table has no such pair
   (their `s4-200` is comparable). Fix: add the row or drop the citation.
4. Low - REPORT.md's drive paragraph says the scenery reaches both outer eighths "in every frame"; drive-105 and
   drive-150 are inside a structure, drive-075 a blur in palm trees. Fix: carry TASKS.md's inside-buildings caveat.
5. Low - `hardware/pause-probe.cfg`, `hardware/reference-pause-probe/` (desktop, profile m09-pause-probe) and
   `host-tests-pre/` (77ddfc7, dirty=True) are not indexed anywhere. Fix: one line each, or remove them.

Checks: milestone_gate PASS; validate_project PASS (188 tasks, 17 milestones); unittest 34 OK (1 skipped);
upstream_overlay PASS 13 patches; host tests 8/8 PASS on overlay 00fc91bc, identical to the committed logs; `gh pr checks
10` on c01a8c7: build and test checks pass, milestone-review fails for the missing review.json (expected); manifest
af1a582d from 0f6cc36, root_dirty false; NRO-to-run map and read-back hashes match; shape judge and scopemask re-runs
byte-identical; all 795 screenshot hashes verify; the eight reference pairs, cost-169 and the scopes viewed (pillars
L160 / R160, cutscene bars T88 / B88 on both machines); perf, GPU, target, heap figures recomputed. Notes, not findings:
"gameinfo mode 4 at 395 / 405 s" fits reference-silent2 only (reference-cost-169's scope shots at 215 / 227 s, also
mode 4); cost-169 on the Switch shows no scope either (M09-05 rests on silent3 alone); brief `[present] display aspect
1.778` lines between the boot movies (empty frames); a zero-width texture desc would be counted under {1,h} and released
under {0,h} in the rhi_gl stats map (stats only); the settings host tests rely on load() starting from current values
(production order).
