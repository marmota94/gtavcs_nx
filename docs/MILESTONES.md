# Milestone execution contract

This is the authoritative task ledger. Read with [PORT_DISCOVERY.md](PORT_DISCOVERY.md),
[PORTING_CHECKLIST.md](PORTING_CHECKLIST.md), [AUTOMATED_EXECUTION.md](AUTOMATED_EXECUTION.md)
and root [AGENTS.md](../AGENTS.md). M00 establishes the plan and inventories;
it does not validate a Switch executable. Every later milestone requires its
own implementation AND observed acceptance evidence.

## Mandatory workflow

1. Exactly one active milestone branch, cut from the latest reviewed `main`.
   Do not pre-create later implementation branches or start their tasks early.
2. Tasks start unchecked. Work in dependency order. A checkbox means its stated
   result was observed and a repository evidence record exists, not merely coded.
   If required hardware/tooling is missing, keep the task unchecked and report it.
3. Store each task's evidence in `docs/milestones/Mxx/evidence.json`: task ID ->
   nonempty list of `{ "path": "repository/file", "result": "observed result" }`.
   Include command, source/target commit, environment, expected/observed outcome,
   log location and limitations in the referenced report. URLs alone are insufficient.
4. Commit implementation and evidence. Run local checks and CI for that candidate.
   Only when EVERY task is complete and `python tools/milestone_gate.py Mxx`
   passes may an independent reviewer be created. No reviewer during unfinished
   implementation, no premature partial review and no self-review substitute.
5. Create a fresh reviewer agent, separate from implementation, read-only initially.
   Give exact candidate SHA, milestone scope, task ledger, evidence and commands.
   Reviewer independently checks all tasks, reproduces feasible tests, examines
   material limits, and returns `APPROVE` or `REQUEST_CHANGES` with file/line findings.
   Hardware claims require hardware evidence; an agent cannot invent a console run.
6. Any finding reopens its task; fix on the SAME branch. No merge with unresolved
   findings, including low-severity ones. Once every task is done again, request
   independent re-review of the new SHA. Do not start another milestone meanwhile.
7. Record `docs/milestones/Mxx/review.json` with milestone, exact candidate_commit,
   verdict, reviewer_id, independent=true, unresolved_findings=[], and substantive
   checks/limitations. The reviewer report is the only allowed post-review change.
   Any other change, even evidence/task edits, invalidates the approval.
8. Run `python tools/milestone_gate.py Mxx --require-review`, wait for all required
   CI checks, merge the PR with a merge commit, verify remote `main`, then cut/push
   the next milestone branch from that merged main. Never merge just to save work.
9. Repository visibility remains private. No ISO, executable, extracted assets,
   generated game C++, credentials or user saves in Git/CI artifacts. Public
   upstream source is a pinned submodule; port edits use maintained overlay patches.

GitHub controls are supplementary: see M00 evidence for actual account support.
Local/CI structural checks validate records, not the truth of behavioral claims.
The independent reviewer must examine the underlying evidence. No arbitrary task
waivers, "N/A", reduced scope or performance exceptions to force completion;
scope changes require the user's explicit decision and a revised plan.

## Dependency map and acceptance budgets

Milestone numbers follow the execution order. They were renumbered on 2026-11-11
by user decision. Records written before then (M00-M03 evidence, M02 audit JSON,
`tools/audit_core_coverage.py`) keep the old IDs; translate them with this table:

| Old ID | New ID | Milestone |
|---|---|---|
| M08 | M07 | Real sound, music and movies |
| M09 | M08 | Developer menu on L3+R3 |
| M12 | M09 | Widescreen |
| M10 | M10 | Stability and performance |
| M13 | M11 | Custom textures |
| M14 | M12 | Custom audio |
| M07 | M13 | Complete controllers |
| M15 | M14 | Whole world and saves |
| M11 | M15 | Final release |

M00-M06 kept their numbers. Task numbers inside a milestone did not change.

Execution order:

```text
BETA TRACK   M00 -> M01 -> M02 -> M03 -> M04 -> M05 -> M06 = BETA
POST-BETA    M07 -> M08 -> M09 -> M10
AFTER PERF   M11 -> M12 -> M13 -> M14 -> M16 -> M15
```

M16 (Pro Controller and docked) was added by the user's decision of 2026-11-19: once
every handheld milestone is done, one milestone tests the Pro Controller and docked
mode, before the final release. Docked and Pro Controller parts of M09-M14 go to it
(docs/milestones/M08/SCOPE_DECISIONS.md).

Order by the user's decision of 2026-11-11, replacing the 2026-11-09 post-beta
order: (1) picture through a new game = beta, (2) sound, (3) dev menu then
widescreen, (4) performance; every other milestone follows performance.
See BETA_ROADMAP.md, "Scope decision (user, 2026-11-11)".

Each milestone also reruns relevant proof from earlier milestones. A discovered
prerequisite belongs in the active milestone before review, with a new unique ID.
See [BETA_ROADMAP.md](BETA_ROADMAP.md) for the step-by-step route.

Beta, by the user's decision of 2026-11-09, means the real game opens on a real
Switch, goes ingame and loads the first scene: from an empty test card and with
no ISO at runtime it reaches gameplay at Fort Baxter with the 3D world and HUD
visible and the player controllable, then runs 5 continuous minutes and exits
cleanly, in handheld and docked. M06-18 is the acceptance run. Beta has no FPS
target, but timing is measured and reported. It does not claim audible sound,
movies, saves, the dev menu, widescreen or whole-map coverage.

The final release adds audio, controller-only menus, the dev menu, safe saves,
widescreen, custom textures and audio, internal render scaling and a measured
25+ FPS baseline at stock clocks. Output 720p/1080p does not imply rendering all
scenes internally at those sizes. Report the exact internal scale with every
measurement; native internal-resolution 25+ FPS is a separately measured goal,
not assumed. Never tune guest time or skip game work to inflate a frame counter.

## M00 — Private repository, exhaustive discovery and execution plan

Branch: `milestone/m00-foundation` <!-- M00 -->

Entry: initial scaffold exists, no target compilation/gameplay evidence.
Exit: a private, traceable repository and executable development governance;
all discoverable scoped metadata catalogued with explicit unknowns and next tasks.

- [x] M00-01 Create private GitHub repo, verify owner/visibility/default main, configure merge-commit-only milestone flow.
- [x] M00-02 Establish bootstrap main and isolated M00 branch; register upstream as a pinned submodule and preserve its clean tree.
- [x] M00-03 Add ignore/attribute/license policy and verify no game data, generated game code or credentials are tracked.
- [x] M00-04 Preserve the initial SDL2/GL scaffold and label its unbuilt, silent, stub-only status accurately.
- [x] M00-05 Inspect startup/platform/RHI/runtime/recompiler ownership and document exact entry functions and required replacements.
- [x] M00-06 Generate complete metadata inventories within declared source scope: files, parsed definitions/declarators, includes, build declarations and parser gaps.
- [x] M00-07 Inventory all supplied guest function-map rows, deduplicated addresses, SDK macro calls, recompiler configuration and VU uploads without game bytes.
- [x] M00-08 Verify local disc identity and catalogue every ISO9660 file/directory, size and LBA; classify formats and unresolved archive members.
- [x] M00-09 Map every subsystem, dependency and portability candidate family to owning files, milestone tasks, validation and unknowns.
- [x] M00-10 Evaluate REA need; document source-first decision and exact escalation workflow for unresolved binary/runtime questions.
- [x] M00-11 Write exhaustive ordered milestones with tasks, entry/exit criteria, commands, evidence, recovery and independent-review/merge gates.
- [x] M00-12 Implement and test fail-closed all-task/evidence review eligibility and exact-candidate approval checks; test incomplete and stale approval rejection.
- [x] M00-13 Add source-only foundation CI, milestone PR gate and packaging exclusions; exercise local checks and remote foundation CI.
- [x] M00-14 Attempt GitHub main protection; record actual result and any plan limitation without claiming unavailable enforcement.
- [x] M00-15 Record M00 verification, environment blockers and reproducibility limits; verify the final candidate is clean and eligible before creating its independent reviewer.

Commands: discovery and foundation commands in AUTOMATED_EXECUTION; then review
eligibility gate. Evidence: inventories, discovery narrative, M00 verification,
CI run and repository settings report. No target build required for this milestone.

## M01 — Reproducible toolchain and platform smoke NRO

Branch: `milestone/m01-platform-smoke` <!-- M01 -->

Entry: reviewed M00 main. Exit: clean-machine reproducible platform NRO and actual
Switch clear-screen/input/audio-device/shutdown proof; no game claim. By explicit
user decision on 2026-11-08, docking acceptance is deferred to the first real
game boot that loads the main menu (M06-13). The user subsequently accepted the
supplied Joy-Con tests for M01 and deferred the Pro Controller/formal physical
connection-state matrix to the same trigger (M06-14). Neither deferred test is
claimed passed; see docs/milestones/M01/SCOPE_DECISIONS.md.

- [x] M01-01 Restore usable WSL Ubuntu/Docker Linux engine and record host versions/resources without changing unrelated projects.
- [x] M01-02 Pin devkitA64 image digest and package versions, official Switch CMake, libnx, SDL2, Mesa and cross pkg-config; save lock manifest.
- [x] M01-03 Create noninteractive Dockerfile/build command and cache strategy; fail on unavailable dependencies or accidental host libraries.
- [x] M01-04 Build scaffold from a fresh checkout/submodule init with all generated GLAD dependencies present; inspect compiler/include/library paths.
- [x] M01-05 Inspect AArch64 ELF, PIE/relocations/map and NACP/NRO; demonstrate incremental and clean rebuild plus failed-build artifact rejection.
- [x] M01-06 Stage only NRO under switch/gtavcs_nx, verify missing asset-root failure and successful checked chdir; provide visible persistent diagnostics.
- [x] M01-07 Boot on an actual Switch with full application memory; capture firmware/libnx/driver versions and GL 3.3 context/clear screen.
- [x] M01-08 Record the supplied attached/wireless Joy-Con input, disconnect/reattach and orderly Plus-exit tests as working per user confirmation; retain Pro Controller and formal physical connection-state verification as unchecked M06-14 under the user's explicit main-menu deferral.
- [x] M01-09 Verify SDL2 opens audren-backed audio, streams bounded silence and closes it cleanly; record buffer/queue behavior.
- [x] M01-10 Record the user-approved deferral of docking acceptance to first real game boot/main-menu loading, preserve the unchecked hardware requirement as M06-13 and revise M01 exit/evidence without claiming a docking pass.
- [x] M01-11 Exercise HOME/focus/suspend/resume and failure cleanup; eliminate invalid stream/context use and runaway loops.
- [x] M01-12 Add source-only cross-build CI/artifact retention with game data excluded; retain reproducible supplied hardware logs, verify fixes for in-scope M01 failures, and explicitly transfer user-deferred controller/docking acceptance to M06-13/M06-14 without claiming those tests passed.

Proof: cross-build logs, package lock, ELF/NRO inspection, console logs/video,
controller/device matrix and suspend observations. Docking proof is deferred to
M06-13 and the Pro/formal connection-state matrix to M06-14 by the recorded user
scope decisions. Emulator-only results are
supplementary and cannot close required hardware tasks.

## M02 — Host generator and ARM64 EE/VU foundation

Branch: `milestone/m02-arm64-core` <!-- M02 -->

Exit: reproducible private generated EE/VU code compiles, links and registers on
ARM64, and the real runtime executes validated CPU, scheduler and synthetic guest
workloads there with guest semantics preserved. Generated game code first executes
in M04.

- [x] M02-01 Capture a desktop reference baseline from the verified disc, isolated saves and recorded source/flags; preserve upstream's reference path.
- [x] M02-02 Build recompiler/analyzer as host tools separately from target code; prove generator does not try to execute AArch64 utilities on the host.
- [x] M02-03 Run upstream recompile.py on the exact supported ELF; record stamp, generated registration count and all missing/unresolved functions.
- [x] M02-04 Audit generated constructs and compiler compatibility; decide documented GCC-compatible adaptation or compatible Clang cross setup without masking errors.
- [x] M02-05 Classify every x86 SIMD candidate by operation/owner; port guest 128-bit memory/register representation and unaligned accesses.
- [x] M02-06 Port packed EE arithmetic, saturation, shifts, shuffles, comparisons and bit casts with edge-case/reference differential evidence.
- [x] M02-07 Port VU interpreter/compiled operations, rounding, exceptional floats and conversion semantics; retain FP contraction prohibition.
- [x] M02-08 Verify PS2_GUEST_CC declarations, function tables, hook wrappers, stack alignment, tail/yield/resume behavior and guest return-address integrity.
- [x] M02-09 Verify ARM char signedness, endian/layout, aliasing, atomics, TLS, allocation alignment and exceptions/unwind assumptions.
- [x] M02-10 Adapt runtime build flags and disable unsupported x86 JIT/desktop backends explicitly; never silently use incorrect fallback math.
- [x] M02-11 Resolve parser gaps affecting compiled core with compiler/preprocessed evidence; catalogue macro-generated SDK call registrations.
- [x] M02-12 Run relevant existing EE/VU/memory/kernel tests plus differential suites on host and ARM64; retain failures and final passing outputs.
- [x] M02-13 Establish memory/stack/worker budgets under full application mode and compile a null-renderer core harness with no desktop dependencies.
- [x] M02-14 Demonstrate deterministic scheduler/guest execution progress and clean stop on target; keep generated code/ELF entirely out of Git/CI.
- [x] M02-15 Establish the required root-owned ordered patch application/reversal tool against the pinned upstream, using disposable build checkouts; prove new-file round trips, idempotence and refusal to overwrite unowned edits or accept altered patches.

Proof: per-operation comparison, toolchain decision, generated identity/register
metadata, native test logs, target trace. Compilation alone does not close execution.
Detailed work/proof tracking: [M02 execution checklist](milestones/M02/TASKS.md).
Required unchecked substeps keep their owning parent task open.

## M03 — Extracted assets, sector VFS and persistent paths

Branch: `milestone/m03-folder-vfs` <!-- M03 -->

Exit: gamefolder-only runtime reads preserve required ISO semantics; assets,
mods, settings and saves resolve to the intended roots without a runtime ISO.

- [x] M03-01 Build an explicit user-disc extraction tool that verifies identity and preserves original path names and extent metadata.
- [x] M03-02 Enumerate internal archive/directory formats (IMG/DIR, DTZ/GTAG, LVZ/WRLD, XTX, PSP frontend, SDT/VB/RAW, SCM, GXT, PSS); classify what remains guest-handled.
- [x] M03-03 Produce private content-hash manifest and metadata-only coverage map; document unsupported members and resolve each boot-critical format gap.
- [x] M03-04 Implement IPS2DiscImage folder backend for findFile, readBytes, totalSectors and listDirectory with deterministic directory metadata.
- [x] M03-05 Normalize cdrom0 prefixes, slash/case and ISO version suffixes; reject path escapes, ambiguous names and invalid sizes/offsets.
- [x] M03-06 Match original LBA mapping and reads crossing files/sectors; identify raw reads into padding/gaps and preserve actually consumed bytes privately.
- [x] M03-07 Implement thread-safe random access and async CDVD/IOP streaming completion/cancellation; avoid shared seek-pointer races.
- [x] M03-08 Differentially compare boot, world streaming, radio, cutscene and unaligned boundary reads against source ISO. Live BEACH/MALL level loads moved to M14-02 by user decision 2026-11-12 (M03/SCOPE_DECISIONS.md).
- [x] M03-09 Remove runtime ISO discovery/staging dependency; resolve required ELF/data staging under gamefolder without shipping it in the NRO. Switch boot-path use moved to M04-13 by user decision 2026-11-12 (M03/SCOPE_DECISIONS.md).
- [x] M03-10 Route configuration/cache/test saves separately under gamefolder/userdata; preserve memory-card directory/file metadata and backups. Runtime use of these paths with Switch proof moved to M04-13 by user decision 2026-11-12 (M03/SCOPE_DECISIONS.md).
- [x] M03-11 Define supported mod overlay precedence/root validation and verify missing/corrupt assets produce actionable logs with safe failure.
- [x] M03-12 Verify SD case/path behavior, bounded file-handle/cache use and clean shutdown during pending reads on hardware.
- [x] M03-13 Pass only rooted paths into runtime std::filesystem code (narrowed from rooted or cwd-relative by the sixth review, 2026-11-12) and test that a device-prefixed `sdmc:` value cannot produce a cwd-doubled path (M02 hardware finding).

Proof: extractor validation, metadata coverage, sampled and recorded-trace byte
comparisons, no-ISO runtime test, corrupt/missing asset and cancellation tests.

## M04 — SDL2 platform adapter and real runtime boot

Branch: `milestone/m04-runtime-boot` <!-- M04 -->

Exit: genuine recompiled runtime boots and advances through its init/front-end
states with null rendering and diagnostic evidence; stub is no longer linked.

- [x] M04-01 Split upstream main ownership into explicit core boot/run/stop behind port C ABI; replace core_stub and avoid duplicate SDL/window startup.
- [x] M04-02 Implement every platform.h function required by runtime in platform_switch_sdl2.cpp, including safe unsupported desktop operations.
- [x] M04-03 Translate SDL3 init/events/timing/controller/audio interfaces correctly to SDL2; no mixed SDL2/SDL3 symbols.
- [x] M04-04 Preserve PS2Runtime initialize/loadELF ordering, early IoPaths, mapped disc lifetime and generated function registration.
- [x] M04-05 Install input/volume/gameinfo/trainer hooks and UI callbacks in correct lifecycle order without desktop hotkey requirements.
- [x] M04-06 Adapt host waits, worker scheduling and SDL event owner; maintain guest yield/resume and avoid main/game-thread deadlocks.
- [x] M04-07 Audit every desktop_os/thread_time candidate on this boot path; gate unsupported crash/profiler/control facilities explicitly.
- [x] M04-08 Replace upstream std::_Exit with joined orderly cleanup; cancel guest/IOP/audio work before destroying owned resources. Stop and drain upstream's stream reader thread (`vcs_stream.cpp`, never joined today) and the EE/IOP CD paths, and clear `ps2_disc::setImage`, before the `FolderDisc` is destroyed; `sceCdBreak`/`sceCdStop` are no-ops in both runtimes (M03 third review).
- [x] M04-09 Demonstrate asset reads, real game constructors, IOP/native sound initialization and front-end state progress using source-backed counters.
- [x] M04-10 Verify stop, startup failures, HOME/focus changes and repeated launches without leaked workers or corrupted saves.
- [x] M04-11 Expose bounded diagnostic/control commands for later scripted scenes, with user-save isolation and stable machine-readable errors.
- [x] M04-12 Compare null-path behavior to desktop reference and resolve every runtime exception/unlisted-resume/unsupported syscall blocking boot; classify M02's 561 unhandled-instruction events and indirect-fallback owners against code that actually executes.
- [x] M04-13 Boot the Switch runtime from `FolderDisc` and `GamePaths` only: no ISO discovery, ELF read from the extracted gamefolder, no staging or NRO embedding; route settings, cache, both memory cards (with their order file) and card backups through `GamePaths` under gamefolder/userdata, and test runs through a `GamePaths` test profile; prove on hardware that no ISO path is opened and that those files land there (moved from M03-09 and M03-10, user decisions 2026-11-12). Check the extracted ELF's SHA-1 at run time against `kElfSha1` or the manifest's `elf_sha1` and fail naming the path, since the folder boot skips upstream's `stageElf` check; offer `FolderDisc::verifyFiles` as a user-run integrity check that names bad files (M03 third review). `std::filesystem::remove_all` fails with EIO on the Switch (M03 hardware probe): replace upstream's card format/unformat `remove_all` (`MemoryCard.cpp`) with checked removal and prove a card format on hardware. `std::filesystem::rename` onto an existing file fails with EEXIST on the Switch: replace the temp-file-then-rename writes of settings (`settings.cpp`), card backups and card renames (`MemoryCard.cpp`) and prove a second settings save, a card backup over an existing one and a card rename on hardware.

Proof: candidate-linked map excludes stub, guest state logs, deterministic init
milestones, state/trace comparison and lifecycle tests. A blue screen is insufficient.

## M05 — OpenGL RHI and shader conformance

Branch: `milestone/m05-opengl-rhi` <!-- M05 -->

Exit: all required rhi::Device methods/shader variants operate correctly on Switch,
with diagnostic offscreen comparisons and resource lifetime proof.

- [x] M05-01 Add OpenGL backend selection to RHI without modifying desktop backend behavior; keep null backend available.
- [x] M05-02 Implement texture formats/creation/subresource update, row pitch, mip chains and sampler/filter/wrap/anisotropy behavior.
- [x] M05-03 Implement vertex/index/constant buffers, update/discard semantics and resource bounds/alignment checks.
- [x] M05-04 Implement immutable pipeline translation, vertex layouts, topology, blend equations/write masks and depth state.
- [x] M05-05 Implement render passes, viewport/scissor, clears, resolves, copies, readback and framebuffer completeness checks.
- [x] M05-06 Implement shader/constants/texture bindings and every required present, GS draw/world/lit, utility and UI GLSL variant.
- [x] M05-07 Verify dual-source blending support/limits and implement a correct bounded fallback only where required; no visual guess.
- [x] M05-08 Resolve clip-space depth/Y origin, handedness, texture swizzle/format and sRGB conventions against known images.
- [x] M05-09 Implement hazards/fences/staging and deferred destruction; freeThreadedCreation must reflect actual GL owner-thread constraints.
- [x] M05-10 Wire SDL-owned backbuffer/present/resize; no second swapchain/context or unsynchronized GL calls from workers.
- [x] M05-11 Implement UI renderer methods through existing ImGui/RHI ownership; verify reset-render-state/scissor and texture lifetime.
- [x] M05-12 Validate every Device virtual method and GLSL variant in deterministic resource/draw/readback scenes on hardware.
- [x] M05-13 Compare pixel/depth results with the desktop reference, record tolerances and resolve all unexplained differences.
- [x] M05-14 Run GL diagnostics and repeated resize/resource churn; enforce measured GPU/CPU memory budgets and no unbounded growth.

Proof: RHI method matrix, shader manifest, image/depth differences and target GL
diagnostics. Claim no ingame milestone until world integration is tested.

## M06 — First scene on Switch (BETA GATE)

Branch: `milestone/m06-ingame` <!-- M06 -->

Exit: the beta definition above, proven on hardware by M06-18. Stages from an empty
test card: S1 legal/loading, S2 opening flyover, S3 intro cutscene, S4 gameplay at
Fort Baxter. The game has no title menu on an empty card. Bring up one stage at a
time and compare each with the desktop reference.

- [x] M06-01 Connect presenter/GS/VIF/VU/native-world submission to OpenGL and ensure the authentic/reference path remains selectable.
- [x] M06-02 Validate GAME.DTZ, common models/textures and the LVZ/IMG streaming and relocation that S1-S4 actually use.
- [x] M06-03 Verify opaque/alpha-tested/transparent geometry, lighting, fog, texture palettes/mips and draw ordering in S2-S4.
- [x] M06-04 Verify the skinned characters, animation, shadows and transient effects that appear in S2-S4; wider coverage is M14-01.
- [x] M06-05 Verify camera, culling, HUD, radar and subtitles at the authentic 4:3 picture in S2-S4, pillarboxed without stretch or crop; widescreen is M09.
- [x] M06-06 Render at a fixed recorded internal scale independent of output dimensions; recreate targets and camera state on docking (docking: M10-13, user decision 2026-11-17, docs/milestones/M06/SCOPE_DECISIONS.md).
- [x] M06-07 Establish a repeatable procedure from cold boot with an empty test card to S4, with state/position evidence and input timestamps.
- [x] M06-09 Compare S1-S4 screenshots and key guest state with the desktop reference; resolve every unexplained material mismatch.
- [x] M06-11 Capture median/p95 frame time, memory and CPU/GPU timing in S2 and S4 at stock clocks in handheld mode (docked: M10-13, user decision 2026-11-17, docs/milestones/M06/SCOPE_DECISIONS.md); report them with no FPS target.
- [x] M06-12 Verify exit/load/restart with active streaming and eliminate world resource leaks or hangs.
- [x] M06-13 Moved to M10-13 by the user decision of 2026-11-17 (the dock is broken; docs/milestones/M06/SCOPE_DECISIONS.md): record the deferral and that nothing is claimed for docking; no docking test is part of M06. Original text: when the first real game boot reaches S2, the first real game picture, perform the user-deferred M01 docking check: dock/undock repeatedly and verify drawable pixels, viewport, 1280x720 handheld/1920x1080 docked output and ownership of one SDL GL context/surface. The M01 decision named the main menu; the game has no title menu on an empty card, so S2 is the equivalent trigger. Record actual hardware observations; this is not passed by the scaffold or a null-renderer boot.
- [x] M06-14 When the first real game boot reaches S2, complete the user-deferred formal physical connection-state matrix on paired Joy-Cons (the Pro Controller part moved to M13-11 by the user decision of 2026-11-17, docs/milestones/M06/SCOPE_DECISIONS.md): initial open, hotplug, full disconnect-to-neutral, reconnect and orderly application exit. Verify paired Joy-Con identity and physical connection reporting, investigate and resolve the M01 SDL connected=1 observation, and retain hardware logs. M01's user-confirmed Joy-Con result does not prove Pro compatibility or correct physical-disconnection metadata.
- [x] M06-15 Build without FFmpeg using vcs_movie's existing no-decoder path and an intro-skip setting; confirm on hardware that S1-S4 do not depend on the skipped PSS movies, and record the limitation.
- [x] M06-16 Provide the minimum real controller path for beta: SDL physical-position mapping for Cross, Start and on-foot sticks on paired Joy-Cons, attached and wireless (Pro Controller: M13-11, user decision 2026-11-17), L3/R3 reported, Plus as Start; replace the smoke Plus-to-exit with a deliberate exit path.
- [x] M06-17 Run native sound/IOP against a bounded silent or muted sink and verify S1-S4 do not stall when no audible output exists, in particular S3 cutscene pacing; make no audible-sound claim.
- [x] M06-18 Beta acceptance on real Switch in application mode at stock clocks, from a clean SD layout with the NRO and gamefolder only and an empty test card: reach S4 through S1-S3, world and HUD visible, player moves, 5 continuous minutes without crash or hang, orderly exit, in handheld mode (docked: M10-13, user decision 2026-11-17, docs/milestones/M06/SCOPE_DECISIONS.md). Retain logs and video.
- [x] M06-19 Package the beta privately: exact NRO SD layout, checksums, user disc-preparation instructions and honest notes listing skipped movies/cutscene, silence, measured frame times and all non-claims. No game data, generated code or ISO in any artifact.
- [x] M06-20 In handheld mode (docked: M10-13, user decision 2026-11-17, docs/milestones/M06/SCOPE_DECISIONS.md), prove from a handheld start that the fixed-surface output (src/switch/output.c) shows the 1280x720 picture on the panel the right way up and uncut, with GL errors 0. Original scope, the docked part of which moved: restore mode-appropriate output at docking (1280x720 handheld / 1920x1080 docked) without the switch-sdl2 surface-resize leak: since M05 the window is fixed at the starting mode's size, so a handheld start outputs 720p upscaled on the TV and a docked start renders 1080p downscaled on the panel. Use a leak-free surface recreation (for example a patched switch-sdl2 in the locked toolchain) or a fixed drawable with a mode-appropriate render scale; prove heap flat across repeated dock changes in both start modes. M06-13 depends on it.

Proof: S1-S4 screenshots and guest-state comparison, timing report, hardware
logs and video. Tag the beta only after independent review and merge.

## M07 — Real sound, music, movies and synchronized streaming

Branch: `milestone/m07-audio-movies` <!-- M07 -->

- [x] M07-01 Connect native libsd/SPU2 output to bounded PCM producer ring and SDL2 AudioStream/device consumer; remove silence stub.
- [x] M07-02 Preserve sample rate, interpolation, stereo channel order, clipping/saturation and game volume settings against desktop reference.
- [x] M07-03 Keep consumer timing independent of rendering; measure queue depth, latency, underruns and recovery from deliberate frame stalls.
- [x] M07-04 Verify effects/voice/mission banks, ambient sounds, positional balance and simultaneous channels across representative scenes.
- [x] M07-05 Verify radio/music/mission streams, seeking/looping and world transitions using AUDIO lookup/SDT/VB/RAW metadata correctly.
- [x] M07-06 Add pinned target movie decoder dependencies or equivalent validated implementation; host FFmpeg libraries are forbidden.
- [x] M07-07 Decode/render INTRO/STINGS PSS and gameplay cutscenes with audio/video sync, correct colors/aspect and bounded memory.
- [x] M07-08 Validate skip/pause/seek/stop and cancellation with pending I/O/audio; no stale buffers replay after transitions.
- [x] M07-09 Verify HOME/suspend/resume/device shutdown and error recovery without double audren ownership or callback I/O.
- [x] M07-10 Record audible hardware playback plus PCM comparison and long streaming route; silence/nonzero PCM alone cannot close this milestone.
- [x] M07-11 Verify no audio drift or cutscene desynchronization across variable frame rates in handheld mode (docked: M10-13, user decision 2026-11-18, docs/milestones/M07/SCOPE_DECISIONS.md).

## M08 — Developer menu on L3+R3 and settings

Branch: `milestone/m08-dev-menu` <!-- M08 -->

Exit: on real Switch, pressing L3+R3 opens and closes one developer menu with
controller-only navigation, and the chord does not reach the game.

- [x] M08-01 Integrate upstream's ImGui core with SDL2 input backend and existing RHI UI renderer, one context and correct frame order.
- [x] M08-02 Open and close the menu with L3+R3 on Switch, replacing the desktop Back+Start chord only for this target; add gamepad navigation, focus/input capture and readable 720p handheld and 1080p docked layout (the 1080p layout checked in handheld with forced 1920x1080 output; docked: M16-01, user decision 2026-11-19, docs/milestones/M08/SCOPE_DECISIONS.md).
- [x] M08-03 Expose renderer scale, timing, audio, controller settings and diagnostics through the upstream settings registry, without clutter in ordinary game menus.
- [x] M08-04 Persist settings beneath gamefolder and verify defaults/reset/relaunch; recover from malformed config safely.
- [x] M08-08 Verify settings/menu close/reopen during gameplay, cutscene transitions and suspend without leaking input or GL state.
- [x] M08-09 Verify trainer/dev commands use guest scheduler-safe hooks and cannot invalidate normal release save tests silently.
- [x] M08-14 Prevent chord leakage: withhold L3/R3 from the guest for a short recorded window until a chord is ruled in or out, so no L3/R3 edge reaches the game on open or close; prove R1-then-L3 free aim still works and report the added input latency.
- [x] M08-15 Reach the existing trainer, free camera, performance overlay and frame-time recording from the controller through the menu, with no keyboard or F-key dependency on Switch.
- [x] M08-16 Hardware acceptance on paired Joy-Cons in handheld mode (Pro Controller: M16-02, user decision 2026-11-19, docs/milestones/M08/SCOPE_DECISIONS.md): repeated L3+R3 open/close in S2, S3, S4 and across suspend, with logged proof that no chord input reached the guest.
- [x] M08-17 Make Dear ImGui's touch and mouse positions agree with its display size on the fixed 1920x1080 surface (since M06-20 the display size is the mode's output, 1280x720 in handheld, while the SDL2 backend reports window coordinates); verify on hardware (touch screen; the USB mouse's hardware check: M16-04, user decision 2026-11-19, docs/milestones/M08/SCOPE_DECISIONS.md).

## M09 — Widescreen

Branch: `milestone/m09-widescreen` <!-- M09 -->

Exit: 16:9 Hor+ picture on Switch with correct HUD, culling and 2D frames, selectable
from the dev menu; 4:3 remains available. Upstream's design and limits:
`upstream/docs/SETTINGS_MENU_WIDESCREEN.md`.

- [x] M09-01 Wire the upstream `display.aspect` chain (presenter display aspect, Hor+ widening, GS widening, HUD squeeze) through the OpenGL path; make 16:9 the Switch default once accepted and keep 4:3 selectable, both persisted through the dev menu.
- [x] M09-02 Verify the Hor+ view-window wrapper and entity-culling plane widening on ARM64 under guest-hook rules, numerically against the desktop reference.
- [x] M09-03 Verify HUD modes (keep 4:3, screen edges, stretch), HUD classification and scissor squeeze on GL, including radar and centred text.
- [ ] M09-04 Verify 2D-only frames (loading, pause map) stay 4:3 inside the frame, and each presented frame's shape matches its picture at transitions.
- [x] M09-05 Keep the game's own PS2 widescreen option off and its menu row hidden; verify the cutscene-bars setting and the scope/camera mask.
- [ ] M09-06 Size internal render targets for 16:9 and record memory and GPU cost against 4:3 at the same scale in both modes (handheld here; docked: M16-03, user decision 2026-11-19, docs/milestones/M09/SCOPE_DECISIONS.md).
- [x] M09-07 Verify no missing geometry or unstreamed cells at the frame edges in S2-S4 and a driven route.
- [ ] M09-08 Hardware acceptance in handheld and docked: reference-scene comparison with desktop at 16:9, and live 4:3/16:9 switching without corruption (handheld here, paired Joy-Cons; docked: M16-03, user decision 2026-11-19, docs/milestones/M09/SCOPE_DECISIONS.md).

## M10 — Stability, lifecycle and measured performance

Branch: `milestone/m10-stability-performance` <!-- M10 -->

- [ ] M10-01 Pin stock-clock hardware/firmware/build/settings and define a repeatable busy gameplay route with warm/cold run procedures.
- [ ] M10-02 Instrument CPU frame/game/worker/streaming time, GPU time, stalls, memory, cache churn and audio underruns with bounded overhead.
- [ ] M10-03 Identify bottlenecks from measurements and optimize one owning layer at a time with before/after evidence and visual/numeric regression checks. From M09-06 (user decision 2026-11-21, docs/milestones/M09/SCOPE_DECISIONS.md): 16:9, the Switch default, doubles the S4 game frame against 4:3 (median 48.8 vs 23.9 ms at 1x, handheld) and its wide picture targets (largest 853x1472, against 640x1472 at 4:3) add a stall of about 400 ms roughly once a second (p95 366.1 ms; 103.3 ms with PS2X_HW_WIDE_SCALE=0).
- [ ] M10-04 Establish 25+ FPS baseline on the busy route in both modes; report median/p95 frame time and sustained slow intervals at explicit internal/output scales.
- [ ] M10-05 Attempt native 720p/1080p internal resolutions separately; report observed limits without relabelling scaled output as native rendering.
- [ ] M10-06 Verify frame pacing and simulation/mission timer/audio cadence at 25/30/variable FPS; no guest-time manipulation to meet FPS target.
- [ ] M10-07 Run 30-minute minimum continuous gameplay soak in both handheld and docked modes with streaming, combat, missions and interior transitions. From M06-12: heap_obtained still grew after the heap in use levelled off (fragmentation), and patch 0009's prune age measured 6.6-10.5 minutes on the Switch while driving; check both over the soak.
- [ ] M10-08 Repeat docking/undocking, HOME, suspend/resume, controller loss and orderly exit under heavy streaming/audio/GPU work.
- [ ] M10-09 Inject missing/corrupt assets, allocation/I/O failures and save interruptions safely; prove diagnostics and bounded recovery.
- [ ] M10-10 Fix all crash/hang/leak/save/audio/visual findings and rerun the affected reference matrices; retain unresolved issues as unchecked tasks.
- [ ] M10-11 Validate memory headroom and thermal/clock observations over the soak; no undocumented overclock requirement.
- [ ] M10-13 Moved to M16-01 by the user decision of 2026-11-19 (docs/milestones/M08/SCOPE_DECISIONS.md): record the move; nothing docked is claimed in M10.
- [ ] M10-12 Produce a reproducible performance/stability report and final acceptance matrix tied to exact candidate SHA.

25+ baseline is operationalized as >=25 measured presented gameplay frames/s over
the standard route with p95 frame time <=40 ms after stated warmup. Loading screens
are reported separately, never silently removed. Any scope/budget change needs the
user's decision; reviewers cannot waive a missed target.

## M11 — Custom textures

Branch: `milestone/m11-custom-textures` <!-- M11 -->

Exit: optional folder-based texture replacement on Switch, with safe fallback. No
replacement system was found upstream, so identity and injection are new design.

- [ ] M11-01 Choose and document a stable texture identity from the GS texture cache/native world path (indexed data plus CLUT), proving it stays stable across streaming and palette changes; exclude or handle render targets and animated palettes explicitly.
- [ ] M11-02 Add a controller-triggered dump mode writing identified textures to a local mods folder; dumps are user-local and never committed.
- [ ] M11-03 Load replacements through the VFS from `gamefolder/mods/textures/`; choose and pin the decoder and supported formats, document naming and precedence, and support higher-resolution replacements with correct UV behavior.
- [ ] M11-04 Enforce GPU/CPU memory budgets, bounded upload per frame and no streaming stalls; measure against the same scene without replacements.
- [ ] M11-05 Fall back to the original texture when a replacement is absent or invalid; preserve original assets and reject absolute or traversing paths.
- [ ] M11-06 Add dev-menu controls to enable/disable and safely reload custom textures; show actionable load errors and bounded memory use, release replaced GPU resources safely and persist the setting under userdata.
- [ ] M11-07 Validate on Switch in handheld/docked gameplay: known replacements, fallback, corrupt/oversized files, rejected paths, repeated reload, world streaming and suspend/resume; record visual comparison and memory/resource-lifetime evidence.

## M12 — Custom audio

Branch: `milestone/m12-custom-audio` <!-- M12 -->

Entry: M07 accepted, because replacements cannot be heard or compared without real
game sound. Exit: optional folder-based music and effect replacement on Switch.

- [ ] M12-01 Identify replaceable units and stable IDs: `.VB` music/cutscene streams through M_LOOKUP.BIN, and `.RAW`/`.SDT` effect banks; document which are supported and why.
- [ ] M12-02 Provide a metadata listing of those IDs for mod authors, with no game audio committed or distributed.
- [ ] M12-03 Define `gamefolder/mods/audio/` layout, naming, precedence and formats; pin any decoder, with uncompressed PCM as the baseline.
- [ ] M12-04 Inject music/stream replacements at the native sound layer, preserving looping, seeking, duration-dependent cutscene sync and stop/cancel; no file I/O on the audio callback.
- [ ] M12-05 Inject effect-bank replacements within SPU2 RAM limits, with rate and loop handling; reject replacements that do not fit, with a clear error.
- [ ] M12-06 Add dev-menu enable/disable, reload rules, load errors and persisted settings; fall back to original audio when a replacement is absent or invalid, and reject absolute or traversing paths.
- [ ] M12-07 Validate on Switch with audible human confirmation: known replacements, fallback, corrupt/oversized files, cutscene sync, long streaming route and suspend/resume, with underrun and memory evidence.

## M13 — Complete controllers and controller-only interaction

Branch: `milestone/m13-controllers` <!-- M13 -->

- [ ] M13-01 Confirm SDL physical-position mapping on paired Joy-Cons, handheld controls and Pro Controller with logged button identity.
- [ ] M13-02 Map every supported PS2 action, both sticks, ZL/ZR, Plus/Minus, d-pad and stick clicks through existing binding/report layers.
- [ ] M13-03 Verify active-low DualShock report, pressure/trigger expectations, signed extremes, dead zones and neutral disconnected state.
- [ ] M13-04 Preserve on-foot aim/camera, vehicle controls, lock-on, weapon cycling, swimming and contextual mission inputs.
- [ ] M13-05 Make all game front-end/pause/map/text/save menus usable without keyboard/mouse; implement required text-entry flow.
- [ ] M13-06 Verify rumble amplitude/duration/stop and reconnection without stale handles or blocking frame loops.
- [ ] M13-07 Test disconnect/reconnect and controller reassignment during gameplay, menus, HOME and resume.
- [ ] M13-08 Preserve configurable bindings/presets and safe persistence; prevent overlay/game simultaneous input.
- [ ] M13-09 Replay complete controller-only gameplay/menu route on each supported controller configuration and document unsupported layouts explicitly.
- [ ] M13-10 Re-verify, after M06-16 removed smoke Plus-to-exit, ordinary Start/Select semantics and safe deliberate application exit.
- [ ] M13-11 Moved to M16-02 by the user decision of 2026-11-19 (docs/milestones/M08/SCOPE_DECISIONS.md): record the move; nothing is claimed for the Pro Controller in M13.

Exit proof: hardware controller matrix and action-by-action route, not only device
enumeration. Single sideways Joy-Con is not implicitly supported; decide via scope.

## M14 — Whole world and safe saves

Branch: `milestone/m14-world-saves` <!-- M14 -->

Exit: representative outdoor, interior and streaming play across the map, with
reliable saves.

- [ ] M14-01 Verify skinned characters/animations, vehicles, water, particles, shadows, cutouts and transient effects beyond the first scene.
- [ ] M14-02 Traverse BEACH, MAINLA and MALL/interior boundaries and verify no missing terrain, corrupt textures or stalls; include live BEACH and MALL level loads through the folder disc compared with the source ISO (moved from M03-08, user decision 2026-11-12).
- [ ] M14-03 Exercise mission start/progress, pause/map/shop/interior entry and return to gameplay; distinguish menu boot from real game simulation.
- [ ] M14-04 Establish a scripted reproducible on-foot and driving route with state/position evidence and input replay timestamps.
- [ ] M14-05 Verify memory-card file layout, directory order, metadata and capacity behavior match game expectations.
- [ ] M14-06 Implement/verify save backups and atomic replacement/recovery; tests operate on disposable copies, never live user saves.
- [ ] M14-07 Complete create/save/load/overwrite/delete/full-card/error flows through controller menus and verify mission/player state restored.
- [ ] M14-08 Document player controls/SD layout/troubleshooting and reproduce a full controller-only start-to-save-to-reload session.
- [ ] M14-09 Make every save test check the run's `trainer-used.txt` (`gamefolder/userdata/`, or `userdata/tests/<profile>/` for a test profile; written by the developer menu at a run's first trainer use, M08-09) and report a save made after trainer use as not a normal-release result, never silently passing it.

## M16 — Pro Controller and docked mode

Branch: `milestone/m16-pro-controller-docked` <!-- M16 -->

Entry: M14 merged, every handheld milestone done. Added by the user's decision of
2026-11-19 (docs/milestones/M08/SCOPE_DECISIONS.md); runs before M15. Needs a Pro
Controller and a working dock.

- [ ] M16-01 (was M10-13) Complete the docked work deferred from M06 by the user decision of 2026-11-17 (docs/milestones/M06/SCOPE_DECISIONS.md): M06-06's target and camera recreation on docking, M06-13's docking check, M06-20's docked 1920x1080 output and repeated dock changes from both start modes with a flat heap, docked S2/S4 timing (M06-11) and the docked beta acceptance run (M06-18), M07-11's docked audio drift and cutscene synchronization (user decision 2026-11-18, docs/milestones/M07/SCOPE_DECISIONS.md), and M08-02's docked developer-menu layout, including a dock change while the menu is open (user decision 2026-11-19, docs/milestones/M08/SCOPE_DECISIONS.md), on hardware with logs and video.
- [ ] M16-02 (was M13-11) Complete the Pro Controller work deferred from M06-14/M06-16 by the user decision of 2026-11-17 (docs/milestones/M06/SCOPE_DECISIONS.md): identity, physical-position mapping, the physical connection-state matrix (initial open, hotplug, disconnect to neutral, reconnect, orderly exit) and play on a real Pro Controller, with hardware logs; and M08-16's L3+R3 developer-menu acceptance on the Pro Controller (user decision 2026-11-19, docs/milestones/M08/SCOPE_DECISIONS.md).
- [ ] M16-03 Repeat in docked mode, and on the Pro Controller where a controller is involved, every docked or Pro Controller part that M09-M14 deferred here, each listed with its source task, on hardware with logs. From M09: M09-06's memory and GPU cost of 16:9 against 4:3 at the docked 1920x1080 output, and M09-08's docked acceptance (the 16:9 picture on a TV, a dock change at 16:9 and at 4:3, live switching docked).
- [ ] M16-04 (from M08-17) With a USB mouse on the Switch, verify that Dear ImGui's mouse position matches the pointer on the screen in handheld and docked output (src/platform/platform_switch_sdl2.cpp `toUiCoordinates`), and fix the mapping if switch-sdl2 reports surface pixels rather than panel-scaled positions; user decision 2026-11-19, docs/milestones/M08/SCOPE_DECISIONS.md.

## M15 — Final packaging, clean deployment and acceptance

Branch: `milestone/m15-final-release` <!-- M15 -->

- [ ] M15-01 Rebuild from clean pinned source/toolchain on a second clean environment; verify expected reproducibility or explain deterministic build differences.
- [ ] M15-02 Generate final source/version/build-info, NRO metadata, checksums and license notices without including game/generated assets.
- [ ] M15-03 Package exact switch/gtavcs_nx/gtavcs.nro layout plus separate user-disc preparation instructions/tooling.
- [ ] M15-04 Audit Git history, submodule/patch completeness and release artifacts for game bytes, secrets and accidental test saves.
- [ ] M15-05 Deploy onto a clean test SD layout and reproduce first boot, controller-only game route, audio, overlay, save/reload, dock/resume and exit.
- [ ] M15-06 Repeat final visual/audio/performance/soak acceptance on exact release candidate with 720p handheld/1080p docked output and recorded internal scale.
- [ ] M15-07 Validate mod-folder behavior, failed extraction/wrong-disc diagnostics, storage limits and upgrade preservation of existing saves/settings.
- [ ] M15-08 Write honest release notes, known supported configurations, measured limits, troubleshooting and recovery instructions.

After all eight tasks: independent review, resolve/re-review findings, merge and
tag accepted source, then attach only reviewed allowed artifacts to a private
release. Verify release visibility/checksums/download layout and record those
post-review publication actions separately. Review/merge/publication are workflow
gates, not pre-review tasks; this avoids circular review eligibility. M15 is
last in execution order; after it, do not invent a new branch/milestone without a
new user objective.

