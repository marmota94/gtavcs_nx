"""M10 hardware session: switch_m09_session.py's game run, plus the busy-route numbers M10-04 asks for.

    python tools/switch_m10_session.py preflight <evidence-dir>
    python tools/switch_m10_session.py game <evidence-dir> <run.cfg> [--label NAME] [--prompt TEXT] [--keep-card]
                                                                     [--settings vcs.ini]
    python tools/switch_m10_session.py route <evidence-dir> <label> [--from 90] [--to 390]
    python tools/switch_m10_session.py profile <evidence-dir> <label> [--elf build/m04/private/gtavcs.elf] [--top 40]

`game` is switch_m09_session.py's (screenshots privately under build/evidence/M10/screenshots/<label>). `route` reads
the run's perf.csv / gpu.csv / log into <label>-route.txt for the window FROM-TO (perf.csv t_s, the running sum of
game frame times):

- the mode the run started in (the log's `[video] output WxH` lines: 1280x720 handheld, 1920x1080 docked) and each
  change;
- game frames: count, frames/s (count / window), median / p95 / p99 / max frame time, frames over 40 / 100 / 250 ms
  and the longest slow interval (consecutive frames over 40 ms, summed);
- presented frames from gpu.csv (one row per present) and their GL_TIME_ELAPSED median / p95;
- heap in use / obtained at the window's first and last progress line, GL errors, crash reports.

`profile` reads a run's sampler output (<label>/samples.txt: VCS_NX_SAMPLE_THREAD in the run file, src/switch/sampler.c)
and resolves each sampled pc and lr against the run's ELF symbols (the toolchain's nm in the container; the anchor
symbol gtavcs_sampler_start gives the load offset) into <label>-profile.txt: the share of samples per function (self),
per caller of the hottest functions, and per generated-code / runtime / library group.

The M10 25+ baseline (docs/MILESTONES.md): >= 25 presented gameplay frames/s over the route with p95 <= 40 ms after
the stated warmup. NX_HOST names the console (default 192.168.1.91).
"""
import argparse
import csv
import os
from pathlib import Path
import re
import statistics
import sys

os.environ.setdefault("NX_HOST", "192.168.1.91")
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import switch_m04_session as m04  # noqa: E402
import switch_m06_session as m06  # noqa: E402
import switch_m09_session as m09  # noqa: E402

m06.SHOTS = ROOT / "build/evidence/M10/screenshots"

# NX_NRO: send this NRO instead of build/m04/private/gtavcs.nro (for example the card's own copy of a candidate whose
# game code is unchanged); its SHA-256 is in the run's log as usual.
_game = m04.game


def _game_with_nro(*args, **kwargs):
    if os.environ.get("NX_NRO"):
        m04.NRO_LOCAL = Path(os.environ["NX_NRO"]).resolve()
    return _game(*args, **kwargs)


m04.game = _game_with_nro


def pct(values, q):
    return m06.percentile(values, q) if values else float("nan")


def rows(path):
    if not path.exists():
        return []
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def route(session, label, start, end):
    run = session.evidence / label
    log_path = session.evidence / f"{label}-gtavcs.txt"
    if not log_path.exists():
        log_path = session.evidence / f"{label}-gtavcs.log"
    log = log_path.read_text(errors="replace") if log_path.exists() else ""
    out = [f"run {label}, window {start:g}-{end:g} s (perf.csv t_s)"]
    modes = re.findall(r"\[video\] output (\d+x\d+) on the (\d+x\d+) surface", log)
    out.append("modes: " + (" -> ".join(m for m, _ in modes) if modes else "no [video] output line"))
    perf = [r for r in rows(run / "perf.csv") if start <= float(r.get("t_s") or -1) <= end]
    frames = [float(r["frame_ms"]) for r in perf]
    ok = True
    if frames:
        span = end - start
        slow, longest = 0.0, 0.0
        for ms in frames:
            slow = slow + ms if ms > 40 else 0.0
            longest = max(longest, slow)
        fps = len(frames) / span
        p95 = pct(frames, 0.95)
        out.append(f"game frames: n={len(frames)} fps={fps:.1f} median={statistics.median(frames):.1f} ms "
                   f"p95={p95:.1f} ms p99={pct(frames, 0.99):.1f} ms max={max(frames):.1f} ms")
        out.append(f"  over 40 ms: {sum(f > 40 for f in frames)}, over 100 ms: {sum(f > 100 for f in frames)}, "
                   f"over 250 ms: {sum(f > 250 for f in frames)}; longest slow interval {longest:.0f} ms")
        out.append(f"  25+ baseline (fps >= 25 and p95 <= 40 ms): {'MET' if fps >= 25 and p95 <= 40 else 'NOT MET'}")
        for key in ("game_ms", "render_ms", "wait_ms", "iop_ms", "host_ms", "host_max_ms", "gl_ms", "host_fps",
                    "tex_create_us", "tex_kb"):
            if key in perf[0]:
                vals = [float(r[key]) for r in perf if r.get(key)]
                out.append(f"  {key}: median={statistics.median(vals):.1f} p95={pct(vals, 0.95):.1f} "
                           f"max={max(vals):.1f}")
        if "underruns" in perf[0]:
            under = [int(float(r["underruns"])) for r in perf if r.get("underruns")]
            out.append(f"  audio underruns (counter): {under[0]} -> {under[-1]} in the window")
    else:
        out.append("game frames: none in the window (no perf.csv?)")
        ok = False
    gpu = rows(run / "gpu.csv")
    if gpu:
        # gpu.csv has no time column: its rows are presents, the window is not applied (the whole run, boot included)
        vals = [float(r["gpu_ms"]) for r in gpu if r.get("gpu_ms")]
        late = vals[len(vals) // 4:]
        out.append(f"gpu.csv (whole run, {len(vals)} presents): GL_TIME_ELAPSED median={statistics.median(vals):.2f} "
                   f"p95={pct(vals, 0.95):.2f} ms; last three quarters median={statistics.median(late):.2f} "
                   f"p95={pct(late, 0.95):.2f} ms")
    heaps = re.findall(r"heap_in_use=(\d+) heap_obtained=(\d+)", log)
    if heaps:
        out.append(f"heap in use / obtained: first {int(heaps[0][0]) / 2**20:.1f} / {int(heaps[0][1]) / 2**20:.1f} MiB, "
                   f"last {int(heaps[-1][0]) / 2**20:.1f} / {int(heaps[-1][1]) / 2**20:.1f} MiB, "
                   f"max in use {max(int(h[0]) for h in heaps) / 2**20:.1f} MiB ({len(heaps)} lines)")
    errors = re.findall(r"gl errors ([1-9]\d*)", log)
    out.append(f"GL error lines with errors: {len(errors)}")
    crashes = list(session.evidence.glob(f"crash-after-{label}*"))
    out.append(f"crash reports after the run: {len(crashes)}")
    text = "\n".join(out) + "\n"
    (session.evidence / f"{label}-route.txt").write_text(text)
    for line in out:
        session.say(line)
    return ok


def elf_symbols(elf):
    """(address, size, name) of the ELF's functions, sorted, via the toolchain container's nm."""
    import subprocess
    elf = Path(elf).resolve()
    out = subprocess.run(["docker", "run", "--rm", "--network=none", "--mount",
                          f"type=bind,source={elf.parent},target=/e,readonly", m04.IMAGE,
                          "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm", "-n", "-S", "-C", "--defined-only",
                          f"/e/{elf.name}"], capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split(" ", 3)
        if len(parts) == 4 and parts[2] in "tTwW":
            syms.append((int(parts[0], 16), int(parts[1], 16), parts[3]))
        elif len(parts) == 3 and parts[1] in "tTwW":
            syms.append((int(parts[0], 16), 0, parts[2]))
    syms.sort()
    return syms


def profile(session, label, elf, top):
    import bisect
    from collections import Counter
    path = session.evidence / label / "samples.txt"
    lines = path.read_text().splitlines()
    head = lines[0]
    anchor = int(re.search(r"anchor ([0-9a-f]+)", head).group(1), 16)
    syms = elf_symbols(elf)
    starts = [a for a, _, _ in syms]
    anchor_sym = next(a for a, _, n in syms if n == "gtavcs_sampler_start")
    delta = anchor - anchor_sym

    def name(addr):
        a = addr - delta
        i = bisect.bisect_right(starts, a) - 1
        if i < 0:
            return f"?{addr:x}"
        s, size, n = syms[i]
        return n if not size or a < s + size else f"{n}+{a - s:#x}?"

    self_count, callers, total = Counter(), {}, 0
    for line in lines[1:]:
        c, pc, lr = line.split()
        c = int(c)
        total += c
        f = name(int(pc, 16))
        self_count[f] += c
        callers.setdefault(f, Counter())[name(int(lr, 16))] += c

    def group(n):
        if n.startswith(("FUN_", "fn_")):
            return "generated game code"
        if n.startswith(("ps2", "PS2", "GS", "VU", "vu1", "Vu", "gs_", "vcs", "ps2_")) or "::" in n:
            return "runtime / port (C++)"
        if n.startswith(("mem", "str", "__", "_")):
            return "libc / compiler"
        return "other (Mesa, SDL, libs)"

    groups = Counter()
    for f, c in self_count.items():
        groups[group(f)] += c
    out = [head, f"total {total} samples; load offset {delta:#x}", "", "by group:"]
    out += [f"  {c * 100 / total:5.1f}%  {g}" for g, c in groups.most_common()]
    out += ["", f"top {top} functions (self):"]
    for f, c in self_count.most_common(top):
        out.append(f"  {c * 100 / total:5.1f}%  {f}")
    out += ["", "callers (lr) of the top 10:"]
    for f, _ in self_count.most_common(10):
        out.append(f"  {f}:")
        for g, c in callers[f].most_common(5):
            out.append(f"      {c * 100 / total:5.1f}%  <- {g}")
    (session.evidence / f"{label}-profile.txt").write_text("\n".join(out) + "\n")
    for line in out[: 14 + min(top, 25)]:
        print(line)
    return True


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "profile":
        ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
        ap.add_argument("command")
        ap.add_argument("evidence")
        ap.add_argument("label")
        ap.add_argument("--elf", default=str(ROOT / "build/m04/private/gtavcs.elf"))
        ap.add_argument("--top", type=int, default=40)
        a = ap.parse_args()
        session = m04.Session(a.evidence)
        return 0 if profile(session, a.label, a.elf, a.top) else 1
    if len(sys.argv) > 1 and sys.argv[1] == "route":
        ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
        ap.add_argument("command")
        ap.add_argument("evidence")
        ap.add_argument("label")
        ap.add_argument("--from", dest="start", type=float, default=90)
        ap.add_argument("--to", dest="end", type=float, default=390)
        a = ap.parse_args()
        session = m04.Session(a.evidence)
        ok = route(session, a.label, a.start, a.end)
        session.say(f"RESULT route {'PASS' if ok else 'FAIL'}")
        return 0 if ok else 1
    return m09.main()


if __name__ == "__main__":
    raise SystemExit(main())
