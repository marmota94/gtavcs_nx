"""Build and run the RHI conformance scenes on this host: upstream's Vulkan backend (the desktop reference) and the
port's OpenGL backend, then compare them (tools/rhi_compare.py).

    python tools/rhi_host_tests.py [--deps DIR] [--out build/m05/host] [--churn N] [--no-run]

--deps: upstream's FetchContent folder of a desktop reference build (volk, Vulkan-Headers, VulkanMemoryAllocator at
upstream's pinned tags; default build/m02/reference/build/release/_deps). Needs host clang/gcc, CMake, Ninja,
glslangValidator, EGL (Mesa) and a Vulkan driver (lavapipe on a headless host). No game data, no network.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from glsl_shaders import variants  # noqa: E402
from upstream_overlay import prepare  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def run(args, **kw):
    print("+ " + " ".join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), check=True, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--deps", type=Path, default=ROOT / "build/m02/reference/build/release/_deps")
    ap.add_argument("--out", type=Path, default=ROOT / "build/m05/host")
    ap.add_argument("--churn", type=int, default=200)
    ap.add_argument("--no-run", action="store_true")
    a = ap.parse_args()
    out = a.out.resolve()
    source = ROOT / "build/m05/host-source"
    prepare(source)
    glslang = shutil.which("glslangValidator")
    if not glslang:
        print("FAIL glslangValidator not found", file=sys.stderr)
        return 1
    shaders = out / "shaders"
    (shaders / "spirv").mkdir(parents=True, exist_ok=True)
    cmake_text = (source / "src/render/CMakeLists.txt").read_text()
    for stem, entry, profile, name, header, defines in variants(cmake_text):
        target = shaders / "spirv" / header
        src = source / "src/render/shaders" / (stem + ".hlsl")
        if target.exists() and target.stat().st_mtime > src.stat().st_mtime:
            continue
        args = [sys.executable, "-I", source / "tools/hlsl_spirv.py", "--glslang", glslang,
                "--stage", "vert" if profile.startswith("vs") else "frag", "--entry", entry,
                "--name", name + "_spv", "--out", target]
        for d in defines:
            args += ["-D", d]
        run(args + [src], stdout=subprocess.DEVNULL)
    run([sys.executable, ROOT / "tools/glsl_shaders.py", "--upstream", source, "--out", shaders / "glsl/gs_shaders_glsl.h",
         "--manifest", out / "shader-manifest.json"])
    build = out / "build"
    deps = a.deps.resolve()
    run(["cmake", "-S", ROOT / "tests/rhi", "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
         f"-DVCS_UPSTREAM_DIR={source}", f"-DVULKAN_HEADERS_DIR={deps / 'vulkanheaders-src/include'}",
         f"-DVOLK_DIR={deps / 'volk-src'}", f"-DVMA_DIR={deps / 'vulkanmemoryallocator-src/include'}",
         f"-DSHADER_HEADERS_DIR={shaders}", f"-DIMGUI_DIR={deps / 'imgui-src'}"], stdout=subprocess.DEVNULL)
    run(["cmake", "--build", build, "--parallel", os.environ.get("NX_BUILD_JOBS", "1")])  # the host is a shared NAS
    if a.no_run:
        return 0
    # the desktop reference: upstream's Vulkan backend on this host's GPU (Mesa ANV / RADV, whatever the loader picks
    # first among hardware ICDs) and on lavapipe (a second, software implementation); the port's OpenGL backend
    icds = Path("/usr/share/vulkan/icd.d")
    runs = [("vulkan", "vulkan", {"VK_ICD_FILENAMES": str(icds / "lvp_icd.json")})]
    hardware = [p for p in (icds / "intel_icd.json", icds / "radeon_icd.json") if p.exists()]
    if hardware:
        runs.insert(0, ("vulkan_hw", "vulkan", {"VK_ICD_FILENAMES": ":".join(map(str, hardware))}))
    runs.append(("opengl", "opengl", {}))
    status = 0
    for name, backend, extra in runs:
        result = out / name
        shutil.rmtree(result, ignore_errors=True)
        with open(out / f"{name}.stderr.log", "w") as log:
            code = subprocess.run([build / "rhi_conformance", "--backend", backend, "--out", result, "--churn", str(a.churn)],
                                  env=dict(os.environ, **extra), stderr=log).returncode
        print(f"{name}: exit {code}; {(result / 'log.txt').read_text().splitlines()[0] if (result / 'log.txt').exists() else ''}")
        status |= code != 0
    code = 0
    for ref in [r[0] for r in runs if r[1] == "vulkan"]:
        code |= subprocess.run([sys.executable, ROOT / "tools/rhi_compare.py", out / ref, out / "opengl",
                                "--report", out / f"compare-{ref}-opengl.json"]).returncode
    return 1 if status or code else 0


if __name__ == "__main__":
    raise SystemExit(main())
