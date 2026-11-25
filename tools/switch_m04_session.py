"""Run the M04 game NRO and the public runtime suite on the Switch over FTP and nxlink, and collect the evidence.

    python tools/switch_m04_session.py preflight <evidence-dir>
    python tools/switch_m04_session.py tests <evidence-dir>
    python tools/switch_m04_session.py game <evidence-dir> <run.cfg> [--prompt TEXT] [--expect-failure TEXT]
    python tools/switch_m04_session.py cleanup <evidence-dir>

Needs Sphaira in application mode (title override) with FTP and nxlink; the extracted game is already on the card
(tools/switch_m03_session.py upload). `game` copies <run.cfg> to gamefolder/userdata/nxrun.cfg, sends the private
build/m04/private/gtavcs.nro with nxlink, waits until the game has ended and Sphaira's FTP is back, then reads the
logs, the run's results, the userdata tree and the crash reports back - and always deletes nxrun.cfg again, so a
player's next launch is a normal one. --prompt prints what the person at the Switch must do during the run (HOME,
sleep, closing the game). Every step and hash goes to <evidence-dir>/transcript.txt. The NRO holds game code: it is
sent to the console only, never copied anywhere else.
"""
import argparse
import ftplib
import hashlib
import io
import os
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
HOST, PORT = os.environ.get("NX_HOST", "192.168.1.107"), int(os.environ.get("NX_FTP_PORT", "5000"))
SD = "/switch/gtavcs_nx"
GAME = SD + "/gamefolder"
NRO_LOCAL = ROOT / "build/m04/private/gtavcs.nro"
NRO_SD = SD + "/gtavcs.nro"
TESTS_LOCAL = ROOT / "build/m02/test-bundle/gtavcs_runtime_tests.nro"
TESTS_SD = SD + "/gtavcs_runtime_tests.nro"
TESTS_LOG = SD + "/runtime-tests-M02.log"
# The runtime suite's TMPDIR (src/core_tests/runtime_main.cpp). Upstream's tests leave their folders there, with a
# 2 KiB synthetic disc.iso fixture (ps2_runtime_io_tests.cpp), because std::filesystem::remove_all fails on the
# Switch (M03): the session removes the folder after reading the suite back, so preflight finds no disc image.
TESTS_TMP = SD + "/test-work-M02/tmp"
CRASHES = "/atmosphere/crash_reports"
IMAGE = "gtavcs-nx-toolchain:m01"


class Session:
    def __init__(self, evidence):
        self.evidence = Path(evidence)
        self.evidence.mkdir(parents=True, exist_ok=True)
        self.transcript = open(self.evidence / "transcript.txt", "a", encoding="utf-8")

    def say(self, text):
        line = time.strftime("%H:%M:%S ") + text
        print(line, flush=True)
        self.transcript.write(line + "\n")
        self.transcript.flush()


def connect():
    ftp = ftplib.FTP()
    ftp.connect(HOST, PORT, timeout=30)
    ftp.login()
    ftp.set_pasv(True)
    return ftp


def listing(ftp, path):
    rows = []
    try:
        ftp.retrlines("LIST " + path, rows.append)
    except ftplib.error_perm:
        return None
    return rows


def names(rows):
    out = {}
    for row in rows or []:
        parts = row.split(None, 8)
        if len(parts) == 9:
            out[parts[8]] = None if row.startswith("d") else int(parts[4])
    return out


def walk(ftp, path, depth=8):
    """Every file under path with its size, folders as 'd'; bounded depth."""
    found = {}
    for name, size in names(listing(ftp, path)).items():
        full = f"{path}/{name}"
        found[full] = "d" if size is None else size
        if size is None and depth > 0:
            found.update(walk(ftp, full, depth - 1))
    return found


def fetch(session, remote, local):
    digest, size, buffer = hashlib.sha256(), 0, io.BytesIO()
    ftp = connect()
    try:
        ftp.retrbinary("RETR " + remote, buffer.write)
    except ftplib.error_perm as error:
        ftp.quit()
        session.say(f"read-back {remote} missing ({error})")
        return None
    ftp.quit()
    data = buffer.getvalue()
    Path(local).write_bytes(data)
    session.say(f"read-back {remote} bytes={len(data)} sha256={hashlib.sha256(data).hexdigest()}")
    return data


def remote_sha256(remote):
    digest = hashlib.sha256()
    ftp = connect()
    ftp.retrbinary("RETR " + remote, digest.update, 1 << 20)
    ftp.quit()
    return digest.hexdigest()


def put(session, local_bytes, remote):
    ftp = connect()
    ftp.storbinary("STOR " + remote, io.BytesIO(local_bytes))
    ftp.quit()
    session.say(f"wrote {remote} bytes={len(local_bytes)} sha256={hashlib.sha256(local_bytes).hexdigest()}")


def delete(session, remote):
    ftp = connect()
    try:
        ftp.delete(remote)
        session.say(f"deleted {remote}")
    except ftplib.error_perm:
        session.say(f"nothing to delete at {remote}")
    ftp.quit()


def remove_tests_tmp(session):
    """Delete the runtime suite's leftover temp folder (files first, then folders, deepest first)."""
    ftp = connect()
    try:
        try:
            tree = walk(ftp, TESTS_TMP, depth=16)
        except ftplib.error_perm:
            tree = {}
        files = [p for p, kind in tree.items() if kind != "d"]
        folders = sorted((p for p, kind in tree.items() if kind == "d"), key=lambda p: p.count("/"), reverse=True)
        for path in files:
            ftp.delete(path)
        for path in folders:
            ftp.rmd(path)
    finally:
        ftp.quit()
    session.say(f"removed the suite's temp folder contents under {TESTS_TMP}: files={len(files)} folders={len(folders)}")


def record_tree(session, path, name):
    ftp = connect()
    tree = walk(ftp, path)
    ftp.quit()
    rows = [f"{k}\t{v}" for k, v in sorted(tree.items())]
    (session.evidence / name).write_text("\n".join(rows) + "\n", encoding="utf-8")
    session.say(f"tree {path} entries={len(rows)} file={name}")
    return tree


def crash_list(session, name):
    ftp = connect()
    rows = names(listing(ftp, CRASHES))
    ftp.quit()
    (session.evidence / name).write_text("\n".join(sorted(rows)) + "\n", encoding="utf-8")
    return set(rows)


def nxlink(session, local, remote):
    folder = local.parent.resolve()
    name = f"gtavcs-nxlink-{os.getpid()}"
    command = ["docker", "run", "--rm", "--name", name, "--mount",
               f"type=bind,source={folder.as_posix()},target=/p,readonly",
               IMAGE, "/opt/devkitpro/tools/bin/nxlink", "-a", HOST, "-r", "3", "-p", remote, f"/p/{local.name}"]
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    for attempt in range(1, 4):
        try:
            # a 66 MB NRO takes about a minute; nxlink hangs when Sphaira is not on its main screen
            result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=300)
        except subprocess.TimeoutExpired:
            subprocess.run(["docker", "kill", name], capture_output=True)
            session.say(f"nxlink attempt={attempt} hung for 300 s: container killed (is Sphaira on its main screen?)")
            time.sleep(8)
            continue
        session.say(f"nxlink attempt={attempt} exit={result.returncode} output={(result.stdout + result.stderr).strip()!r}")
        if result.returncode == 0:
            return True
        time.sleep(8)
    return False


def wait_back(session, limit, prompt=None):
    """FTP comes back when the payload has ended and Sphaira runs again."""
    start = time.time()
    time.sleep(15)
    if prompt:
        session.say("ACTION FOR THE PERSON AT THE SWITCH: " + prompt)
    while time.time() - start < limit:
        try:
            ftp = connect()
            ftp.quit()
            session.say(f"ftp back after {int(time.time() - start)}s: the payload has ended")
            time.sleep(3)
            return True
        except (OSError, *ftplib.all_errors):
            time.sleep(10)
    session.say(f"TIMEOUT: no FTP after {limit}s (still running, crashed, or Sphaira closed with the game)")
    return False


def preflight(session):
    ftp = connect()
    ftp.quit()
    session.say(f"ftp {HOST}:{PORT} reachable")
    tree = record_tree(session, SD, "sd-tree-preflight.txt")
    isos = [p for p in tree if p.lower().endswith(".iso")]
    session.say(f"disc images under {SD}: {isos or 'none'}")
    manifest = f"{GAME}/.nxdisc/manifest.tsv"
    session.say(f"folder disc manifest present={manifest in tree} elf present={GAME + '/SLUS_215.90' in tree}")
    crash_list(session, "crash-preflight.txt")
    return not isos and manifest in tree


def tests(session):
    local_sha = hashlib.sha256(TESTS_LOCAL.read_bytes()).hexdigest()
    session.say(f"runtime tests local sha256={local_sha}")
    before = crash_list(session, "crash-before-tests.txt")
    delete(session, TESTS_LOG)
    if not nxlink(session, TESTS_LOCAL, TESTS_SD) or not wait_back(session, 1800):
        return False
    data = fetch(session, TESTS_LOG, session.evidence / "runtime-tests.log")
    remove_tests_tmp(session)
    text = (data or b"").decode("utf-8", errors="replace")
    finished = re.findall(r"finished failures=(\d+)", text)
    host_fs = re.findall(r"\[host-fs\] failures=(\d+)", text)
    sd_sha = remote_sha256(TESTS_SD)
    after = crash_list(session, "crash-after-tests.txt")
    ok = finished == ["0"] and host_fs == ["0"] and sd_sha == local_sha and after == before
    session.say(f"runtime suites failures={finished} host_fs={host_fs} sd_matches_local={sd_sha == local_sha} "
                f"new_crashes={sorted(after - before)} ok={ok}")
    return ok


def profile_of(config_text):
    match = re.search(r"^\s*profile\s+(\S+)", config_text, re.M)
    return match.group(1) if match else ""


def game(session, config, prompt, expect_failure):
    config_bytes = Path(config).read_bytes()
    label = Path(config).stem
    # a repeated configuration (the second boot) keeps the first run's files: m04-boot, m04-boot-2, ...
    if (session.evidence / f"{label}-gtavcs.log").exists():
        n = 2
        while (session.evidence / f"{label}-{n}-gtavcs.log").exists():
            n += 1
        label = f"{label}-{n}"
    profile = profile_of(config_bytes.decode())
    userdata = f"{GAME}/userdata" + (f"/tests/{profile}" if profile else "")
    seconds = re.search(r"^\s*seconds\s+([\d.]+)", config_bytes.decode(), re.M)
    limit = (float(seconds.group(1)) if seconds else 900.0) + 600.0
    local_sha = hashlib.sha256(NRO_LOCAL.read_bytes()).hexdigest()
    session.say(f"== {label}: profile={profile or '(none)'} nro sha256={local_sha}")
    before = crash_list(session, f"crash-before-{label}.txt")
    record_tree(session, f"{GAME}/userdata", f"userdata-before-{label}.txt")
    for name in ("gtavcs.log",):
        delete(session, f"{GAME}/{name}")
    delete(session, f"{userdata}/nxrun-results.txt")
    put(session, config_bytes, f"{GAME}/userdata/nxrun.cfg")
    try:
        launched = nxlink(session, NRO_LOCAL, NRO_SD)
        back = launched and wait_back(session, limit, prompt)
    finally:
        try:
            delete(session, f"{GAME}/userdata/nxrun.cfg")
        except (OSError, *ftplib.all_errors) as error:
            session.say(f"WARNING: could not delete nxrun.cfg yet ({error}); run `cleanup` once FTP is back")
    if not back:
        return False
    log = fetch(session, f"{GAME}/gtavcs.log", session.evidence / f"{label}-gtavcs.log") or b""
    fetch(session, f"{SD}/gtavcs-boot.log", session.evidence / f"{label}-gtavcs-boot.log")
    results = fetch(session, f"{userdata}/nxrun-results.txt", session.evidence / f"{label}-results.txt") or b""
    record_tree(session, f"{GAME}/userdata", f"userdata-after-{label}.txt")
    if profile:
        ftp = connect()
        try:
            ini = names(listing(ftp, userdata)).get("vcs.ini")
        except ftplib.all_errors:
            ini = None  # a boot that failed before creating its profile folder (bad configuration) has none
        ftp.quit()
        if ini is not None:
            fetch(session, f"{userdata}/vcs.ini", session.evidence / f"{label}-vcs.ini")
    sd_sha = remote_sha256(NRO_SD)
    after = crash_list(session, f"crash-after-{label}.txt")
    text = log.decode("utf-8", errors="replace")
    stops = re.findall(r"^(?:stop|shutdown|audit) .*$", results.decode("utf-8", errors="replace"), re.M)
    if expect_failure:
        ok = expect_failure in text
        session.say(f"expected failure text present={ok}")
    else:
        ok = "[shutdown] core, context, window and SDL closed" in text
    session.say(f"{label}: clean_exit_line={'[shutdown] core, context, window and SDL closed' in text} "
                f"results_tail={stops} sd_matches_local={sd_sha == local_sha} new_crashes={sorted(after - before)}")
    return ok and sd_sha == local_sha and after == before


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["preflight", "tests", "game", "cleanup"])
    parser.add_argument("evidence")
    parser.add_argument("config", nargs="?")
    parser.add_argument("--prompt")
    parser.add_argument("--expect-failure")
    args = parser.parse_args()
    session = Session(args.evidence)
    try:
        if args.command == "preflight":
            ok = preflight(session)
        elif args.command == "tests":
            ok = tests(session)
        elif args.command == "cleanup":
            delete(session, f"{GAME}/userdata/nxrun.cfg")
            remove_tests_tmp(session)
            ok = True
        else:
            if not args.config:
                parser.error("game needs a run configuration")
            ok = game(session, args.config, args.prompt, args.expect_failure)
    except (OSError, EOFError, *ftplib.all_errors, subprocess.TimeoutExpired) as error:
        session.say(f"session error {type(error).__name__}: {error}")
        ok = False
    session.say(f"{args.command} end ok={ok}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
