# GTA Vice City Stories NX — developer contract

## Scope and status

Native Nintendo Switch homebrew port of `gothamor/gta-vcs-recomp` (PS2 static
recompilation), using devkitPro/devkitA64 and libnx on Horizon OS. Host work runs
in WSL2 Ubuntu or a Linux Docker container. This workspace initially contained
only a user-provided ISO; the upstream checkout is now in `upstream/`.

Inspected upstream revision: `ecf150e53f87e4656dfe81eacd922839d952b282`.
Check that revision before applying the file checklist to a newer checkout.
Read `upstream/AGENTS.md` before editing anything below `upstream/` and preserve
its documentation, runtime patch markers, numeric accuracy and reference paths.

## Mandatory milestone workflow

Follow [docs/MILESTONES.md](docs/MILESTONES.md) as the authoritative task ledger
and [docs/AUTOMATED_EXECUTION.md](docs/AUTOMATED_EXECUTION.md) as the runbook.
Read [docs/PORT_DISCOVERY.md](docs/PORT_DISCOVERY.md) and its complete scoped
function/asset/portability inventories before implementation. Work on exactly
one milestone branch at a time, created from reviewed main. Never implement or
commit milestone work directly on main.

**Only after ALL tasks in a milestone are complete, evidenced, committed and
validated may a fresh independent reviewer agent be created.** Run
`python tools/milestone_gate.py Mxx` first. Do not create a reviewer for partial
work. The reviewer must independently check the exact candidate; resolve every
finding on the same branch and re-review changes. Record actual independent
approval and pass `--require-review` plus CI before merging into main. Only then
create/push the next milestone branch. Missing hardware keeps required tasks open;
no waivers, invented proof or premature milestone advancement.

The user explicitly authorizes the independent milestone reviewer. This does not
authorize general implementation delegation or additional user-owned chats.
GitHub plan limitations on private branch protection must be recorded honestly;
they do not relax these operating rules.

**Current deliverable (M10: stability, lifecycle and measured performance, in handheld and docked; in progress).**
M06 (the handheld beta), M07 (sound and movies), M08 (the developer menu on L3+R3) and M09 (16:9 Hor+ by default)
are merged. The dock works again (user decision 2026-11-24): M10 also owns the docked work deferred from M06-M08.
Builds use one CPU core by default (`--jobs`, `NX_BUILD_JOBS`): the build host is a shared NAS.
`python tools/build_game.py` links the private generated EE/VU archive, the
overlaid upstream runtime/game layer, `src/core/core.cpp` behind the C ABI in
`src/core.h`, the SDL2 platform adapter and the OpenGL 3.3 RHI backend
(`src/rhi/gl`, GLSL ports in `src/render/glsl`); the RHI swaps the window, and
`VCS_RHI=null` keeps the null backend. The window is a fixed 1920x1080 surface cropped
to the mode's output (`src/switch/output.c`, M06-20). It also builds the public RHI conformance
NRO (`src/rhi_tests`). `--public` (CI) compiles every public library and links
no game code. The C ABI is port-owned glue, not an upstream API. Do not describe
a successful build as ingame validation.

## Corrected graphics architecture

The agreed baseline is **SDL2 + OpenGL core 3.3 + EGL/Mesa nouveau + libnx**:

```text
generated EE/VU C++ + PS2Recomp runtime + VCS native HLE
                       |
               platform / RHI interfaces
                /          |           \
  SDL_GameController   OpenGL RHI     PCM producer ring
       Joy-Con/Pro     GLAD -> Mesa   SDL_AudioStream -> SDL2 -> audren
                          EGL
                          libnx NWindow -> display
```

Use `SDL_WINDOW_OPENGL` and `SDL_GL_SwapWindow`. SDL owns the EGL surface/context
and presentation. Do not create another EGL surface, NVN context, Vulkan
swapchain, SDL_Renderer or libnx framebuffer for the same window.

NVN is a separate proprietary API, not a Vulkan driver. Stock switch-sdl2's
Switch video backend supplies EGL/GL hooks, not Vulkan hooks. Linking `vulkan`
does not add SDL Vulkan support. No Vulkan library belongs in this baseline.
An experimental NVK route requires a separately pinned driver AND compatible
SDL backend and hardware feature tests; it is outside the current build.
deko3d is a possible future backend behind the RHI, after measurements justify it.

Upstream currently uses **SDL3**, D3D11 on Windows and Vulkan on Linux; it has
no existing OpenGL RHI to switch on. Preserve its API-neutral platform/RHI seams.
Implement SDL2 and GL adapters rather than introducing graphics calls into game
logic. Upstream's HLSL shaders need GLSL equivalents and visual comparison.

## Folder structure

```text
gtavcs_nx/
  AGENTS.md                      this contract and build instructions
  CMakeLists.txt                 Switch scaffold; official toolchain only
  src/
    main.c                       process, cwd, SDL2/GL/audio/input lifetime
    core.h                       C ABI between main.c and the C++ core adapter
    core/                        C++ core adapter (boot/frame/shutdown, nxrun.cfg)
    platform/                    SDL2 implementation of upstream's platform.h
    switch/                      libnx link wraps (thread stacks, open audit, indirect-branch trace)
    rhi/gl/                      OpenGL 3.3 core backend of upstream's rhi::Device (M05)
    render/glsl/                 GLSL 330 ports of upstream's HLSL shaders (README: conventions)
    rhi_tests/                   RHI conformance scenes: host runner, Switch NRO
    devmenu/                     developer menu on L3+R3 and its chord filter (M08)
  third_party/glad/              generated GLAD 2.0.8, GL 3.3 core, no extensions
    include/{glad,KHR}/
    src/gl.c
    README.md                    provenance and regeneration
  upstream/                      pinned public upstream Git submodule
    src/{game,platform,rhi,render,sound,settings,ui,disc,debug}/
    third_party/PS2Recomp/        EE/VU/GS/IOP runtime and host tools
    game/vcs/                    supported executable config/function maps
    tools/                       host recompiler and validation scripts
    generated/                   private output generated from the user's disc
  docs/
    PORTING_CHECKLIST.md         exact upstream files and integration order
    SWITCH_REFERENCES.md         inspected public Switch port references
    BETA_ROADMAP.md              beta definition, execution order, step-by-step route
    MILESTONES.md                complete task/branch/review/merge contract
    PORT_DISCOVERY.md            function/asset/subsystem discovery dossier
    AUTOMATED_EXECUTION.md       resume, validation, review and handoff runbook
    discovery/                  metadata-only complete scoped inventories
    milestones/Mxx/             task evidence, reports and independent review
  tools/                         discovery, foundation and milestone gates
  build/                         ignored build output and host tool venv
  dist/                          ignored staged SD layout, NRO only
  gamefolder/                    optional ignored local asset staging
```

`upstream/` is registered in `.gitmodules` and pinned by the root gitlink.
Clone with `git clone --recurse-submodules` or initialize with
`git submodule update --init`. Maintain port changes in root overlay/patch tooling
against that pin; never assume uncommitted submodule edits are captured by a root
commit. See the discovery dossier for the patch strategy.

## Target filesystem

```text
sdmc:/switch/gtavcs_nx/
  gtavcs.nro
  gamefolder/
    TEXT/ AUDIO/ MODELS/ ...     actual extracted PS2 tree, preserve names
    mods/                       future mod loader root
    userdata/                   future saves/cache; test on separate copies
    gtavcs.log                  scaffold stderr log, overwritten per run
```

Call checked `chdir("sdmc:/switch/gtavcs_nx/gamefolder")` before SDL/core startup,
before creating workers. Missing root is a fatal configuration error; never
fall back to the NRO directory or silently create an empty asset tree.
All game assets and mod lookups resolve relative to this root. `chdir` is not a
sandbox: the future VFS must normalize `cdrom0:`, `\\`, ISO `;1` suffixes and case,
and reject absolute/traversing mod paths. Keep the actual disc layout and filename
case; do not invent flat replacements for archives.

Upstream reads the ISO through sector/LBA APIs. A loose extracted tree alone does
not preserve those semantics. Implement and validate a manifest-backed folder
disc adapter, retaining original LBA/size metadata, before claiming folder-only
boot. Do not substitute a hidden runtime ISO dependency for the specified layout.

## Build prerequisites

Install devkitPro using its official instructions, then in a devkitPro-enabled
Ubuntu/container environment install packages (not Ubuntu SDL/OpenGL packages):

```bash
sudo dkp-pacman -Syu --needed switch-dev switch-cmake switch-pkg-config switch-sdl2 switch-mesa switch-ffmpeg
```

Omit `sudo` inside a root container. Install host CMake >=3.24 and Ninja using
the host package manager. `switch-dev` supplies devkitA64, libnx and Switch tools;
`switch-mesa` supplies GL/EGL and depends on the nouveau support library.
`switch-sdl2` is a package name; the linked library is `SDL2`, not `switch-sdl2`.
GLAD sources are already generated and included; no network fetch at configure.

## Custom build commands (WSL2 Ubuntu)

Prefer a WSL ext4 checkout for generated-code builds; the Windows-mounted
Desktop is usable for this small scaffold but expensive for many generated files.
An Ubuntu WSL distribution must be installed: `docker-desktop` alone is not one.

```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITA64="$DEVKITPRO/devkitA64"
export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH"
cmake -S . -B build/switch -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/switch --parallel "$(nproc)" --target gtavcs_nro
cmake --install build/switch --prefix "$PWD/dist"
```

Outputs: `build/switch/gtavcs.elf`, `gtavcs.map`, `gtavcs.nacp`, `gtavcs.nro`.
Installation stages `dist/switch/gtavcs_nx/gtavcs.nro`. Build before installing.
Asset extraction/copy is separate; install never packages the ISO or assets.
Copy the staged NRO into the exact SD layout above. Launch with full application
memory (title override); applet-mode memory is not a supported game target.

For Docker, run the same commands inside a devkitA64 image with the prerequisites
installed, mounting source as `/work` and using `/work` as the working directory:

```bash
# From WSL, after Docker Desktop's Linux engine/WSL integration is running:
docker run --rm -it -v "$PWD:/work" -w /work devkitpro/devkita64:20260219 bash
```

That dated image is an inspected REDRIVER2 reference, not a validated lock for
this project. Run prerequisite installation inside it. Pin an image digest and
record `dkp-pacman -Q` before calling the pipeline reproducible; package updates
change a dated base image's dependencies. CMake deliberately uses only the
Switch cross pkg-config and static portlibs, not Linux X11/Wayland/ALSA libraries.

The upstream generator remains a **host** tool. First check a disc without
extracting it: `python3 upstream/tools/vcs_disc.py check /path/to/disc.iso`.
The provided ISO was checked successfully as SLUS-21590 USA v1.00 on 2026-11-08.
Generate EE/VU code with upstream's host tooling in a separate host build;
`tools/build.py --recompile` also builds the desktop target, so it is not the
Switch build command. See the checklist before attempting ARM64 core compilation.

## Runtime ownership and developer guidelines

- The main thread owns SDL events, window/context, GL calls and presentation.
  The window is a fixed 1920x1080 surface without SDL_WINDOW_RESIZABLE:
  switch-sdl2 2.28 resizes a resizable window at dock changes and every resize
  (also SDL_SetWindowSize) leaks the EGL surface's buffers (M05). The picture is
  drawn at the operation mode's output size (1280x720 handheld, 1920x1080 docked)
  and the layer cropped to it (src/switch/output.c, M06-20); the renderer sees the
  output size as its back buffer. Proven in handheld only; docked output and dock
  changes are M16-01. Output sizes are targets, not measured performance.
  Keep render scale distinct from output size and keep HUD aspect correct.
- Snapshot controller state once per event pump; publish a synchronized snapshot
  to the future game thread. Handle hotplug, disconnect-to-neutral, dead zones,
  signed axis extremes, triggers and rumble. Map physical positions:
  south -> PS2 Cross, east -> Circle, west -> Square, north -> Triangle. Test the
  actual SDL mapping for paired Joy-Cons and Pro Controller. Plus -> Start,
  Minus -> Select. The smoke test temporarily uses Plus to exit.
- SDL2 `SDL_AudioStream` converts PCM; an audio device actually sends it to audren.
  SDL owns audren initialization/shutdown. Do not initialize audren a second time.
  The scaffold queues bounded silent S16 stereo 48 kHz blocks on the main thread.
  For the game use a producer ring and independent consumer so render stalls do
  not starve audio; count underruns/latency. Never perform file I/O or game boot
  from an audio callback. Mute/reset queued audio across suspend/resume safely.
- Keep a small C entry point; implement the C++ core adapter behind `core.h`.
  Convert upstream's blocking `main` into explicit boot/run/shutdown ownership.
  Preserve EE scheduler yields and guest-call semantics. Do not call a generated
  function directly and assume it runs to completion.
- Port x86 intrinsics through verified portable/SIMDe/NEON operations at the owning
  layer; no blanket header substitutions or AVX flags on ARM64. Preserve 128-bit
  guest layout, unaligned accesses, saturation, rounding and aliasing behavior.
  Disable FP contraction and preserve the guest/VU rounding rules.
- Upstream currently requires Clang 19+ in CMake, but `ps2_guest_cc.h` already
  defaults to the platform ABI on non-x86. Audit GCC compatibility and generated
  code before deciding whether to patch that gate or use a compatible Clang
  cross setup. Do not claim devkitA64 GCC is already supported by the game.
- Reuse upstream's ImGui context and RHI UI renderer. Replace its SDL3 backend
  with `imgui_impl_sdl2`, enable gamepad navigation and design an overlay chord
  that consumes input without leaking it into gameplay. Done in M08: one context
  (platform layer), upstream's settings menu and trainer, and the port's developer
  menu on L3+R3 (src/devmenu: the chord filter withholds every L3/R3 press 100 ms).
- Preserve original source licenses and credit; no game ISO, ELF, extracted assets,
  saves, generated game C++ or copyrighted shader dumps in commits/releases.
  Keep generated GLAD notices. Record runtime modifications in upstream's
  `docs/PS2RECOMP_PATCHES.md` and discoveries in `docs/game-internals.md`.
- Keep changes scoped and reviewable. Preserve desktop behavior and upstream's
  reference rendering paths. No speculative speed claims or GPU overclock changes.

## Acceptance direction

The authoritative M00–M15 tasks and their execution order live in MILESTONES.md;
docs/BETA_ROADMAP.md explains the route. Milestone numbers follow the execution
order since the 2026-11-11 renumbering; MILESTONES.md maps older record IDs.

Beta (M06, user decision 2026-11-09): the real game opens on a real Switch, goes
ingame and loads the first scene, Fort Baxter gameplay, with the world and HUD
visible and the player controllable for 5 minutes, then exits cleanly. Timing is
measured, with no FPS target. After beta, in order (user decision 2026-11-11):
real sound and movies, dev menu on L3+R3, widescreen, then performance (M10);
custom textures, custom audio, full controllers, saves and release follow M10.

Final acceptance (M10/M15) includes a controllable ingame route with audible
   music/effects, stable saves and docking, 30-minute soak, 720p/1080p output and
   adjustable internal scale. Target 25+ FPS: <=40 ms frame budget on a repeatable
   busy route. Record median/p95 frame time, audio underruns, memory and CPU/GPU
   timing at stock clocks in both modes. Intro/title FPS is not gameplay proof.

Current verification status (2026-11-17): the pinned container pipeline also runs
on a Linux host with Docker's engine (no local devkitPro). The game NRO links the
private generated core, the game layer and the SDL2 platform; on an actual Switch
it boots the real runtime from the folder disc, reaches the game's outdoor state
with the null renderer (black screen, the game's own sound), and passes HOME,
sleep, close and startup-failure checks (M04 evidence). The M05 OpenGL backend
runs on the Switch's Mesa 20.1 nouveau (GL 4.3 core): 127/127 conformance
readbacks within tolerance of upstream's Vulkan backend (101 identical), the real
game drawn through it with 0 GL errors, the fixed-size window kept across docking
from either starting mode in the game and the test program, with no heap growth
per change shown in the test program (the game's heap is sampled at the changes
only; output size: M06-20); every GL call stays on
the main thread (a shared worker context shares nothing on this driver, M05-09).
M06 (2026-11-17, handheld only, paired Joy-Cons only, stock clocks): from an empty
test card the game reaches S4 at Fort Baxter through S1-S3 with the world and HUD
drawn through OpenGL at 1x internal / 1280x720 output, the player controlled for
5+ continuous minutes, closed from HOME in order; the S3 cutscene runs out without
an audio device; Joy-Con disconnect/reconnect works through HID style sets. Measured
route S4 game frame median 43.1 ms / p95 67.9 ms (about 19 fps); no FPS target (M10).
Review 3 (2026-11-18): a 21-minute drive found a host heap leak in upstream's RSL
recording cache; root patch 0009 prunes it and the heap levels off while driving
(M06-12). The acceptance was repeated on the patched code (run 17: about 6 minutes
of play, game frame median 84.6 ms / p95 419.9 ms while moving).
Docked mode, docking and the Pro Controller are untested (user decisions: M16, added
2026-11-19 after M14, takes over M10-13 and M13-11).
M07 (2026-11-18, handheld): the boot movies play through the locked switch-ffmpeg 7.1
(target build, GPL) in sync with their sound (the picture due 0.00-0.07 s ahead, across HOME and
frame stalls); the game's sound matches the desktop reference in format and level, and the
volume settings apply. Root patch 0010 keeps the sound mixing through long game frames
(a 16-minute drive: 169 underruns before, 0 after the boot movies with the revised
patch); patch 0011 records that the game never loops a music stream. The user heard movies, effects,
radio, left / right and a mission's speech as correct. Docked audio: M16-01.
M08 (2026-11-19, handheld, paired Joy-Cons attached): L3+R3 opens one developer menu (src/devmenu) with
settings, trainer, free camera, performance overlay, frame-time recording and diagnostics; every L3/R3 press waits
100 ms for the other, and no chord's L3/R3 reached the game (15 chords in the final acceptance run; lone presses handed on
103-185 ms late; R1-then-L3 sets the game's own free-aim flag). Menus use A to choose and B to go back; touch is read from HID (switch-sdl2
delivered none). Damaged vcs.ini values are ignored and repaired; settings survive relaunch and reset. Root patch 0012.
The 1080p menu layout was checked in handheld with VCS_NX_OUTPUT=1080; docked and the Pro Controller: M16.
M09 (2026-11-21, review fixes 2026-11-24, handheld, paired Joy-Cons attached): upstream's Hor+ chain runs through the OpenGL backend unchanged;
the Switch's Aspect ratio offers 4:3 and 16:9, 16:9 by default (root patch 0013), switched live from the developer menu
and kept across relaunch. The view window and entity planes match the desktop reference; HUD modes, cutscene bars, the
scope mask and 2D-only frames (4:3 inside the 16:9 frame, 0 mismatched shapes) were checked on hardware. Cost at 1x
handheld (S4 standing): game frame median 48.8 / p95 366.1 ms at 16:9 against 23.9 / 57.5 ms at 4:3, with a ~400 ms
stall about once a second from the wide targets; the user kept 16:9 the default and moved the stall to M10-03.
Docked parts: M16-03.
