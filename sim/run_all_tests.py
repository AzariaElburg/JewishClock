"""
Every test without the clock, in one go - before every release (CLAUDE.md).

    C:/pio3/jc_clean/venv/Scripts/python sim/run_all_tests.py --from 2.1.0 [--package FILE] [--port-offset 100] [--skip NAME ...]

Order: builds (jcsim, jcclock for the current code), the screen simulator's
tests (rain/dew prayer, light learning, when the Shabbat slides show), the date check against the real
clock's screenshots (replay_moments.py, no change allowed but flames), the
four Wi-Fi trips on a simulated clock that starts at --from and installs the
newest package (as the family's clock would), the web answers against the
clock's recorded ones (compare_web.py), every drill (drills.py), and every
control of the web pages in a browser (ui_tests.py - its own clock, 8060 + offset).
Needs: the --from version's build in C:/pio3/jc_clean/sim/versions/ (build_clock.py
--tree), the package from make_update_package.py, the firmware built (pio run). ~1.8 h.
Summary: C:/pio3/jc_clean/sim/all_tests/<time>.txt
"""
import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "sim"))
from sim_ports import ports  # noqa: E402
PY = "C:/pio3/jc_clean/venv/Scripts/python"
OUT = Path("C:/pio3/jc_clean/sim/all_tests")
PIO = [str(Path.home() / ".pioarduino/venv/Scripts/pio.exe"), "run", "-c", "pioarduino.ini"]


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--from", dest="frm", required=True, help="the version the update trip starts from, e.g. 2.1.0")
    ap.add_argument("--package", help="default: the newest in C:/pio3/jc_clean/dist")
    ap.add_argument("--port-offset", type=int, default=0)
    ap.add_argument("--skip", nargs="*", default=[])
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    summary = OUT / (time.strftime("%Y%m%d_%H%M") + ".txt")
    off = a.port_offset
    lines = []

    def step(name, cmd, ok_text=None, timeout=3 * 3600):
        if name in a.skip:
            return
        t0 = time.time()
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)
        out = (r.stdout or "") + (r.stderr or "")
        (OUT / (summary.stem + "_" + name + ".log")).write_text(out, encoding="utf-8")
        last = [l for l in out.splitlines() if l.strip()][-1:] or [""]
        ok = r.returncode == 0 and (ok_text is None or ok_text in out)
        line = "%s %-14s %5.0f s  %s" % ("PASS" if ok else "FAIL", name, time.time() - t0, last[0][:120])
        lines.append(line)
        print(line, flush=True)
        summary.write_text("\n".join(lines) + "\n", encoding="utf-8")

    # This run's own firmware and update package (2026-11-08: the newest package in
    # dist was used - an older release - and the trip clock got the last USB upload's
    # page files: stale on both counts). The package goes into this run's folder,
    # never next to the ones that get emailed; --package tests a given one instead.
    os.environ["PLATFORMIO_CORE_DIR"] = "C:/pio3"
    step("build-firmware", PIO, "[SUCCESS]")
    pkg_dir = OUT / (summary.stem + "_package")
    if a.package:
        pkg = a.package
    else:
        step("package", [PY, "utils/make_update_package.py", "--out", str(pkg_dir)], "->")
        found = sorted(pkg_dir.glob("*.jcupdate"))
        pkg = str(found[-1]) if found else ""
    step("build-jcsim", [PY, "sim/build.py"], "OK:")
    step("build-clock", [PY, "sim/build_clock.py"], "OK:")
    step("prayer", [PY, "sim/test_prayer.py"], "ALL CORRECT")
    step("brightness", [PY, "sim/test_brightness.py"], "ALL CORRECT")
    step("show-windows", [PY, "sim/test_show_windows.py"], "ALL CORRECT")
    step("show-combos", [PY, "sim/test_show_combos.py"], "ALL CORRECT")
    step("dates", [PY, "sim/replay_moments.py", "--expect-changed", "#"], ", 0 MISMATCH")
    # The Wi-Fi trips: a fresh simulated clock at --from, then the package.
    if "trips" not in a.skip:
        d = Path("C:/pio3/jc_clean/sim/all_tests/clock")
        sys.path.insert(0, str(ROOT / "utils"))
        import web_files, io, tarfile
        web_files.remove_tree(d)
        # A clock at --from has that version's page files (from its tag), as a
        # real one does: today's pages need today's firmware (/api/limits...).
        tag = OUT / (summary.stem + "_from_pages")
        tar = subprocess.run(["git", "archive", "v" + a.frm, "Data"], cwd=ROOT, capture_output=True, check=True).stdout
        tarfile.open(fileobj=io.BytesIO(tar)).extractall(tag)
        web_files.stage(tag / "Data", d / "fs")
        for f in ("config.json", "events.json"):
            shutil.copy2(ROOT / "Data" / f, d / "fs" / f)
        for sub in ("images", "custom"):   # the family's pictures (not in git, so not in the tag): its events show them
            if (ROOT / "Data" / sub).is_dir():
                shutil.copytree(ROOT / "Data" / sub, d / "fs" / sub, dirs_exist_ok=True)
        exe = Path("C:/pio3/jc_clean/sim/versions") / a.frm / "jcclock.exe"
        log = open(d / "run.log", "wb")
        p = subprocess.Popen([str(exe), "--fs", str(d / "fs"), "--state", str(d / "state"), "--verbose",
                              "--home-port", str(ports("trips", off)[0]), "--ap-port", str(ports("trips", off)[1]), "--control-port", str(ports("trips", off)[2])],
                             stdout=log, stderr=subprocess.STDOUT, env=dict(os.environ, JC_TITLE="Test (all tests)"))
        try:
            step("trips", [PY, "utils/ap_tests.py", "--sim", "--sim-port-offset", str(off), "--package", pkg,
                           "--pages-before", str(tag / "Data")], "0 failed")
            # Trip 4 ends with a restore and a restart: until its time sync the clock
            # answers "/" with its "initializing" page (WebUI.cpp) - wait for it.
            import json, urllib.request
            for _ in range(60):
                try:
                    if json.loads(urllib.request.urlopen("http://127.0.0.1:%d/api/status" % ports("trips", off)[0], timeout=3).read()).get("synced"):
                        break
                except Exception:
                    pass
                time.sleep(2)
            time.sleep(5)   # the calendar's first download after the sync
            step("web", [PY, "sim/compare_web.py", "--sim", "http://127.0.0.1:%d" % ports("trips", off)[0]], "0 different")
        finally:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.pid)], capture_output=True)
            log.close()
    step("drills", [PY, "sim/drills.py", "--port-offset", str(off)], "0 failed")
    # Every control of the web pages, in a browser (needs the firmware of this same
    # version built - pio run - for its update test).
    step("ui", [PY, "sim/ui_tests.py", "--port-offset", str(off)], ", 0 failed, 0 uncovered")
    bad = [l for l in lines if l.startswith("FAIL")]
    lines.append("== %d steps, %d failed ==" % (len(lines), len(bad)))
    summary.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(lines[-1])
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
