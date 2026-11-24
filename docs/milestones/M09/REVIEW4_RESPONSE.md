# M09 review 4: findings and answers

Review 4 (independent reviewer agent `m09-review4`, candidate `f1abca2`, NRO `af1a582d`): **REQUEST_CHANGES**, three low
findings, all in the records ([REVIEW4.md](REVIEW4.md)). M09-06 and M09-07 were reopened and closed again after the
fixes below. No source change: the candidate NRO stays `af1a582d` (commit `0f6cc36`).

| # | Finding | Answer |
|---|---|---|
| 1 (low) | evidence.json still said "every drive frame" | The M09-07 entry now carries REPORT.md's caveat (drive-105, drive-150 inside a structure, drive-075 a blur in palm trees) and says pop-in is not claimed. |
| 2 (low) | Drive speed and time misstated | TASKS.md, REPORT.md (Runs row and drive paragraph) and evidence.json: commanded 40 m/s from 44 s; `cruise status` 334 m at 60 s -> 5,336 m at 360 s, about 16.7 m/s average (`drive-results.txt`). |
| 3 (low) | cost-169 ran the pre-TERMINATE schedule | REPORT.md's Runs table: `cost-169` and `reference-cost-169` ran `m09-cost-169.cfg` before 75a65ac (their logs: `scm 0601cfa5071e0732`), no scope on either; the 80-200 s cost window is not affected. `cost-43`, `cost-169n` and `reference-cost-43` have no scope commands. |

Notes taken: the af1a582d screenshots (presets-r1, presets-env) are added to `hardware/screenshots-sha256.txt` (800
entries, all verify); evidence.json M09-06 says the target ranges are over the whole run.
