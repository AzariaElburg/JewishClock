"""
Start (or stop) one of the two simulated clocks that can run side by side:

    C:/pio3/jc_clean/venv/Scripts/python sim/start_clock.py family [--fresh]
    C:/pio3/jc_clean/venv/Scripts/python sim/start_clock.py dev [--fresh] [--build]
    C:/pio3/jc_clean/venv/Scripts/python sim/start_clock.py stop family|dev

family - exactly the build the parents' clock runs (sim/family_version.txt, the
         kept build in C:/pio3/jc_clean/sim/versions/<version>/, never rebuilt), that
         version's page files (from its git tag) and the family's settings,
         events and pictures as last synced (C:/pio3/jc_clean/sim/family/seed/).
         For complaints: reproduce on their build, not on ours.
         Web http://127.0.0.1:8070 (its own Wi-Fi 8071, control 8079).
dev    - the working version: today's code (--build rebuilds it first), Data/.
         Every start is like flashing it over USB: the working build and page
         files go in, its settings, events, pictures and log stay.
         Web http://127.0.0.1:8080 (8081, control 8090).

Each keeps its flash and state between starts (C:/pio3/jc_clean/sim/<name>/clock);
--fresh starts from the seed again. When the family installs an update: set
family_version.txt, and refresh seed/ from a backup of theirs.
"""
import io
import os
import stat
import json
import shutil
import socket
import subprocess
import sys
import tarfile
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SIM = Path("C:/pio3/jc_clean/sim")
PY = "C:/pio3/jc_clean/venv/Scripts/python"
sys.path.insert(0, str(ROOT / "sim"))
from sim_ports import ports  # noqa: E402
CLOCKS = {k: dict(zip(("home", "ap", "ctl"), ports(k))) for k in ("family", "dev")}
TITLES = {"family": "Released", "dev": "Work"}   # first word of each clock's window title
sys.path.insert(0, str(ROOT / "utils"))
import web_files  # noqa: E402


def rmtree(d):
    """Delete a clock's folder: a just-stopped clock may hold it a moment, and
    files copied from git can be read-only."""
    def onerror(func, path, _exc):
        os.chmod(path, stat.S_IWRITE)
        func(path)
    for _ in range(10):
        try:
            shutil.rmtree(d, onerror=onerror)
            return
        except FileNotFoundError:
            return
        except OSError:
            time.sleep(1)
    shutil.rmtree(d, onerror=onerror)


def family_version():
    return (ROOT / "sim" / "family_version.txt").read_text(encoding="utf-8").strip()


def listening(port):
    try:
        socket.create_connection(("127.0.0.1", port), timeout=1).close()
        return True
    except OSError:
        return False


def stop(name):
    """The clock stops without answering /stop - so: running = its control port
    answers; stopped = it no longer does."""
    ctl = CLOCKS[name]["ctl"]
    if not listening(ctl):
        print(name, "was not running")
        return
    try:
        urllib.request.urlopen(urllib.request.Request("http://127.0.0.1:%d/stop" % ctl, data=b""), timeout=5)
    except Exception:
        pass
    for _ in range(20):
        if not listening(ctl) and not listening(CLOCKS[name]["home"]):
            print("stopped", name)
            return
        time.sleep(0.5)
    sys.exit("%s did not stop" % name)


def set_window_title(host_pid, title):
    """Titles the clock's window (the conhost host_pid; the clock's supervisor
    in it shares it with the clock). From a separate windowless helper:
    attaching to another console means leaving one's own."""
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "(Get-CimInstance Win32_Process -Filter 'ParentProcessId=%d' | Where-Object Name -eq 'jcclock.exe').ProcessId" % host_pid],
                         capture_output=True, text=True).stdout.split()
    code = ("import ctypes,sys; k=ctypes.windll.kernel32; k.FreeConsole(); "
            "sys.exit(0 if k.AttachConsole(int(sys.argv[1])) and k.SetConsoleTitleW(sys.argv[2]) else 1)")
    for pid in out:
        subprocess.run([sys.executable, "-c", code, pid, title], creationflags=subprocess.CREATE_NO_WINDOW)


def tag_data(version, dest):
    """Data/ as it is in tag v<version> (the page files that version shipped)."""
    tar = subprocess.run(["git", "archive", "v" + version, "Data"], cwd=ROOT, capture_output=True, check=True).stdout
    tarfile.open(fileobj=io.BytesIO(tar)).extractall(dest)
    return dest / "Data"


def make_fs(name, fs):
    tmp = SIM / name / "staging"
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir(parents=True)
    if name == "family":
        data, seed = tag_data(family_version(), tmp), SIM / "family" / "seed"
    else:
        data, seed = ROOT / "Data", ROOT / "Data"
    web_files.stage(data, fs)
    for f in ("config.json", "events.json"):
        shutil.copy2(seed / f, fs / f)
    for d in ("images", "custom"):
        if (seed / d).is_dir():
            shutil.copytree(seed / d, fs / d, dirs_exist_ok=True)
    shutil.rmtree(tmp, ignore_errors=True)


def update_page(fs):
    """The working page files over the dev clock's (as uploadfs, but keeping its
    own data: settings, events, log, pictures, recycle bin)."""
    tmp = SIM / "dev" / "staging"
    web_files.stage(ROOT / "Data", tmp)
    for f in tmp.rglob("*"):
        rel = f.relative_to(tmp).as_posix()
        if f.is_dir() or rel in ("config.json", "events.json") or rel.split("/")[0] in ("log", "images", "custom"):
            continue
        (fs / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(f, fs / rel)
    shutil.rmtree(tmp, ignore_errors=True)


def start(name, fresh, build):
    p = CLOCKS[name]
    stop(name)
    if name == "family":
        exe = SIM / "versions" / family_version() / "jcclock.exe"
    else:
        if build:
            subprocess.run([PY, str(ROOT / "sim" / "build_clock.py")], check=True)
        exe = SIM / "build" / "jcclock.exe"
    if not exe.exists():
        sys.exit("no build: %s" % exe)
    d = SIM / name / "clock"
    if name == "dev":   # its own copy: a running clock locks its program file, and the next build failed on it
        d.mkdir(parents=True, exist_ok=True)
        for _ in range(20):   # a just-stopped clock may still hold it a moment
            try:
                shutil.copy2(exe, d / "jcclock.exe")
                break
            except PermissionError:
                time.sleep(0.5)
        else:
            shutil.copy2(exe, d / "jcclock.exe")
        exe = d / "jcclock.exe"
    if fresh:
        rmtree(d)
    if not (d / "fs" / "config.json").exists():
        make_fs(name, d / "fs")
    elif name == "dev":
        update_page(d / "fs")
    (d / "state").mkdir(parents=True, exist_ok=True)
    if name == "dev":
        # As a USB flash: the working build becomes the firmware in its flash. The
        # slots remember the last installed version, so without this it went on
        # running an older kept build (2026-11-07).
        ota = d / "state" / "flash" / "ota.json"
        if ota.exists():
            ota.unlink()
    # Its own classic console window (conhost), not a tab of Windows Terminal:
    # tabs cut the title short (the user, 2026-11-07). One window for the clock
    # and its supervisor, kept across the clock's restarts; their output shows
    # there (not in run.log). JC_TITLE: the title (sim_clock_main.cpp).
    host = subprocess.Popen(["conhost.exe", str(exe), "--fs", str(d / "fs"), "--state", str(d / "state"), "--verbose",
                      "--home-port", str(p["home"]), "--ap-port", str(p["ap"]), "--control-port", str(p["ctl"])],
                     cwd=str(d), env=dict(os.environ, JC_TITLE=TITLES[name]),
                     creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP)
    for _ in range(60):
        try:
            s = json.loads(urllib.request.urlopen("http://127.0.0.1:%d/api/status" % p["home"], timeout=3).read())
            print("%s clock up: version %s, http://127.0.0.1:%d" % (name, s.get("firmware"), p["home"]))
            # Builds from before 2026-11-07 (the family's) don't title their
            # window themselves: titled from here (the window outlives their restarts).
            set_window_title(host.pid, "%s v%s - 127.0.0.1:%d" % (TITLES[name], s.get("firmware"), p["home"]))
            return
        except Exception:
            time.sleep(1)
    sys.exit("%s clock did not answer - see its window" % name)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    args = sys.argv[1:]
    if not args or args[0] not in ("family", "dev", "stop"):
        sys.exit(__doc__)
    if args[0] == "stop":
        for n in (args[1:] or ["family", "dev"]):
            stop(n)
        return
    start(args[0], "--fresh" in args, "--build" in args)


if __name__ == "__main__":
    main()
