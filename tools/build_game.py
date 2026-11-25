"""Build the Switch game NRO (root CMakeLists.txt) in the locked devkitPro container.

    python tools/build_game.py            the private game NRO: links the generated EE/VU archive built from the
                                          user's disc (Docker volume gtavcs-nx-m02-core, generated-build/)
    python tools/build_game.py --public   source CI: every public library compiles; no generated code, no NRO
    --jobs N                              compile jobs and container CPUs (default 1: the host is a shared NAS)

Host side: fetches the locked public dependencies (sse2neon, Dear ImGui) into build/, prepares the overlaid pinned
upstream checkout (tools/upstream_overlay.py) and runs the container offline. The private NRO, ELF and map stay in
build/m04/private (ignored); they hold game code and are never committed, uploaded to CI or released.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from build_core_tests import dependency as sse2neon_dependency
from build_switch import IMAGE, reclaim
from upstream_overlay import prepare

ROOT = Path(__file__).resolve().parents[1]
VOLUME = "gtavcs-nx-m02-core"
LOGS = ROOT / "build/evidence/M04/build"
PRIVATE = ROOT / "build/m04/private"
ARCHIVE = "/cache/generated-build/libvcs_generated_cpu.a"
STAMP = "/cache/generated/recompile_stamp.json"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def run(args, log, cwd=ROOT):
    log.write("+ " + " ".join(map(str, args)) + "\n")
    log.flush()
    process = subprocess.Popen(list(map(str, args)), cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, encoding="utf-8", errors="replace")
    lines = []
    for line in process.stdout:
        print(line, end="", flush=True)
        log.write(line)
        lines.append(line)
    log.flush()
    if process.wait():
        raise RuntimeError(f"Command failed ({process.returncode}): {args[0]}")
    return "".join(lines)


def git(path, *args):
    return subprocess.check_output(["git", "-C", str(path), *args], text=True).strip()


def imgui_dependency(log):
    lock = json.loads((ROOT / "tools/core-dependencies.json").read_text())["imgui"]
    target = ROOT / "build/m04/deps/imgui"
    if not target.exists():
        target.mkdir(parents=True)
        run(["git", "init", str(target)], log)
        # the locked hash is of the repository's own (LF) bytes: no line-ending conversion on Windows hosts
        run(["git", "-C", str(target), "config", "core.autocrlf", "false"], log)
        run(["git", "-C", str(target), "config", "core.eol", "lf"], log)
        run(["git", "-C", str(target), "remote", "add", "origin", lock["repository"]], log)
        run(["git", "-C", str(target), "fetch", "--depth", "1", "origin", lock["commit"]], log)
        run(["git", "-C", str(target), "checkout", "--detach", lock["commit"]], log)
    if git(target, "rev-parse", "HEAD") != lock["commit"]:
        raise RuntimeError("Dear ImGui checkout is not the locked commit")
    if git(target, "status", "--porcelain", "--untracked-files=normal"):
        raise RuntimeError("Dear ImGui checkout has unowned changes")
    if digest((target / "imgui.h").read_bytes()) != lock["header_sha256"]:
        raise RuntimeError("Dear ImGui header checksum mismatch")
    return target


# Archive members with static constructors that may stay out of the NRO, and why. Anything else with one must be
# linked: a member that only registers itself (PS2_REGISTER_GAME_OVERRIDE, the VU0 natives' Registrar) is dropped
# silently by the linker when nothing references it, and the game then runs without it (first M04 Switch session).
UNLINKED_CONSTRUCTORS_ALLOWED = {
    "games_database.o": "a static name table for PS2Recomp's generic runner main(), which the port does not build",
}


def unlinked_constructors(build, linkmap):
    """Members of the port's archives that have static constructors but are not in the link map."""
    objdump = "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-objdump"
    missing = []
    for archive in (build / "libnx_core.a", build / "libnx_platform.a", build / "iop/libps2_iop.a"):
        member = None
        for line in subprocess.check_output([objdump, "-h", str(archive)], text=True).splitlines():
            if "file format" in line:
                member = line.split(":")[0].strip()
            elif ".init_array" in line and member:
                if f"{archive.name}({member})" not in linkmap and member not in UNLINKED_CONSTRUCTORS_ALLOWED:
                    missing.append(f"{archive.name}({member})")
    return sorted(set(missing))


RHI_TESTS = ROOT / "build/m05/rhi-tests"


def stage_rhi_tests(build):
    """The public RHI conformance NRO and its map / shader manifest, to build/m05/rhi-tests (ignored)."""
    out = Path("/work") / RHI_TESTS.relative_to(ROOT)
    out.mkdir(parents=True, exist_ok=True)
    for name in ("gtavcs_rhi_tests.nro", "gtavcs_rhi_tests.map", "shader-manifest.json"):
        shutil.copyfile(build / name, out / name)
    linkmap = (build / "gtavcs_rhi_tests.map").read_text(errors="replace")
    if "libvcs_generated_cpu" in linkmap or "nx_game_private" in linkmap:
        raise RuntimeError("Game code linked into the RHI tests NRO")
    print("PASS RHI tests NRO " + file_digest(out / "gtavcs_rhi_tests.nro"), flush=True)


def inside(args, log):
    if run(["dkp-pacman", "-Q"], log) != (ROOT / "docs/milestones/M01/package-lock.txt").read_text():
        raise RuntimeError("Toolchain package lock mismatch")
    build = Path("/cache/game-build-public" if args.public else "/cache/game-build")
    options = ["-DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake", "-DCMAKE_BUILD_TYPE=Release",
               f"-DVCS_UPSTREAM_DIR=/work/{args.source}", f"-DSSE2NEON_INCLUDE_DIR=/work/{args.sse2neon}",
               f"-DNX_IMGUI_DIR=/work/{args.imgui}"]
    if args.public:
        options.append("-DNX_PRIVATE_GENERATED_ARCHIVE=")
    else:
        stamp = json.loads(Path(STAMP).read_text())
        base = json.loads((ROOT / "patches/upstream/series.json").read_text())["base_commit"]
        if stamp["source"]["commit"] != base:
            raise RuntimeError("Private code was not generated from the pinned source")
        options += [f"-DNX_PRIVATE_GENERATED_ARCHIVE={ARCHIVE}", "-DNX_PRIVATE_GENERATED_DIR=/cache/generated/ee"]
        # the homebrew menu's icon: the ignored root icon.jpg when present (a local input, never committed)
        icon = ROOT / "icon.jpg"
        options.append(f"-DNX_GAME_ICON={icon.as_posix() if icon.is_file() else ''}")
    run(["cmake", "-S", "/work", "-B", build, "-G", "Ninja", *options], log)
    # the RHI conformance NRO (M05) is public: no game code, built in both modes
    targets = (["nx_core", "nx_self_registering", "nx_entry_check"] if args.public else ["gtavcs_nro"]) + ["gtavcs_rhi_tests_nro"]
    run(["cmake", "--build", build, "--parallel", str(args.jobs), "--target", *targets, "--",
         "-k", "0"], log)
    if args.public:
        commands = (build / "compile_commands.json").read_text()
        if "/generated/" in commands or "libvcs_generated_cpu" in commands:
            raise RuntimeError("Private generated input appeared in the public build")
        stage_rhi_tests(build)
        print("PASS public core library compiled; no game code linked, no NRO", flush=True)
        return
    stage_rhi_tests(build)
    out = Path("/work") / PRIVATE.relative_to(ROOT)
    out.mkdir(parents=True, exist_ok=True)
    for name in ("gtavcs.nro", "gtavcs.map", "gtavcs.elf", "compile_commands.json"):
        shutil.copyfile(build / name, out / name)
    linkmap = (build / "gtavcs.map").read_text(errors="replace")
    manifest = {
        "nro_sha256": file_digest(build / "gtavcs.nro"),
        "nro_bytes": (build / "gtavcs.nro").stat().st_size,
        "archive_sha256": file_digest(ARCHIVE),
        "core_stub_linked": "core_stub" in linkmap,
        "generated_linked": "libvcs_generated_cpu.a" in linkmap,
        "wrapped_thread_create": "__wrap___syscall_thread_create" in linkmap,
        # applyVcs registers itself statically; without this object the game boots with none of its hooks
        "game_overrides_linked": "(vcs_overrides.o)" in linkmap,
        "vu0_natives_linked": "(vcs_vu0_native.o)" in linkmap,
        "unlinked_constructors": unlinked_constructors(build, linkmap),
    }
    if (manifest["core_stub_linked"] or not manifest["generated_linked"] or not manifest["wrapped_thread_create"]
            or not manifest["game_overrides_linked"] or not manifest["vu0_natives_linked"]
            or manifest["unlinked_constructors"]):
        raise RuntimeError(f"Unexpected link map: {manifest}")
    (out / "manifest-inside.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("PASS private game NRO " + manifest["nro_sha256"], flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--public", action="store_true")
    parser.add_argument("--inside", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--source", help=argparse.SUPPRESS)
    parser.add_argument("--sse2neon", help=argparse.SUPPRESS)
    parser.add_argument("--imgui", help=argparse.SUPPRESS)
    parser.add_argument("--jobs", type=int, default=1, help="compile jobs and container CPUs (default 1)")
    args = parser.parse_args()
    LOGS.mkdir(parents=True, exist_ok=True)
    name = ("public" if args.public else "private") + ("-container" if args.inside else "")
    try:
        with (LOGS / f"{name}.log").open("w", encoding="utf-8") as log:
            if args.inside:
                inside(args, log)
                return 0
            sse2neon, _ = sse2neon_dependency(log)
            imgui = imgui_dependency(log)
            series = (ROOT / "patches/upstream/series.json").read_bytes()
            source = ROOT / ("build/m04/game-source-" + digest(series)[:16])
            prepare(source)
            if not args.public:
                (PRIVATE / "gtavcs.nro").unlink(missing_ok=True)
            run(["docker", "build", "--network=none", f"--cpu-quota={args.jobs * 100000}", "-f", "Dockerfile.switch", "-t", IMAGE, "."], log)
            command = ["docker", "run", "--rm", "--network=none", f"--cpus={args.jobs}",
                       "--mount", f"type=bind,source={ROOT},target=/work",
                       "--mount", f"type=volume,source={VOLUME},target=/cache", "-w", "/work", IMAGE,
                       "python3", "tools/build_game.py", "--inside",
                       "--source", source.relative_to(ROOT).as_posix(),
                       "--sse2neon", sse2neon.relative_to(ROOT).as_posix(),
                       "--imgui", imgui.relative_to(ROOT).as_posix(), "--jobs", str(args.jobs)]
            if args.public:
                command.append("--public")
            try:
                run(command, log)
            finally:
                reclaim(ROOT, PRIVATE, LOGS, RHI_TESTS)
            if not args.public:
                manifest = json.loads((PRIVATE / "manifest-inside.json").read_text())
                manifest.update({"root_commit": git(ROOT, "rev-parse", "HEAD"),
                                 "root_dirty": bool(git(ROOT, "status", "--porcelain")),
                                 "overlay_series_sha256": digest(series), "overlay_checkout": source.name,
                                 "icon_sha256": file_digest(ROOT / "icon.jpg") if (ROOT / "icon.jpg").is_file() else None})
                (PRIVATE / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        return 0
    except (OSError, RuntimeError, subprocess.CalledProcessError, ValueError, KeyError) as error:
        print(f"FAIL game build: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
