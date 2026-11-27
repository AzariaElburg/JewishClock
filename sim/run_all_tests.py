"""
Every test without the clock, in one go - before every release (CLAUDE.md).

    C:/pio3/jc_clean/venv/Scripts/python sim/run_all_tests.py --from 2.1.2 [--package FILE] [--port-offset 100] [--skip NAME ...]
        [--only GROUP|STEP[:PART] ...] [--one-by-one]

First the builds (firmware, package, jcsim, jcclock for the current code), then
four groups side by side, each with its own clocks and ports (names: the user,
2026-11-11):
  display    - what the screen shows, on the screen simulator's simulated time:
               prayer (rain/dew), brightness (light learning), show-windows and
               show-combos (when the Shabbat slides show), retry (downloads down),
               dates (against the real clock's screenshots - replay_moments.py,
               no change allowed but flames)
  web-ui     - every control of the web pages in a browser, followed to the
               clock's screen, and the family's path: a clock at --from with their
               data, updated over its own Wi-Fi to this run's package, their old
               backup restored (web_ui.py, section from_family)
  resilience - what the clock does when things go wrong: Wi-Fi, power, services,
               damaged or full flash, bad updates (resilience.py, several at a time)
  simulator-validity - the simulator's web answers against the
               real clock's recorded ones (compare_web.py)
--only picks a group, a step or a part: display, prayer:"abroad request",
web-ui:chains.24_hour, resilience:light, show-combos:shabbat3 ...
Needs: the --from version's build in C:/pio3/jc_clean/sim/versions/ (build_clock.py
--tree).
Summary: C:/pio3/jc_clean/sim/all_tests/<time>.txt
"""
import argparse
import os
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "sim"))
from sim_ports import ports  # noqa: E402
PY = "C:/pio3/jc_clean/venv/Scripts/python"
OUT = Path("C:/pio3/jc_clean/sim/all_tests")
PIO = [str(Path.home() / ".pioarduino/venv/Scripts/pio.exe"), "run", "-c", "pioarduino.ini"]


DRILL_JOBS = 6   # resilience tests side by side (resilience.py --jobs) - measured 2026-11-11, see CLAUDE.md
UI_JOBS = 4      # web-ui lanes (web_ui.py --jobs), each its own clock and browser (3 until 2026-11-15)
# The screen simulator's tests run as fast as the PC allows; below normal priority (inherited by
# their jcsim processes) so they never slow the real-time clocks beside them (resilience, web-ui, simulator-validity).
LOW = getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0)
# What --only can pick inside a step: how to pass it on.
PART_ARGS = {
    "prayer": lambda parts: ["--only"] + parts,
    "show-windows": lambda parts: ["--only"] + parts,
    "show-combos": lambda parts: ["--only"] + parts,
    "retry": lambda parts: ["--only"] + parts,
    "dates": lambda parts: ["--only"] + parts,
    "resilience": lambda parts: ["--only"] + parts,
    "web-ui": lambda parts: ["--only", ",".join(parts)],
}
GROUPS = {"build": ["build-firmware", "package", "build-jcsim", "build-clock"],
          "display": ["prayer", "brightness", "own-calculations-vs-hebcal", "show-windows", "show-combos", "retry", "status-slide", "screen-edges", "dates", "random-moments", "midnight"],
          "web-ui": ["web-ui"], "resilience": ["resilience"], "simulator-validity": ["simulator-validity"]}
STEPS = [s for g in GROUPS.values() for s in g]
GROUP_OF = {s: g for g, ss in GROUPS.items() for s in ss}


UPTIME_STARTS = {"half": 2 ** 31 - 5 * 60 * 1000, "wrap": 2 ** 32 - 5 * 60 * 1000,
                 "1000y": 1000 * 365 * 86400 * 1000}


def counting_build_as_version():
    """Code coverage: a clock that updates to this version runs C:/pio3/jc_clean/sim/versions/<version>/
    jcclock.exe (sim_clock_main.cpp exeFor()) - the normal build, which counts nothing: every line
    run only after an update looked never run (2026-11-17, the update drills start from an older
    version). For the run, the counting build stands there; put back after. Returns the undo."""
    sys.path.insert(0, str(ROOT / "sim"))
    import build_clock
    dest = build_clock.VERSIONS / build_clock.version() / "jcclock.exe"
    if "-" not in dest.parent.name:
        print("coverage: %s is a release's build - left as it is; lines after an update to it count nothing" % dest)
        return None
    keep = dest.with_name("jcclock.exe.normal")
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists() and not keep.exists():
        dest.replace(keep)
    shutil.copy2(build_clock.OUT / "jcclock.exe", dest)

    def undo():
        if keep.exists():
            keep.replace(dest)
        else:
            dest.unlink(missing_ok=True)
    return undo


KEEP_RUNS = 10   # each group's newest run folders kept; the older ones removed at the start of a run


def prune_old_runs():
    """The groups' old run folders (2026-11-21: 600 of them, 19 GB, filled the PC's disk mid-run). Kept: the
    newest KEEP_RUNS of each - their durations.json orders the next run's drills."""
    for d in (Path("C:/pio3/jc_clean/sim/resilience"), Path("C:/pio3/jc_clean/sim/web_ui")):
        if not d.is_dir():
            continue
        for old in sorted((x for x in d.iterdir() if x.is_dir()), reverse=True)[KEEP_RUNS:]:
            shutil.rmtree(old, ignore_errors=True)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from time_relay import ensure_relay
    print("the test clocks' time server: 127.0.0.1:%d (sim/time_relay.py)" % ensure_relay(), flush=True)
    ap = argparse.ArgumentParser()
    ap.add_argument("--from", dest="frm", required=True, help="the family's version (web-ui's from_family starts there), e.g. 2.1.2")
    ap.add_argument("--package", help="the package from_family installs (default: one made from this run's build)")
    ap.add_argument("--port-offset", type=int, default=0)
    ap.add_argument("--skip", nargs="*", default=[])
    ap.add_argument("--coverage", metavar="DIR", help="code coverage (2026-11-16): the simulators built with --coverage first "
                    "(sim/build.py, build_clock.py), the builds skipped; each test's counts in DIR/<test>/ - sim/coverage_report.py")
    ap.add_argument("--only", nargs="*", help="GROUP, STEP or STEP:PART (e.g. display resilience:light web-ui:chains.24_hour); "
                                              "the builds always run (--skip them if they're done)")
    ap.add_argument("--stress", action="store_true", help="6 web-ui lanes and 10 drills side by side - a loaded machine")
    ap.add_argument("--one-by-one", action="store_true", help="the groups one after another (as before 2026-11-11)")
    ap.add_argument("--uptime-start", choices=sorted(UPTIME_STARTS),
                    help="every simulated clock starts as if already up that long (2026-11-14): half = 5 min before "
                         "24.8 days (2^31 ms), wrap = 5 min before 49.7 days (2^32 ms), 1000y = a thousand years. "
                         "The clock must run for years without a restart; this finds what a fresh clock never shows")
    ap.add_argument("--millis-wrap", action="store_const", const="wrap", dest="uptime_start", help="= --uptime-start wrap")
    ap.add_argument("--seed", type=int, help="the run's random choices (2026-11-20: which web-ui sections share a lane and "
                                             "their order, the drills' start order) - given: that run again; else a new one, printed")
    a = ap.parse_args()
    prune_old_runs()
    seed = a.seed if a.seed is not None else int(time.time() * 1000) % 1000000
    os.environ["JC_TEST_SEED"] = str(seed)   # inherited by every test process (sim/clock_facts.py test_random())
    print("seed %d (again: --seed %d)" % (seed, seed), flush=True)
    # How many side by side: around the measured best, by the seed - the load changes, and with it
    # which timings meet (2026-11-20, the night cycles). Same seed, same choice.
    global UI_JOBS, DRILL_JOBS
    import random
    rnd = random.Random("%d/jobs" % seed)
    UI_JOBS, DRILL_JOBS = rnd.choice((3, 4, 5)), rnd.choice((5, 6, 7))
    if a.stress:   # well past the measured best: timings that only hold on a quiet machine show (2026-11-21)
        UI_JOBS, DRILL_JOBS = 6, 10
    print("side by side: %d web-ui lanes, %d drills" % (UI_JOBS, DRILL_JOBS), flush=True)
    if a.uptime_start:
        os.environ["JC_UPTIME_START_MS"] = str(UPTIME_STARTS[a.uptime_start])   # sim_platform.cpp; inherited by every clock started
    parts = {}
    for item in a.only or []:
        step_name, _, part = item.partition(":")
        if step_name in GROUPS and step_name not in STEPS:   # a whole group
            if part:
                sys.exit("%s is a group - name a step in it: %s" % (step_name, " ".join(GROUPS[step_name])))
            for st in GROUPS[step_name]:
                parts.setdefault(st, [])
            continue
        if step_name not in STEPS:
            sys.exit("no group or step %s - groups: %s; steps: %s" % (step_name, " ".join(GROUPS), " ".join(STEPS)))
        if part and step_name not in PART_ARGS:
            sys.exit("%s has no parts" % step_name)
        lst = parts.setdefault(step_name, [])
        if part:
            lst.append(part)
    OUT.mkdir(parents=True, exist_ok=True)
    summary = OUT / (time.strftime("%Y%m%d_%H%M") + ".txt")
    off = a.port_offset
    lines, lock = {}, threading.Lock()

    def wanted(name):
        return name not in a.skip and (not a.only or name in parts or GROUP_OF[name] == "build")

    def step(name, cmd, ok_text=None, timeout=3 * 3600, low=False):
        if not wanted(name):
            return True
        cmd = cmd + (PART_ARGS[name](parts[name]) if parts.get(name) else [])
        t0 = time.time()
        # code coverage: this step's counts in its own folder (resilience and web-ui split theirs
        # further) - its own environment: the steps run side by side
        env = dict(os.environ, JC_COV_DIR=a.coverage, LLVM_PROFILE_FILE=a.coverage + "/" + name + "/%p.profraw") if a.coverage else None
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout,
                           creationflags=LOW if low else 0, env=env)
        out = (r.stdout or "") + (r.stderr or "")
        (OUT / (summary.stem + "_" + name + ".log")).write_text(out, encoding="utf-8")
        last = [l for l in out.splitlines() if l.strip()][-1:] or [""]
        ok = r.returncode == 0 and (ok_text is None or ok_text in out)
        line = "%s %-18s %-18s %5.0f s  %s" % ("PASS" if ok else "FAIL", GROUP_OF[name], name, time.time() - t0, last[0][:120])
        with lock:
            lines[name] = line
            print(line, flush=True)
            summary.write_text("\n".join(lines[n] for n in STEPS if n in lines) + "\n", encoding="utf-8")
        return ok

    # This run's own firmware and update package (2026-11-08: the newest package in
    # dist was used - an older release - and the update test's clock got the last USB
    # upload's page files: stale on both counts). The package goes into this run's
    # folder, never next to the ones that get emailed; --package tests a given one instead.
    t_all = time.time()
    os.environ["PLATFORMIO_CORE_DIR"] = "C:/pio3"
    step("build-firmware", PIO, "[SUCCESS]")
    pkg_dir = OUT / (summary.stem + "_package")
    if a.package:
        pkg = a.package
    else:
        step("package", [PY, "utils/make_update_package.py", "--out", str(pkg_dir)], "->")
        found = sorted(pkg_dir.glob("*.jcupdate"))
        if not found and not wanted("package"):
            # Builds skipped: the last run's package (it goes with the build being reused). Never
            # nothing - the tests then fell back to dist's newest, an old unsigned release
            # (2026-11-16: update_bad_files failed 7 runs in a row on it).
            found = sorted(OUT.glob("*_package/*.jcupdate"), key=lambda p: p.stat().st_mtime)
        pkg = str(found[-1]) if found else ""
        if not pkg:
            sys.exit("no update package for this run - build one (don't skip 'package') or give --package")
    step("build-jcsim", [PY, "sim/build.py"], "OK:")
    step("build-clock", [PY, "sim/build_clock.py"], "OK:")

    def display_lane():   # the screen simulator (simulated time)
        # Three streams side by side (2026-11-15: ~9.5 min one after another), balanced by the
        # 2026-11-15 run's times; each test has its own work folder.
        streams = [
            [("show-combos", [PY, "sim/test_show_combos.py"], "ALL CORRECT"),                      # 160 s
             ("brightness", [PY, "sim/test_brightness.py"], "ALL CORRECT"),                        # 5 s
             ("own-calculations-vs-hebcal", [PY, "sim/test_own_calculations_vs_hebcal.py"], "ALL CORRECT"),                    # 7 s
             ("retry", [PY, "sim/test_retry.py"], "== 0 failed =="),                              # 32 s
             ("midnight", [PY, "sim/test_midnight.py"], "midnight test: ALL CORRECT")],             # 2026-11-21
            [("dates", [PY, "sim/replay_moments.py", "--expect-changed", "#"], ", 0 MISMATCH"),   # 150 s
             ("status-slide", [PY, "sim/test_status_slide.py"], "ALL CORRECT"),                   # 69 s
             ("screen-edges", [PY, "sim/test_screen_edges.py"], "screen edges test: ALL CORRECT")],   # 40 s
            [("show-windows", [PY, "sim/test_show_windows.py"], "ALL CORRECT"),                    # 83 s
             ("prayer", [PY, "sim/test_prayer.py"], "ALL CORRECT"),                                # 72 s
             ("random-moments", [PY, "sim/test_random_moments.py"], "random moments test: ALL CORRECT")],   # ~15 s, 3 random moments
        ]
        ths = [threading.Thread(target=lambda s=s: [step(n, c, ok, low=True) for n, c, ok in s]) for s in streams]
        for th in ths:
            th.start()
        for th in ths:
            th.join()

    def web_ui_lane():   # every control of the web pages, in a browser (its update tests need this version's firmware - built above)
        step("web-ui", [PY, "sim/web_ui.py", "--port-offset", str(off), "--jobs", str(UI_JOBS), "--from", a.frm]
             + (["--package", pkg] if pkg else [])
             + (["--js-coverage", a.coverage + "/js"] if a.coverage else []),   # the page's own code too
             ", 0 failed" + (", 0 uncovered" if not parts.get("web-ui") else ""))

    def resilience_lane():
        step("resilience", [PY, "sim/resilience.py", "--port-offset", str(off), "--jobs", str(DRILL_JOBS)]
             + (["--package", pkg] if pkg else []), "0 failed")

    def simulator_validity_lane():   # a clock of this build with Data/, its web answers against the real clock's recorded ones
        if not wanted("simulator-validity"):
            return
        import json
        import urllib.request
        sys.path.insert(0, str(ROOT / "utils"))
        import web_files
        d = OUT / "validity_clock"
        web_files.remove_tree(d)
        web_files.stage(ROOT / "Data", d / "fs")
        for sub in ("images", "custom"):   # the family's pictures (not in git)
            if (ROOT / "Data" / sub).is_dir():
                shutil.copytree(ROOT / "Data" / sub, d / "fs" / sub, dirs_exist_ok=True)
        (d / "state").mkdir(parents=True, exist_ok=True)
        shutil.copy2("C:/pio3/jc_clean/sim/build/jcclock.exe", d / "jcclock.exe")   # its own copy: a running clock locks it
        home, ap_port, ctl_port = ports("simulator_validity", off)
        log = open(d / "run.log", "wb")
        p = subprocess.Popen([str(d / "jcclock.exe"), "--fs", str(d / "fs"), "--state", str(d / "state"), "--verbose",
                              "--home-port", str(home), "--ap-port", str(ap_port), "--control-port", str(ctl_port)],
                             stdout=log, stderr=subprocess.STDOUT, cwd=str(d), env=dict(os.environ, JC_TITLE="Test (simulator-validity)",
                                     **({"LLVM_PROFILE_FILE": a.coverage + "/simulator-validity/%p.profraw"} if a.coverage else {})))
        try:
            for _ in range(90):   # until its time sync "/" is the "initializing" page (WebUI.cpp)
                try:
                    if json.loads(urllib.request.urlopen("http://127.0.0.1:%d/api/status" % home, timeout=3).read()).get("synced"):
                        break
                except Exception:
                    pass
                time.sleep(2)
            time.sleep(5)   # the calendar's first download after the sync
            step("simulator-validity", [PY, "sim/compare_web.py", "--sim", "http://127.0.0.1:%d" % home,
                                        "--staged-fs", str(d / "fs")], "0 different")
        finally:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.pid)], capture_output=True)
            log.close()

    # Each group has its own clocks on its own ports (sim_ports.py): side by side (2026-11-11; ~1 h 45 min one by one).
    lanes = [web_ui_lane, resilience_lane, simulator_validity_lane, display_lane]
    swapped = counting_build_as_version() if a.coverage else None
    try:
        if a.one_by_one:
            for lane in lanes:
                lane()
        else:
            threads = [threading.Thread(target=lane, name=lane.__name__) for lane in lanes]
            for th in threads:
                th.start()
            for th in threads:
                th.join()
    finally:
        if swapped:
            swapped()
    out = [lines[n] for n in STEPS if n in lines]
    bad = [l for l in out if l.startswith("FAIL")]
    out.append("== %d steps, %d failed, %.0f min, seed %d (%d lanes, %d drills) ==" % (len(out), len(bad), (time.time() - t_all) / 60, seed, UI_JOBS, DRILL_JOBS))
    summary.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(out[-1])
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
