# M09 review 1 (a fresh independent reviewer agent, `m09-review1`)

Candidate `ee4705bb904f537f9a0a7d8637529bcc629d45bf` (NRO `99472722`). Verdict: **REQUEST_CHANGES**, eight findings:
seven in the records, one in patch 0013's log line. The reviewer edited no file (scratch output only under
build/review1-m09/). The implementation and every task's substance were found to hold against the committed logs.

1. Medium - shape3's frame counts are shape2's. TASKS.md M09-04, REPORT.md "Frame shapes" and evidence.json give
   24 + 96 entering and 12 + 108 leaving for `shape3` (99472722); the committed `hardware/shape3-burst-in-shapes.txt` /
   `-out-shapes.txt` give 31 game + 89 menu entering and 13 menu + 107 game leaving. 24/96 and 12/108 are `shape2`'s
   (b3fc1722). 0 mismatched holds for both. Fix: shape3's own counts; 24/96, 12/108 only for shape2.
2. Medium - shape2's original FAIL is not disclosed. `session-shape2.txt` records `burst-in frames=120 mismatched=97`,
   `burst-out ... mismatched=12`, `RESULT game FAIL`; the committed shape2 shapes files are the re-check by the judge
   changed in 92357a4 (which compared the picture with the 16:9 frame rectangle instead of the aspect it is shown at).
   The change is justified (the reviewer re-ran the judge on the private bursts: byte-identical output, PNG hashes
   match the session log), but REPORT.md / TASKS.md present shape2 as passing without the FAIL ("never erase failing
   proof"). Fix: one sentence: shape2 failed under the old judge, why that judge was wrong, the committed files are
   the 92357a4 re-check of the same PNGs.
3. Medium - AGENTS.md is stale: "Current deliverable (M08 ... awaiting independent review)" although M08 is merged, and
   no M09 paragraph in the verification status. Fix: M09 as the current deliverable, a short M09 status paragraph.
4. Low - patch 0013's `applyPreset` fallback treats every refused `settings::set` as a value not offered; `set` also
   refuses a key locked by its environment variable (`overridden`, e.g. `VCS_ASPECT`), which then logs "not offered
   here, N instead" and the fallback `set` is refused too (desktop as well; behaviour unchanged, the log wrong). Fix:
   skip the fallback when `settings::overridden(p.key)`, or check the value against `s.choices`.
5. Low - MILESTONES.md M10-03 says "wide picture targets (853x448)"; the measured largest target is 853x1472. Fix: the
   measured size.
6. Low - REPORT.md / TASKS.md give 17.6-21.1 MiB at 4:3 "over 80-200 s"; inside 80-200 s cost-43 logs only 17.6 MiB,
   the 21.1 lines fall outside the span. Fix: state the ranges as the whole run, or recompute over 80-200 s.
7. Low - M09-05's scope-mask figures (2/2, 255/47) have no committed source, only evidence.json text (the reviewer
   reproduced them exactly with upstream's `step_scopemask` strip test on the private PNGs). Fix: commit a result file.
8. Low - M09-07's "no late pop-in at the edges seen" overclaims: stills 15 s apart cannot show pop-in. Fix: drop or
   qualify, or add a driving burst.

Checks: milestone_gate PASS; validate_project PASS (188 tasks, 17 milestones); unittest 34 OK (1 skipped);
upstream_overlay PASS 13 patches (unchanged overlay 329e323a); host tests not re-run (Docker engine not running; the
committed host-tests from 178f68a, dirty=False, all PASS, tests/core unchanged since); `gh pr checks 10`: build and test
checks pass, milestone-review fails for the missing review.json (expected); manifest NRO 99472722 from 178f68a,
root_dirty false, later non-doc change only tests/switch/m09-text.cfg; quoted log lines found; frame median / p95, GPU
means, heap and the 131 / 1,288 slow frames recomputed from the committed CSVs and logs; screenshots agree with the
descriptions; the 2026-11-19 docked deferral and the 2026-11-21 16:9 decision recorded. Notes, not findings: cost-169n
has a higher GPU mean per frame (34.20 ms) than cost-169 (21.28 ms) yet no stall (for M10-03); drive-240.png shows an
odd purple texture at the left edge while the rig moves the car over water (probably the cruise passing through
geometry, as the report says).
