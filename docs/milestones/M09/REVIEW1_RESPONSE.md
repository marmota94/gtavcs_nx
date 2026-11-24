# M09 review 1: findings and answers

Review 1 (independent reviewer agent `m09-review1`, candidate `ee4705b`, NRO `99472722`): **REQUEST_CHANGES**, eight
findings ([REVIEW1.md](REVIEW1.md)). M09-01, 04, 05, 06 and 07 were reopened in the ledger (`a65e47f`) and closed again
after the fixes below. The candidate NRO is now `af1a582d` (commit `0f6cc36`, clean); see [TASKS.md](TASKS.md)
"Candidate NRO" for why the earlier runs stand.

| # | Finding | Answer |
|---|---|---|
| 1 (medium) | shape3's frame counts were shape2's | TASKS.md M09-04, REPORT.md and evidence.json give shape3's own counts from its committed shapes files: entering 31 game + 89 menu, leaving 13 menu + 107 game, 0 mismatched; 24/96 and 12/108 only for shape2. |
| 2 (medium) | shape2's original FAIL not disclosed | TASKS.md M09-04 and REPORT.md now say shape2 first failed (`session-shape2.txt`: 97 and 12 mismatched, `RESULT game FAIL`) because that judge compared each picture with the 16:9 `frame` rectangle, so every 4:3 menu frame failed; the judge changed in 92357a4 to the `aspect` the frame is shown at, and the committed shape2 files are its re-check of the same PNGs. |
| 3 (medium) | AGENTS.md status stale | Current deliverable is M09 awaiting review, M08 listed as merged; an M09 paragraph in the verification status (16:9 default, 4:3 kept, cost and stall to M10-03, docked to M16-03). |
| 4 (low) | A preset logged "not offered here" for a key locked by its environment variable | Patch 0013: `applyPreset` takes the host-default fallback only when `!settings::overridden(p.key)` (`b0c40c3`). Proven on af1a582d: `presets-env` (`VCS_ASPECT=43`): Enhanced, Widescreen, light and Authentic leave the aspect at 43 and log no "not offered here" line; `presets-r1` repeats `presets` with the same results as on 99472722 (169 / 43 / 169). |
| 5 (low) | M10-03 named 853x448 | Now "largest 853x1472, against 640x1472 at 4:3", the measured `[rhi-stats] targets` size. |
| 6 (low) | 4:3 target range not from 80-200 s | REPORT.md and TASKS.md say the ranges are the two values each run logged over the whole run (14 or 15 targets), and that inside 80-200 s `cost-43` logged only 17.6 MiB. |
| 7 (low) | Scope-mask figures had no committed source | `tools/switch_m09_session.py scopemask` runs upstream's step_scopemask strip test; `hardware/scopemask-results.txt` holds its output with each screenshot's sha256 (silent3 sniper / camera 2 / 2, silent2 without the scope 255 / 47); evidence.json M09-05 cites it. |
| 8 (low) | "No late pop-in" overclaimed | Dropped: the drive stills (15 s apart) show what is drawn at the edges, not whether it appeared late; TASKS.md and REPORT.md say pop-in is not claimed. |

The reviewer's notes (cost-169n's higher GPU mean without the stall; drive-240.png's purple texture at the left edge
while the rig moves the car over water) are left for M10-03 and the cruise rig, as noted in the review.
