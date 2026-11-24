# M09 review 2 (a fresh independent reviewer agent, `m09-review2`)

Candidate `8db88445ef67473c4363ac2f7c9cb8b4cee1f73a` (NRO `af1a582d`). Verdict: **REQUEST_CHANGES**, one medium and
three low findings, all in the records. The reviewer edited no file (scratch output only under build/review2-m09/). All
eight review 1 findings were found resolved in the committed files; the implementation and every task's substance hold.

1. Medium - M09-08 (TASKS.md) lists "the scope" among the scenes compared with the desktop, but the desktop reference
   never showed one: `reference-silent2` ran the pre-TERMINATE schedule (gameinfo camera mode 4 at at=395 / 405, where
   silent3 has 7 / 47), its two scope screenshots are byte-identical (`3c5abd7b`), as are `reference-cost-169`'s
   (`4c782ed4`), and the scopemask test on them gives 255 / 63 and 255 / 61 (no scope). evidence.json M09-08 cites
   REPORT.md for "same framing and HUD placement by description", but REPORT.md has no such section. Fix: drop the scope
   from the desktop comparison (or state the reference showed none; the scope rests on M09-05's strip test) and add a
   short per-scene comparison to REPORT.md naming the pairs looked at.
2. Low - "so every 4:3 menu frame failed" explains 96 of shape2's 97 first mismatches: frame 0011 (aspect 1.778, shows
   the game, `3d 0 menu 1`, the menu's first batch) failed the old rule's `menu and not drew3d and frame > 1.34`. Fix:
   one clause for it.
3. Low - REPORT.md's "Game frames over 250 ms" gives "-" for 4:3 and cost-169n; perf.csv over 80-200 s has 6 of 4,169
   (to 405 ms) and 9 of 1,928 (to 407 ms). Fix: the counts.
4. Low - REPORT.md's Runs table omits `presets-r1` and `presets-env` on the candidate NRO af1a582d. Fix: one row.

Checks: milestone_gate PASS; validate_project PASS; unittest 34 OK (1 skipped); upstream_overlay PASS 13 patches; host
tests 8/8 PASS (overlay 00fc91bc, as committed from d9522d9); scopemask reproduces scopemask-results.txt; shapes
re-run byte-identical, 0 mismatched; manifest af1a582d from 0f6cc36, root_dirty false; transcript NRO lines map runs to
NROs as TASKS.md says; the runs with `env VCS_ASPECT` pressed no preset; all 795 screenshots-sha256.txt entries verify;
`gh pr checks 10`: build and test checks pass, milestone-review fails for the missing review.json (expected). Notes, not
findings: m04_reference.py `--setting` leaves its vcs.ini if Popen raises with none before (private folder); the AGENTS.md
M09 paragraph is dated 2026-11-21 while the af1a582d runs are 2026-11-24; the applyPreset fallback has no host test
(anonymous namespace in main.cpp), covered on hardware by presets-env / presets-r1.
