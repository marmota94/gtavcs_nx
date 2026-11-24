# M09 review 5: findings and answers

Review 5 (independent reviewer agent `m09-review5`, candidate `b1c51c9`, NRO `af1a582d`): **REQUEST_CHANGES**, three low
findings, all in the records ([REVIEW5.md](REVIEW5.md)). M09-06, 07 and 08 were reopened and closed again after the
fixes below. No source change: the candidate NRO stays `af1a582d` (commit `0f6cc36`).

| # | Finding | Answer |
|---|---|---|
| 1 (low) | reference-cost-169 has no gameinfo at 215 / 227 s | REPORT.md "Reference scenes": mode 4 at 395 / 405 s (`reference-silent2`) and at 210 / 220 / 230 s around the 215 / 227 s shots (`reference-cost-169`). |
| 2 (low) | "14 or 15 targets ... whole run" ignored the boot's 1 and 5 | REPORT.md (paragraph and table row "in play"), TASKS.md M09-06 and evidence.json M09-06: in play (from ~10 s) each run logs 14 or 15 targets, the two values given; the boot logs 1 and 5. |
| 3 (low) | The "driven route" is a fixed-height flight | TASKS.md M09-07, REPORT.md (Runs row, section now "Frame edges along the cruise route") and evidence.json say it: upstream's cruise rig, a low flight at a fixed ~35 m over the roofs (`player` z 34.70-35.01 at all 24 samples, upstream docs/PERFORMANCE.md) streaming the cells along the route, not road driving; mid-air over water 240-345 s; drive-210 named with drive-105 / drive-150; drive-225's stunt-jump result noted. |

Notes not acted on: the stall description in REPORT.md / SCOPE_DECISIONS.md stays approximate (M10-03 measures it); the
other notes are observations on the tools and on rendering outside M09.
