# M09 review 2: findings and answers

Review 2 (independent reviewer agent `m09-review2`, candidate `8db8844`, NRO `af1a582d`): **REQUEST_CHANGES**, four
findings, all in the records ([REVIEW2.md](REVIEW2.md)). M09-04, 06 and 08 were reopened (`3b235ae`) and closed again
after the fixes below. No source change: the candidate NRO stays `af1a582d` (commit `0f6cc36`).

| # | Finding | Answer |
|---|---|---|
| 1 (medium) | M09-08 compared the scope with a desktop reference that showed none; REPORT.md had no per-scene comparison | The eight `silent3` / `reference-silent2` pairs were put side by side at half size and looked at; REPORT.md "Reference scenes (M09-08)" says what each shows: Fort Baxter with the three HUD modes, 4:3 pillarboxed and back at 16:9 match in framing, HUD and radar placement; the flyover and cutscene pairs fell on different frames and compare only the picture's shape and the bars. The scope is dropped from the desktop comparison and the reason stated (the references' scope shots: scopemask 255 / 63 and 255 / 61, re-run here); it rests on M09-05's strip test. TASKS.md M09-08 and evidence.json now say so. |
| 2 (low) | shape2's 97th first mismatch unexplained | TASKS.md and REPORT.md: the old rule (a menu-flagged frame without 3D must be 4:3) also failed frame 0011, the menu's first batch, which still showed the game at 16:9; shape3's 0018 is the same case. |
| 3 (low) | "-" for slow frames at 4:3 and in cost-169n | Recomputed from perf.csv over 80-200 s: 6 of 4,169 (to 405 ms) at 4:3, 131 of 1,288 (to 456 ms) at 16:9, 9 of 1,928 (to 407 ms) with 640-wide targets. |
| 4 (low) | Runs table omitted the af1a582d runs | Row added for `presets-r1` and `presets-env`. |

Notes taken: AGENTS.md's M09 paragraph now names the 2026-11-24 review fixes. Not changed: m04_reference.py's vcs.ini
left behind if Popen raises (private folder only); the applyPreset fallback has no host test (anonymous namespace in
main.cpp; covered on hardware by presets-env / presets-r1).
