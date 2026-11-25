"""Build/stage only public M02 CPU/runtime test NROs, never private game code.

Fetch the locked public dependency and prepare a reversible upstream overlay on
the host. Compilation runs offline in the locked Switch container. Immutable
public-only source snapshots and dedicated ext4 build paths avoid private caches.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from build_switch import IMAGE, PIN, reclaim
from upstream_overlay import prepare

ROOT = Path(__file__).resolve().parents[1]
VOLUME = "gtavcs-nx-m02-core"
LOGS = ROOT / "build/evidence/M02/public-build"
BUNDLE = ROOT / "build/m02/test-bundle"
PUBLIC_NROS = ("gtavcs_cpu_tests", "gtavcs_runtime_tests", "gtavcs_disc_tests")
BUNDLE_NAMES = {name + ".nro" for name in PUBLIC_NROS} | {
    "manifest.json", "LICENSE.SSE2NEON.txt", "LICENSE.UPSTREAM.txt", "LICENSE.PS2RECOMP.txt"}
UPSTREAM_PATHS = (
    "third_party/PS2Recomp/ps2xRuntime", "third_party/PS2Recomp/ps2xIOP",
    "third_party/PS2Recomp/ps2xTest", "third_party/PS2Recomp/ps2xRecomp/include",
    "src/platform/platform.h", "src/rhi/rhi.h",
    "src/rhi/rhi.cpp", "src/rhi/null/rhi_null.cpp", "src/render/presenter.h",
    "src/render/gs_shaders.h", "src/debug/watch.cpp", "tools/vu1_recomp.py")
PORT_PATHS = ("cmake/core-tests", "cmake/runtime", "cmake/CPUOptions.cmake",
              "src/core_tests", "src/compat", "src/vfs", "tests/core", "tools/core-dependencies.json")
TOOL_PATHS = ("tools/build_core_tests.py", "tools/build_switch.py", "tools/upstream_overlay.py",
              "tools/inspect_cpu.py", "tools/generate_vu_test_fixture.py", "Dockerfile.switch",
              "docs/milestones/M01/package-lock.txt", "patches/upstream")
# compile jobs and container CPUs; 1 by default (the host is a shared NAS), CI raises it
JOBS = max(1, int(os.environ.get("NX_BUILD_JOBS", "1")))


def digest(data):
    return hashlib.sha256(data).hexdigest()


def snapshot_key(manifest):
    identity = {"port":manifest["port_inputs"], "upstream":manifest["upstream_inputs"],
                "tooling":manifest["tooling_inputs"], "dependency":manifest["dependency"]}
    return digest(json.dumps(identity, sort_keys=True).encode())


def run(args, log):
    log.write("+ " + " ".join(map(str, args)) + "\n")
    log.flush()
    process = subprocess.Popen(list(map(str, args)), cwd=ROOT, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
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


def below_build(path):
    resolved = path.resolve()
    if path.is_symlink() or not resolved.is_relative_to((ROOT / "build").resolve()):
        raise RuntimeError(f"Expected a regular workspace build path: {path}")
    return resolved


def dependency(log):
    lock = json.loads((ROOT / "tools/core-dependencies.json").read_text())["sse2neon"]
    target = below_build(ROOT / "build/m02/deps/sse2neon")
    if not target.exists():
        target.mkdir(parents=True)
        run(["git", "init", str(target)], log)
        run(["git", "-C", str(target), "remote", "add", "origin", lock["repository"]], log)
        run(["git", "-C", str(target), "fetch", "--depth", "1", "origin", lock["commit"]], log)
        run(["git", "-C", str(target), "checkout", "--detach", lock["commit"]], log)
    if git(target, "rev-parse", "HEAD") != lock["commit"]:
        raise RuntimeError("SSE2NEON checkout is not the locked commit")
    if git(target, "status", "--porcelain", "--untracked-files=normal"):
        raise RuntimeError("SSE2NEON checkout has unowned changes")
    if digest((target / "sse2neon.h").read_bytes()) != lock["header_sha256"]:
        raise RuntimeError("SSE2NEON header checksum mismatch")
    return target, lock


def selected_files(root, paths, tracked=False):
    files = set()
    for relative in paths:
        path = root / relative
        if path.is_file():
            files.add(path)
        elif path.is_dir():
            files.update(p for p in path.rglob("*") if p.is_file())
        else:
            raise RuntimeError(f"Missing public source path: {path}")
    if tracked:
        owned = set(git(root, "ls-files").splitlines())
        if any(p.relative_to(root).as_posix() not in owned for p in files):
            raise RuntimeError("Untracked input in disposable upstream source")
    for path in files:
        if path.is_symlink() or not path.resolve().is_relative_to(root.resolve()):
            raise RuntimeError(f"Source escape: {path}")
        if path.suffix.lower() in {".iso", ".elf", ".nro", ".nacp", ".a", ".o", ".obj"}:
            raise RuntimeError(f"Binary/private input forbidden in public snapshot: {path}")
    return {p.relative_to(root).as_posix(): digest(p.read_bytes()) for p in sorted(files)}


def stage_clear():
    below_build(BUNDLE)
    BUNDLE.mkdir(parents=True, exist_ok=True)
    if any(p.name not in BUNDLE_NAMES or p.is_symlink() or not p.is_file() for p in BUNDLE.iterdir()):
        raise RuntimeError("Public bundle contains unowned files; preserving them")
    # Only these specifically named public output files are replaceable.
    for name in BUNDLE_NAMES:
        (BUNDLE / name).unlink(missing_ok=True)


def snapshot(inputs, source, destination):
    if destination.is_symlink():
        raise RuntimeError("Public source snapshot root is a symlink")
    destination.mkdir(parents=True, exist_ok=True)
    actual = {p.relative_to(destination).as_posix() for p in destination.rglob("*") if p.is_file()}
    if actual - inputs.keys():
        raise RuntimeError("Unexpected files in immutable public source cache")
    for relative, checksum in inputs.items():
        src, dst = source / relative, destination / relative
        if Path(relative).is_absolute() or ".." in Path(relative).parts or src.is_symlink() or not src.resolve().is_relative_to(source.resolve()):
            raise RuntimeError("Public source snapshot input escapes its source root")
        data = src.read_bytes()
        if digest(data) != checksum:
            raise RuntimeError(f"Source changed during snapshot; rerun after edits: {relative}")
        if dst.is_symlink() or not dst.resolve().is_relative_to(destination.resolve()):
            raise RuntimeError("Public source cache escape")
        if dst.exists() and digest(dst.read_bytes()) != checksum:
            raise RuntimeError("Modified immutable public source cache; preserving it")
        if not dst.exists():
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(data)


def inside(manifest_path, log):
    manifest = json.loads(manifest_path.read_text())
    if run(["dkp-pacman", "-Q"], log) != (ROOT / "docs/milestones/M01/package-lock.txt").read_text():
        raise RuntimeError("Toolchain package lock mismatch")
    key = manifest["snapshot_key"]
    if key != snapshot_key(manifest):
        raise RuntimeError("Public snapshot identity mismatch")
    cache = Path("/cache/public-m02") / key
    if cache.is_symlink() or not cache.resolve().is_relative_to(Path("/cache").resolve()):
        raise RuntimeError("Public build cache escapes the mounted cache volume")
    port, upstream, deps = cache / "port", cache / "upstream", cache / "deps"
    source = ROOT / manifest["overlay_checkout"]
    below_build(source)
    snapshot(manifest["port_inputs"], ROOT, port)
    snapshot(manifest["upstream_inputs"], source, upstream)
    snapshot(manifest["tooling_inputs"], ROOT, cache / "tooling")
    dep_source = ROOT / "build/m02/deps/sse2neon"
    snapshot({"sse2neon.h": manifest["dependency"]["header_sha256"]}, dep_source, deps)
    inspections = {}
    for project, stem, title in (("core-tests", PUBLIC_NROS[0], "GTA VCS NX - CPU tests"),
                                 ("runtime", PUBLIC_NROS[1], "GTA VCS NX - Runtime tests"),
                                 ("runtime", PUBLIC_NROS[2], "GTA VCS NX - Disc tests")):
        build = cache / project
        options = ["-DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake", "-DCMAKE_BUILD_TYPE=Release",
                   f"-DVCS_UPSTREAM_DIR={upstream}", f"-DSSE2NEON_INCLUDE_DIR={deps}"]
        if project == "runtime":
            options += ["-DNX_NATIVE_REFERENCE=OFF", "-DNX_RUNTIME_TESTS=ON",
                        "-DNX_EXTENDED_CPU_TESTS=ON", "-DNX_PRIVATE_GENERATED_ARCHIVE=",
                        "-DNX_BASELINE_IMGUI_DIR="]
        run(["cmake", "-S", port / "cmake" / project, "-B", build, "-G", "Ninja", *options], log)
        run(["cmake", "--build", build, "--parallel", str(JOBS),
             "--target", stem + "_nro"], log)
        commands = (build / "compile_commands.json").read_text()
        linkmap = (build / (stem + ".map")).read_text()
        if "/generated/" in commands or "libvcs_generated_cpu" in linkmap or "gtavcs_private_core" in linkmap:
            raise RuntimeError("Private generated input appeared in public test pipeline")
        if project == "runtime" and not re_cache_empty(build / "CMakeCache.txt", "NX_PRIVATE_GENERATED_ARCHIVE"):
            raise RuntimeError("Private archive cache entry was not cleared")
        inspection = run(["python3", cache / "tooling/tools/inspect_cpu.py", "--build-dir", build,
                          "--stem", stem, "--title", title, "--compile-commands", build / "compile_commands.json",
                          "--map", build / (stem + ".map"), "--allow-root", port, "--allow-root", upstream,
                          "--allow-root", deps, "--allow-root", build], log)
        inspections[stem] = json.loads(inspection)
        shutil.copyfile(build / (stem + ".nro"), BUNDLE / (stem + ".nro"))
    manifest["inspections"] = inspections
    for name, source_path in {
        "LICENSE.SSE2NEON.txt": dep_source / "LICENSE",
        "LICENSE.UPSTREAM.txt": source / "LICENSE",
        "LICENSE.PS2RECOMP.txt": source / "third_party/PS2Recomp/LICENSE",
    }.items():
        shutil.copyfile(source_path, BUNDLE / name)
    manifest["public_artifacts"] = {p.name: {"sha256": digest(p.read_bytes()), "bytes": p.stat().st_size}
                                    for p in BUNDLE.iterdir() if p.name != "manifest.json"}
    (BUNDLE / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("PASS public CPU/runtime NRO bundle; no private game inputs or hardware PASS claimed", flush=True)


def re_cache_empty(path, name):
    entries = [line.split("=", 1)[1] for line in path.read_text().splitlines()
               if line.startswith(name + ":") and "=" in line]
    return entries == [""]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inside", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    LOGS.mkdir(parents=True, exist_ok=True)
    try:
        with (LOGS / ("container.log" if args.inside else "prepare.log")).open("w", encoding="utf-8") as log:
            if args.inside:
                inside(below_build(args.inside), log)
                return 0
            stage_clear()
            _, lock = dependency(log)
            series = (ROOT / "patches/upstream/series.json").read_bytes()
            overlay = below_build(ROOT / ("build/m02/public-source-" + digest(series)[:16]))
            prepare(overlay)
            # The checked-in fixture is generated only from public synthetic
            # instruction encodings. CI checks exact reproduction and never
            # regenerates/accepts a different golden output automatically.
            fixture_check = run([sys.executable, ROOT / "tools/generate_vu_test_fixture.py",
                                 "--generator", overlay / "tools/vu1_recomp.py", "--check"], log)
            port_inputs = selected_files(ROOT, PORT_PATHS)
            upstream_inputs = selected_files(overlay, UPSTREAM_PATHS, tracked=True)
            tooling_inputs = selected_files(ROOT, TOOL_PATHS)
            manifest = {"scope": "Public synthetic CPU and upstream runtime tests only; no generated game code/assets",
                        "root_commit": git(ROOT, "rev-parse", "HEAD"),
                        "root_dirty": bool(git(ROOT, "status", "--porcelain")),
                        "toolchain_base": PIN, "dependency": lock,
                        "overlay_manifest_sha256": digest(series),
                        "overlay_checkout": overlay.relative_to(ROOT).as_posix(),
                        "synthetic_vu_fixture_check": fixture_check.strip(),
                        "port_inputs": port_inputs, "upstream_inputs": upstream_inputs,
                        "tooling_inputs": tooling_inputs}
            manifest["snapshot_key"] = snapshot_key(manifest)
            manifest_path = LOGS / "public-inputs.json"
            manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
            run(["docker", "build", "--network=none", f"--cpu-quota={JOBS * 100000}", "-f", "Dockerfile.switch", "-t", IMAGE, "."], log)
            try:
                run(["docker", "run", "--rm", "--network=none", f"--cpus={JOBS}", "-e", f"NX_BUILD_JOBS={JOBS}", "--mount", f"type=bind,source={ROOT},target=/work",
                     "--mount", f"type=volume,source={VOLUME},target=/cache", "-w", "/work", IMAGE,
                     "python3", "tools/build_core_tests.py", "--inside",
                     "/work/" + manifest_path.relative_to(ROOT).as_posix()], log)
            finally:
                reclaim(ROOT, BUNDLE, LOGS)
        return 0
    except (OSError, RuntimeError, subprocess.CalledProcessError, ValueError, KeyError, TypeError) as error:
        # No stale NRO may be mistaken for a newly validated public bundle.
        if BUNDLE.exists() and not BUNDLE.is_symlink():
            for name in BUNDLE_NAMES:
                path = BUNDLE / name
                if path.is_file() and not path.is_symlink():
                    path.unlink()
        print(f"FAIL public CPU build: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
