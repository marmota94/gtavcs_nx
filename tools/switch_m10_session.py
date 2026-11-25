"""M10 hardware session: switch_m09_session.py's game run, plus the busy-route numbers M10-04 asks for.

    python tools/switch_m10_session.py preflight <evidence-dir>
    python tools/switch_m10_session.py game <evidence-dir> <run.cfg> [--label NAME] [--prompt TEXT] [--keep-card]
                                                                     [--settings vcs.ini]
    python tools/switch_m10_session.py route <evidence-dir> <label> [--from 90] [--to 390]

`game` is switch_m09_session.py's (screenshots privately under build/evidence/M10/screenshots/<label>). `route` reads
the run's perf.csv / gpu.csv / log into <label>-route.txt for the window FROM-TO (perf.csv t_s, the running sum of
game frame times):

- the mode the run started in (the log's `[video] output WxH` lines: 1280x720 handheld, 1920x1080 docked) and each
  change;
- game frames: count, frames/s (count / window), median / p95 / p99 / max frame time, frames over 40 / 100 / 250 ms
  and the longest slow interval (consecutive frames over 40 ms, summed);
- presented frames from gpu.csv (one row per present) and their GL_TIME_ELAPSED median / p95;
- heap in use / obtained at the window's first and last progress line, GL errors, crash reports.

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


def main():
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
