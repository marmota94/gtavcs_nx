# M09 review 3: findings and answers

Review 3 (independent reviewer agent `m09-review3`, candidate `c01a8c7`, NRO `af1a582d`): **REQUEST_CHANGES**, five low
findings, all in the records ([REVIEW3.md](REVIEW3.md)). M09-02, 07 and 08 were reopened and closed again after the
fixes below. No source change: the candidate NRO stays `af1a582d` (commit `0f6cc36`).

| # | Finding | Answer |
|---|---|---|
| 1 (low) | Flyover switch claimed for silent2 / silent3 | TASKS.md M09-08 now gives their switch times (222 / 228 s cutscene, 376 / 382 s play) and names the superseded `silent` (b3fc1722, 112 / 118 s, peekf `0.700208 1.25 0.1 2000` at 115 s) as the only flyover switch. |
| 2 (low) | The S3 4:3 pair described wrongly; reference-silent unlisted | REPORT.md's M09-02 table: S3 at 16:9 `0.433374` (silent3 231 s, reference-silent 240 s) and at 4:3 `0.325031 1.25 0.3 2000`, planes `0.951028 -0.309104` identical on both (silent2 225 s, reference-silent 259 s), ratio 1.33333; the wrong sentence removed; `reference-silent` in the Runs table. TASKS.md M09-02 no longer says "one frame". |
| 3 (low) | cost-169 / reference-cost-169 pair cited, not described | Looked at side by side and added to "Reference scenes": the same framing, HUD (keep 4:3) and radar placement; the Switch picture's REC mark is the runtime's perf.csv recording (`PS2X_PERF_LOG`). |
| 4 (low) | Drive "in every frame" | REPORT.md carries the caveat: some frames inside a structure (drive-105, drive-150) or a blur in palm trees (drive-075). |
| 5 (low) | Unindexed files | REPORT.md lists `pause-probe.cfg` / `reference-pause-probe/` (the desktop run that found how to open the pause menu from a run file, setting the shape runs' Start hold) and `host-tests-pre/` (main's 77ddfc7 before any M09 change, dirty=True, a baseline only). |

Note taken: the reference scope's camera mode is now given for both references (395 / 405 s and 215 / 227 s, mode 4).
