"""Run a Switch run schedule (tests/switch/*.cfg, the nxrun.cfg format) on the desktop reference.

    python tools/m04_reference.py tests/switch/m04-boot.cfg <evidence-dir> [--iso PATH] [--rhi null|d3d11|vulkan|opengl]
        [--setting KEY=VALUE ...]

--rhi (default null, M04's comparison) picks the reference's renderer (d3d11: a Windows reference build, M09);
`opengl` needs the reference built with the port's backend (tests/rhi/desktop-opengl.patch, M05). A schedule's
`screenshot <path>` commands write to <evidence-dir>/screenshots/<file name> instead (game pictures: keep the
evidence folder under build/). --setting (M09) starts the reference with a setting's value as a saved one, not an
environment override, so the schedule's `setting` commands can still change it (the Switch's defaults, e.g.
display.aspect=169): the lines go into the reference's vcs.ini for the run, and the file is restored afterwards.

The reference is upstream's own desktop build in build/m02/reference (M02-01), started hidden with VCS_RHI=null and
an empty card of its own (VCS_DATA_DIR under the evidence folder; never the player's). The schedule's commands go
over the control channel at the same times the Switch core sends them; answers and progress are written in the
Switch core's results format (results.txt) next to the game's log, for tools/m04_compare.py.
"""
import argparse
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
REFERENCE = ROOT / "build/m02/reference"
PORT = 47681


def parse(path):
    """The subset of src/core/run_config.cpp this tool needs (that parser validates the same file on the Switch)."""
    config = {"env": [], "seconds": 0.0, "report": 5.0, "commands": [], "profile": ""}
    for number, raw in enumerate(Path(path).read_text().splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        verb, _, rest = line.partition(" ")
        rest = rest.strip()
        if verb == "profile":
            config["profile"] = rest
        elif verb == "env":
            name, _, value = rest.partition("=")
            config["env"].append((name, value))
        elif verb in ("seconds", "report"):
            config[verb] = float(rest)
        elif verb == "at":
            at, _, command = rest.partition(" ")
            config["commands"].append((float(at), command.strip()))
        elif verb == "every":
            period, start, end, command = rest.split(" ", 3)
            t = float(start)
            while t <= float(end) + 1e-9:
                config["commands"].append((t, command.strip()))
                t += float(period)
        elif verb == "verify-files":
            pass
        else:
            raise SystemExit(f"{path}:{number}: unknown directive {verb}")
    config["commands"].sort(key=lambda c: c[0])
    return config


def send(command, timeout=70.0):
    with socket.create_connection(("127.0.0.1", PORT), timeout=timeout) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        s.sendall((command + "\n").encode())
        data = b""
        while not data.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    return data.decode("utf-8", errors="replace").strip()


def private_bytes(process):
    """The reference's private bytes (Windows' commit charge of the process; None elsewhere): its heap, for M06-12."""
    if os.name != "nt":
        return None
    import ctypes
    from ctypes import wintypes

    class Counters(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + \
                   [(name, ctypes.c_size_t) for name in ("PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
                                                         "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage",
                                                         "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage",
                                                         "PrivateUsage")]

    handle = ctypes.windll.kernel32.OpenProcess(0x1000, False, process.pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return None
    try:
        counters = Counters()
        counters.cb = ctypes.sizeof(counters)
        ok = ctypes.windll.psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb)
        return counters.PrivateUsage if ok else None
    finally:
        ctypes.windll.kernel32.CloseHandle(handle)


def quote(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", " ") + '"'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("config")
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--iso", type=Path)
    parser.add_argument("--rhi", default="null", choices=["null", "d3d11", "vulkan", "opengl"])
    parser.add_argument("--setting", action="append", default=[], metavar="KEY=VALUE")
    args = parser.parse_args()
    config = parse(args.config)
    iso = args.iso or next(iter(sorted(ROOT.glob("*.iso"))), None)
    # upstream's tools/build.py writes build/vcs.exe on Windows and build/vcs on Linux
    exe = next((p for p in (REFERENCE / "build/vcs.exe", REFERENCE / "build/vcs") if p.is_file()), None)
    if not iso or not exe:
        raise SystemExit("Need the reference build (build/m02/reference/build/vcs.exe or build/vcs) and the disc image")
    evidence = args.evidence.resolve()
    data = evidence / "reference-userdata"
    if data.exists():
        shutil.rmtree(data)
    data.mkdir(parents=True)
    env = dict(os.environ)
    env.update({"PS2X_HIDDEN": "1", "PS2X_CONTROL": "1", "PS2X_CONTROL_PORT": str(PORT), "VCS_RHI": args.rhi,
                "VCS_DATA_DIR": str(data)})
    env.update(dict(config["env"]))
    log = open(evidence / "reference-game.log", "w", encoding="utf-8", errors="replace")
    results = open(evidence / "reference-results.txt", "w", encoding="utf-8")

    def result(t, kind, fields):
        line = f"{kind} t={t:.3f} {fields}"
        print(line, flush=True)
        results.write(line + "\n")
        results.flush()

    ini = REFERENCE / "build/vcs.ini"
    saved_ini = ini.read_bytes() if ini.exists() else None
    if args.setting:
        text = (saved_ini or b"").decode()
        ini.write_text(text + ("" if not text or text.endswith("\n") else "\n") + "\n".join(args.setting) + "\n")
    try:
        process = subprocess.Popen([str(exe), str(iso)], cwd=REFERENCE / "build", env=env, stdout=log,
                                   stderr=subprocess.STDOUT)
    except OSError:
        if saved_ini is not None:
            ini.write_bytes(saved_ini)
        raise
    try:
        start = time.time() + 60
        while time.time() < start:
            try:
                if send("ping", timeout=2.0) == "ok":
                    break
            except OSError:
                time.sleep(0.2)
        else:
            raise SystemExit("The reference never answered on the control channel")
        t0 = time.time()
        result(0.0, "config", f"profile={quote(config['profile'])} seconds={config['seconds']} "
                              f"commands={len(config['commands'])} renderer={args.rhi} settings={quote(' '.join(args.setting))}")
        pending = list(config["commands"])
        next_report = 0.0
        while True:
            now = time.time() - t0
            if process.poll() is not None:
                result(now, "stop", f"reason=exited code={process.returncode}")
                break
            if now >= next_report:
                frames = send("frames")
                used = private_bytes(process)
                result(now, "progress", ("game_frames=" + frames.split()[-1] if frames.startswith("ok") else frames) +
                       ("" if used is None else f" private_bytes={used}"))
                result(now, "perf", quote(send("perf")))
                next_report = now + config["report"]
            while pending and pending[0][0] <= now:
                at, command = pending.pop(0)
                if command.startswith("screenshot "):
                    shots = evidence / "screenshots"
                    shots.mkdir(exist_ok=True)
                    command = "screenshot " + str(shots / Path(command.split(" ", 1)[1]).name)
                try:
                    answer = send(command)
                except OSError as error:
                    answer = f"error {error}"
                result(time.time() - t0, "command", f"at={at:.6f} command={quote(command)} "
                       f"status={'ok' if answer.startswith('ok') else 'error'} reply={quote(answer)}")
            if config["seconds"] and now >= config["seconds"]:
                result(now, "stop", "reason=seconds")
                break
            time.sleep(0.05)
    finally:
        try:
            send("quit", timeout=5.0)
        except OSError:
            pass
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
        results.close()
        log.close()
        if args.setting:
            if saved_ini is None:
                ini.unlink()
            else:
                ini.write_bytes(saved_ini)
    return 0


if __name__ == "__main__":
    sys.exit(main())
