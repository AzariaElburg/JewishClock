"""
The real clock against the simulator at many moments: holidays, Shabbat,
fasts, sunset, and every day around the three rain/dew prayer switch days.

    C:/pio3/jc_clean/venv/Scripts/python sim/moments.py --clock <clock-ip> [--only NAME ...]

For each moment: set the clock's debug time (POST /api/debug/time), restart
it with a reset pulse over USB (COM5 - writes nothing to the clock), wait
for it to fetch that day's calendar, then take screenshots while the
simulator runs the same moment and compares them pixel for pixel (see
compare.py). Rain/dew prayer moments are run with the setting at 0, 1 and 2
(changed live), and the clock's slides are checked against the expected
text, not only against the simulator.

ALWAYS puts the clock back on real time at the end (also after an error or
Ctrl+C), restores its rain/dew prayer setting, and restarts it.
Results: C:/pio3/jc_clean/sim/work/moments/<name>/..., summary in C:/pio3/jc_clean/sim/work/moments/summary.txt.
Needs: sim/build.py and sim/prepare_fs.py run first; pyserial (in C:/pio3/jc_clean/venv).
"""
import argparse
import json
import shutil
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
from datetime import date, timedelta
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "utils"))
from local_dirs import SIM_WORK, SIM_EXE  # noqa: E402 - outside the OneDrive folder
ROOT = Path(__file__).resolve().parent.parent
WORK = SIM_WORK
EXE = SIM_EXE
OUT = WORK / "moments"

from test_prayer import mention, pesach, request, PRAYER  # noqa: E402

MOMENTS = [   # name, local time (Israel)
    ("weekday", "2026-10-20 12:00"),
    ("friday-before-candles", "2026-10-23 14:00"),
    ("shabbat", "2026-10-24 12:00"),
    ("after-havdalah", "2026-10-24 19:30"),
    ("before-sunset", "2026-10-21 16:45"),
    ("after-sunset", "2026-10-21 18:30"),
    ("shabbat-mevarchim", "2026-11-07 12:00"),
    ("rosh-chodesh", "2026-11-10 12:00"),
    ("chanukah-evening", "2026-12-06 18:00"),
    ("chanukah-eve-of-first", "2026-12-03 21:00"),     # no Chanukah yet (first lighting is tomorrow evening)
    ("chanukah-first-morning", "2026-12-04 08:00"),    # tonight's count: 1 candle
    ("chanukah-first-night", "2026-12-04 21:00"),      # still 1 candle until midnight
    ("chanukah-rosh-chodesh", "2026-12-10 12:00"),     # Rosh Chodesh Tevet + 7 candles together
    ("chanukah-day-after-last", "2026-12-12 12:00"),   # Hebcal's "8th Day": nothing shown
    ("night-before-fast", "2026-12-19 21:00"),
    ("fast-10-tevet", "2026-12-20 12:00"),
    ("taanit-esther", "2027-03-22 12:00"),             # two holidays: Ta'anit Esther + Erev Purim
    ("purim", "2027-03-23 12:00"),
    ("erev-pesach", "2027-04-21 15:00"),
    ("omer-night", "2027-05-05 21:00"),
    ("yom-haatzmaut", "2027-05-12 12:00"),
    ("erev-tisha-bav", "2027-08-11 18:00"),
    ("tisha-bav", "2027-08-12 12:00"),
    ("erev-yom-kippur", "2027-10-10 15:00"),
    ("yom-kippur", "2027-10-11 12:00"),
    # Beyond 32-bit time (firmware 2.0+): a 32-bit clock wraps at 2038-01-19 03:14:08 UTC.
    ("y2038-before", "2038-01-19 05:00"),
    ("y2038-after", "2038-01-20 12:00"),
    ("y2040", "2040-04-01 12:00"),
    ("y2100", "2100-09-10 21:00"),
    ("y2240-hebrew-6000", "2240-09-03 12:00"),
    ("y2300", "2300-03-04 12:00"),
]
# Rain/dew prayer: every day around each switch day, and the evening before
# it after צאת; expected text per setting from test_prayer.py (the rules).
PRAYER_EXPECT = {}   # moment name -> f(setting) -> expected lines
for _kind, _day, _rule in (("atzeret", date(2026, 10, 3), mention),    # 22 Tishrei 5787
                           ("pesach", date(2027, 4, 22), pesach),       # 15 Nisan 5787
                           ("request", date(2026, 10, 17), request)):   # its evening opens 7 Cheshvan 5787
    for _d in range(-3, 4):
        MOMENTS.append(("prayer-%s%+d" % (_kind, _d), "%s 12:00" % (_day + timedelta(days=_d))))
        PRAYER_EXPECT["prayer-%s%+d" % (_kind, _d)] = (lambda r, d: lambda w: r(d, w))(_rule, _d)
for _name, _when, _rule, _d in (("prayer-atzeret-eve", "2026-10-02 20:00", mention, -1),   # after צאת: no change
                                ("prayer-pesach-eve", "2027-04-21 20:30", pesach, -1),
                                ("prayer-request-eve", "2026-10-17 19:30", request, 0)):
    MOMENTS.append((_name, _when))
    PRAYER_EXPECT[_name] = (lambda r, d: lambda w: r(d, w))(_rule, _d)


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


class Clock:
    def __init__(self, hosts):
        self.hosts = hosts

    def req(self, path, data=None, timeout=10):
        last = None
        for h in list(self.hosts):
            try:
                r = urllib.request.Request("http://%s%s" % (h, path), data=data)
                with urllib.request.urlopen(r, timeout=timeout) as resp:
                    body, headers = resp.read(), resp.headers
                if self.hosts[0] != h:
                    self.hosts.remove(h); self.hosts.insert(0, h)
                return body, headers
            except Exception as e:
                last = e
        raise RuntimeError("clock not reachable: %s" % last)

    def status(self):
        return json.loads(self.req("/api/status", timeout=6)[0])

    def post_json(self, path, obj):
        return self.req(path, data=json.dumps(obj).encode("utf-8"))[0]


def reset_over_usb(port):
    s = serial.Serial()
    s.port, s.baudrate = port, 115200
    s.dtr = False; s.rts = False      # IO0 high = normal boot
    s.open()
    s.rts = True; time.sleep(0.15); s.rts = False
    s.close()


def wait_ready(clock, min_uptime_s=60, limit_s=300):
    t0 = time.time()
    while time.time() - t0 < limit_s:
        try:
            st = clock.status()
            up = st.get("uptimeMs", 0) / 1000
            if st.get("synced") and up >= min_uptime_s:
                return st
            time.sleep(max(2, min(10, min_uptime_s - up)))
        except RuntimeError:
            time.sleep(3)
    raise RuntimeError("clock did not come back within %d s" % limit_s)


def sim_epoch(fs, when):
    out = subprocess.run([str(EXE), "--fs", str(fs), "--time", when, "--print-epoch"], capture_output=True, text=True, check=True)
    return int(out.stdout.strip())


def capture(clock, outdir, seconds, per_text=3):
    """Screenshots (for pixels) plus fast text polling (to catch every slide)."""
    outdir.mkdir(parents=True, exist_ok=True)
    texts, stop = set(), threading.Event()

    def poll_text():
        while not stop.is_set():
            try:
                texts.add(json.loads(clock.req("/api/preview", timeout=5)[0]).get("displayString", ""))
            except Exception:
                pass
            time.sleep(0.3)

    th = threading.Thread(target=poll_text, daemon=True); th.start()
    seen, counts, rows = set(), {}, []
    end = time.time() + seconds
    while time.time() < end:
        try:
            body, h = clock.req("/api/debug/screenshot", timeout=15)
        except RuntimeError:
            time.sleep(1); continue
        text = urllib.parse.unquote(h.get("X-Screen-Text", ""))
        scroll = int(h.get("X-Scroll-X", "0"))
        if (text, scroll) in seen or counts.get(text, 0) >= per_text or len(body) < 1000:
            continue
        seen.add((text, scroll)); counts[text] = counts.get(text, 0) + 1
        name = "%03d.bmp" % len(rows)
        (outdir / name).write_bytes(body)
        rows.append("%s\t%d\t%s" % (name, scroll, text))
    stop.set(); th.join()
    (outdir / "index.txt").write_text("\n".join(rows) + "\n", encoding="utf-8")
    # /api/preview joins elements with " * " - same text otherwise.
    return {t.replace(" * ", " | ") for t in texts if t}, len(rows)


def run_sim_match(fs, outdir, start_epoch, seconds):
    cmd = [str(EXE), "--fs", str(fs), "--out", str(outdir / "sim_slides"), "--http-cache", str(WORK / "http_cache"),
           "--epoch", str(start_epoch), "--seconds", str(seconds), "--match", str(outdir)]
    subprocess.run(cmd, check=True, capture_output=True)
    rows = [l.split("\t", 5) for l in (outdir / "match.txt").read_text(encoding="utf-8").splitlines()]
    sim_texts = {l.split("\t", 3)[3] for l in (outdir / "sim_slides" / "slides.txt").read_text(encoding="utf-8").splitlines()}
    return rows, sim_texts


def make_fs(base, dest, window):
    shutil.rmtree(dest, ignore_errors=True)
    shutil.copytree(base, dest, dirs_exist_ok=True)
    (dest / "jewish_calendar_data.json").unlink(missing_ok=True)
    cfg = json.loads((dest / "config.json").read_text(encoding="utf-8"))
    cfg["prayerSwitchDays"] = window
    cfg["debugTimeOffset"] = 0
    (dest / "config.json").write_text(json.dumps(cfg, ensure_ascii=False), encoding="utf-8")


def restore_real_time(clock, port, window):
    log("restoring real time on the clock ...")
    clock.post_json("/api/config", {"debugTimeOffset": 0, "prayerSwitchDays": window})
    time.sleep(8)   # the settings save lands 2 s after the last change (WebUI.cpp CONFIG_SAVE_QUIET_MS)
    off = json.loads(clock.req("/api/config")[0]).get("debugTimeOffset", None)
    log("debug offset now %s, rain/dew prayer setting %d - restarting" % (off, window))
    reset_over_usb(port)
    st = wait_ready(clock, min_uptime_s=20)
    log("clock back on real time: utc %s (PC %d)" % (st.get("utcTime"), int(time.time())))
    return off


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--clock", action="append", default=[])
    ap.add_argument("--port", default="COM5")
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--capture", type=float, default=50, help="seconds of screenshots per moment/setting")
    args = ap.parse_args()
    base = WORK / "fs"
    clock = Clock(args.clock + ["jewishclock.local"])
    original_window = json.loads(clock.req("/api/config")[0]).get("prayerSwitchDays", 2)
    OUT.mkdir(parents=True, exist_ok=True)
    summary = []

    def note(line):
        summary.append(line); log(line)
        (OUT / "summary.txt").write_text("\n".join(summary) + "\n", encoding="utf-8")

    try:
        for name, when in MOMENTS:
            if args.only and not any(o in name for o in args.only):
                continue
            prayer = name.startswith("prayer-")
            windows = (0, 1, 2) if prayer else (original_window,)
            fs0 = OUT / name / "fs"
            make_fs(base, fs0, windows[0])
            epoch = sim_epoch(fs0, when)
            log("== %s (%s) ==" % (name, when))
            wait_ready(clock, min_uptime_s=0)   # the clock refuses a debug time before its first time sync
            clock.post_json("/api/config", {"prayerSwitchDays": windows[0]})
            clock.post_json("/api/debug/time", {"timestamp": epoch})
            time.sleep(8)   # settings save lands before the restart
            reset_over_usb(args.port)
            try:
                wait_ready(clock)
            except RuntimeError as e:
                note("%s\tERROR\t%s" % (name, e)); continue
            for w in windows:
                outdir = OUT / name / ("setting%d" % w) if prayer else OUT / name
                fs = OUT / name / ("fs_w%d" % w)
                make_fs(base, fs, w)
                if w != windows[0]:
                    clock.post_json("/api/config", {"prayerSwitchDays": w})
                    time.sleep(2)
                st = clock.status()
                texts, n = capture(clock, outdir, args.capture)
                # The simulator powers on at the clock's own power-on moment and goes
                # through the same startup (Wi-Fi, time sync, downloads) - see sim_main.cpp.
                up_s = st["uptimeMs"] / 1000.0
                rows, sim_texts = run_sim_match(fs, outdir, int(st["utcTime"] - up_s), up_s + args.capture + 30)
                same = sum(r[1] == "SAME" for r in rows)
                bad = ["%s %s %s" % (r[0], r[1], r[5]) for r in rows if r[1] != "SAME"]
                only_clock = sorted(texts - sim_texts)
                verdict = "OK" if not bad and not only_clock else "MISMATCH"
                extra = ""
                if prayer:
                    want = PRAYER_EXPECT[name](w)
                    got = sorted({p.strip() for t in texts for p in t.split(" | ") if PRAYER.search(p)})
                    p_ok = got == sorted(want)
                    extra = "\tprayer %s: expected %r, clock showed %r" % ("OK" if p_ok else "WRONG", want, got)
                    if not p_ok:
                        verdict = "MISMATCH"
                note("%s%s\t%s\t%d/%d screenshots identical\tslides on clock: %d%s%s%s" % (
                    name, " setting %d" % w if prayer else "", verdict, same, n, len(texts),
                    ("\tonly on clock: %s" % only_clock) if only_clock else "",
                    ("\tdiffer: %s" % bad) if bad else "", extra))
    finally:
        try:
            off = restore_real_time(clock, args.port, original_window)
            note("RESTORED\tdebug offset %s, rain/dew prayer setting %d" % (off, original_window))
        except Exception as e:
            note("RESTORE FAILED: %s - set the time back by hand (Settings > Advanced)" % e)


if __name__ == "__main__":
    main()
