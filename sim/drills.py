"""
Drills on the full simulator (jcclock.exe): what the clock does when things
go wrong or change - for every new version, a fix, or a complaint to
reproduce, now that the clock itself is with the parents.

    C:/pio3/jc_clean/venv/Scripts/python sim/drills.py [--only NAME ...] [--list]

Each drill starts a fresh simulated clock (the staged files, C:/pio3/jc_clean/fs,
plus the settings and events in Data/), acts on it through the page's own API
and the control port (Wi-Fi world, power, light), and checks the outcome in
its answers and its own log. Results: C:/pio3/jc_clean/sim/drills/<time>/ (one
folder and run.log per drill), summary.txt.
"""
import argparse
import gzip
import json
import os
import shutil
import socket
import stat
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = Path("C:/pio3/jc_clean/sim/build/jcclock.exe")
STAGED = Path("C:/pio3/jc_clean/fs")
OUT = Path("C:/pio3/jc_clean/sim/drills") / time.strftime("%Y%m%d_%H%M%S")
HOME, AP, CTL = "http://127.0.0.1:8080", "http://127.0.0.1:8081", "http://127.0.0.1:8090"
results = []


# ── plumbing ──────────────────────────────────────────────────────────────────
def log(msg):
    line = time.strftime("%H:%M:%S ") + msg
    print(line, flush=True)
    with open(OUT / "summary.txt", "a", encoding="utf-8") as f:
        f.write(line + "\n")


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    log("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


def http(base, path, data=None, timeout=20, raw=False, form=None, headers=None):
    if form is not None:
        data = urllib.parse.urlencode(form).encode()
    req = urllib.request.Request(base + path, data=data, headers=headers or {})
    if form is not None:
        req.add_header("Content-Type", "application/x-www-form-urlencoded")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            body, code = r.read(), r.status
            if r.headers.get("Content-Encoding") == "gzip":
                body = gzip.decompress(body)
    except urllib.error.HTTPError as e:
        body, code = e.read(), e.code
    return (code, body) if raw else body


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k):
        return None


def in_setup_mode():
    """The setup page's own answer: /wifi-scan's JSON, NOT followed through a redirect -
    since 2.1.1 the normal page answers 200 on the clock's own Wi-Fi too."""
    try:
        r = urllib.request.build_opener(_NoRedirect).open(AP + "/wifi-scan", timeout=4)
        b = r.read()
        return r.status == 200 and b.lstrip().startswith(b"{") and b"networks" in b
    except Exception:
        return False


def jget(base, path):
    return json.loads(http(base, path))


def api_post(path, obj, base=HOME, timeout=60):
    """As the page's apiPost(): form field body=<JSON>."""
    return http(base, path, form={"body": json.dumps(obj, ensure_ascii=False)}, raw=True, timeout=timeout)


def ctl(path, body=None):
    try:
        return http(CTL, path, data=body if body is not None else (b"" if path != "/state" and path != "/world" else None), timeout=10)
    except Exception:
        return b""   # power-cut / reset / stop: the child is gone before it answers


def state():
    return json.loads(ctl("/state") or b"{}")


def wait(cond, timeout, step=1.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            v = cond()
            if v:
                return v
        except Exception:
            pass
        time.sleep(step)
    return None


def up(base=HOME):
    try:
        return http(base, "/api/status", timeout=3) and True
    except Exception:
        return False


def synced():
    try:
        return jget(HOME, "/api/status").get("synced")
    except Exception:
        return False


def ntp_now():
    """UTC from a real NTP server (independent of the PC's clock)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(5)
    s.sendto(b"\x23" + 47 * b"\0", ("pool.ntp.org", 123))
    d = s.recv(48)
    t0 = time.time()
    sec, frac = struct.unpack("!II", d[40:48])
    return sec - 2208988800 + frac / 2 ** 32, t0


def rmtree(p):
    def clear(func, path, _):
        os.chmod(path, stat.S_IWRITE)
        func(path)
    if Path(p).exists():
        shutil.rmtree(p, onerror=clear)


class Clock:
    """One fresh simulated clock for one drill."""
    def __init__(self, name, exe=EXE, world=None):
        self.dir = OUT / name
        self.fs, self.state = self.dir / "fs", self.dir / "state"
        rmtree(self.dir)
        shutil.copytree(STAGED, self.fs)
        for f in ("config.json", "events.json"):   # the clock's live settings and events (synced)
            shutil.copy2(ROOT / "Data" / f, self.fs / f)
        for d in ("images", "custom"):
            if (ROOT / "Data" / d).exists():
                shutil.copytree(ROOT / "Data" / d, self.fs / d, dirs_exist_ok=True)
        self.state.mkdir(parents=True)
        if world is not None:
            (self.state / "world.json").write_text(json.dumps(world), encoding="utf-8")
        self.logf = open(self.dir / "run.log", "wb")
        self.p = subprocess.Popen([str(exe), "--fs", str(self.fs), "--state", str(self.state), "--verbose"],
                                  stdout=self.logf, stderr=subprocess.STDOUT, cwd=str(self.dir))

    def log_text(self):
        self.logf.flush()
        return (self.dir / "run.log").read_text(encoding="utf-8", errors="replace")

    def diag(self):
        return http(HOME if up() else AP, "/api/debug/diagnostic-log").decode("utf-8", "replace")

    def stop(self):
        ctl("/stop")
        try:
            self.p.wait(15)
        except subprocess.TimeoutExpired:
            self.p.kill()
        self.logf.close()


def default_world():
    """The world the simulator makes from the known networks (sim_wifi.cpp), read back."""
    return json.loads(ctl("/world"))


# ── the drills ────────────────────────────────────────────────────────────────
def drill_boot_ntp():
    c = Clock("boot_ntp")
    t0 = time.time()
    try:
        ok = wait(synced, 90)
        check("boot: time synced from NTP", ok, "after %.0f s" % (time.time() - t0))
        st = jget(HOME, "/api/status")
        ref, at = ntp_now()
        clock_utc = st["utcTime"] + (time.time() - at)
        check("boot: clock's time = a separate NTP query", abs(clock_utc - ref) < 2.5, "difference %.2f s" % (clock_utc - ref))
        check("boot: no debug offset", jget(HOME, "/api/config").get("debugTimeOffset") == 0)
        txt = c.log_text()
        check("boot: NTP first sync is a step from 1970", "First sync" in txt and "(step)" in txt)
    finally:
        c.stop()


def drill_rtc_memory():
    """Log lines waiting in RTC memory: kept by a reset, lost by a power cut (CLAUDE.md)."""
    c = Clock("rtc_memory")
    try:
        wait(synced, 90)
        # Scroll mode: the screen never stands still, so log lines wait ~2 s
        # for their write (DisplayManager::goodMomentToWrite) - in alternating
        # mode they reach the flash within milliseconds, before any cut.
        api_post("/api/config", {"rotationMode": 1})
        time.sleep(4)
        kept = lost = 0
        for i in range(3):
            tag = "drill-reset-%d-%d" % (i, int(time.time()))
            http(HOME, "/api/debug/diagnostic-log/note", form={"msg": tag})
            ctl("/reset")
            wait(up, 60)
            kept += tag in c.diag()
        for i in range(3):
            tag = "drill-cut-%d-%d" % (i, int(time.time()))
            http(HOME, "/api/debug/diagnostic-log/note", form={"msg": tag})
            ctl("/power-cut")
            wait(up, 60)
            lost += tag not in c.diag()
        check("RTC memory: waiting log lines survive a reset", kept == 3, "%d of 3" % kept)
        check("RTC memory: waiting log lines are lost by a power cut", lost >= 1, "%d of 3 lost (0 = they were written before the cut)" % lost)
    finally:
        c.stop()


def drill_wifi_failover():
    """The network it's on disappears: it joins the other known one."""
    c = Clock("wifi_failover")
    try:
        wait(synced, 90)
        w = default_world()
        first = state().get("ssid")
        for n in w["networks"]:
            if n["ssid"] == first:
                n["up"] = False
        ctl("/world", json.dumps(w).encode())
        other = wait(lambda: (lambda s: s.get("sta") and s.get("ssid") != first and s["ssid"])(state()), 120)
        check("Wi-Fi: its network gone -> joins the other known one", other, "%s -> %s" % (first, other))
        check("Wi-Fi: the page answers again", wait(up, 30))
        txt = c.log_text()
        check("Wi-Fi: the drop is logged (reason 200)", "Disconnected (Reason: 200)" in txt)
    finally:
        c.stop()


def drill_wifi_all_down():
    """No known network around: setup mode; back when one returns (PORTAL_RETRY_MS, 2 min)."""
    c = Clock("wifi_all_down")
    try:
        wait(synced, 90)
        w = default_world()
        for n in w["networks"]:
            n["up"] = False
        ctl("/world", json.dumps(w).encode())
        # Once online, the clock's own Wi-Fi stays its synchronized one ("JewishClock"):
        # the full page must work there, and the screen must name that network
        # (both broken before 2.1.1 - a redirect loop, and "JewishClock-Setup").
        scr = wait(lambda: (lambda s: s if "Setup: join Wi-Fi" in s else None)(state().get("screen", "")), 150, 2)
        check("Wi-Fi all gone: screen names the clock's Wi-Fi that's up",
              scr and "join Wi-Fi JewishClock " in scr, (scr or state().get("screen", ""))[:90])
        code, page = http(AP, "/", raw=True)
        check("Wi-Fi all gone: the page works on the clock's own Wi-Fi", code == 200 and b"<html" in page.lower(), "status %s" % code)
        check("Wi-Fi all gone: its settings answer there", "wifiSSID" in jget(AP, "/api/config"))
        for n in w["networks"]:
            n["up"] = True
        ctl("/world", json.dumps(w).encode())
        back = wait(lambda: state().get("sta"), 200, 3)
        check("Wi-Fi back: rejoins by itself within the 2-minute retry", back)
    finally:
        c.stop()


def drill_wrong_password():
    c = Clock("wrong_password")
    try:
        wait(synced, 90)
        w = default_world()
        for n in w["networks"]:
            n["password"] = n["password"] + "x"   # the router's password changed
        ctl("/world", json.dumps(w).encode())
        ctl("/reset")
        setup = wait(in_setup_mode, 180, 2)
        check("wrong password: ends in setup mode", setup)
        check("wrong password: logged as auth failure (202)", "Reason: 202" in c.log_text())
    finally:
        c.stop()


def drill_no_internet():
    """On Wi-Fi without internet: no time, no calendar - and back once it returns."""
    c = Clock("no_internet")
    try:
        w = None
        wait(lambda: state().get("sta"), 60)
        w = default_world()
        for n in w["networks"]:
            n["internet"] = False
        ctl("/world", json.dumps(w).encode())
        ctl("/power-cut")
        wait(lambda: state().get("sta"), 60)
        time.sleep(20)
        st = jget(HOME, "/api/status")
        check("no internet: Wi-Fi up, time NOT synced", not st.get("synced"), "synced=%s" % st.get("synced"))
        code, page = http(HOME, "/", raw=True)
        check("no internet: page = 'initializing'", b"initializing" in page)
        for n in w["networks"]:
            n["internet"] = True
        ctl("/world", json.dumps(w).encode())
        check("internet back: time syncs by itself", wait(synced, 120))
    finally:
        c.stop()


def drill_settings_power_cut():
    """A change survives a power cut once saved (2 s quiet); within those 2 s it's lost - by design."""
    c = Clock("settings_power_cut")
    try:
        wait(synced, 90)
        api_post("/api/config", {"scrollSpeed": 5})
        time.sleep(4)
        ctl("/power-cut")
        wait(synced, 90)
        check("settings: saved change survives a power cut", jget(HOME, "/api/config").get("scrollSpeed") == 5)
        api_post("/api/config", {"scrollSpeed": 7})
        ctl("/power-cut")
        wait(up, 90)
        sp = jget(HOME, "/api/config").get("scrollSpeed")
        check("settings: change cut within 2 s - old value, file intact", sp == 5, "scrollSpeed=%s" % sp)
        for i in range(8):   # many cuts while saving
            api_post("/api/config", {"scrollSpeed": 2 + i % 3})
            time.sleep(2.0 + 0.1 * i)
            ctl("/power-cut")
            wait(up, 90)
        cfg = json.loads((c.fs / "config.json").read_text(encoding="utf-8"))
        check("settings: config.json intact after 8 cuts around saves", "wifiSSID" in cfg and cfg.get("wifiSSID"))
    finally:
        c.stop()


def drill_light():
    c = Clock("light")
    try:
        wait(synced, 90)
        check("light: sensor in use (as set)", jget(HOME, "/api/config").get("useLightSensor"))
        ctl("/light?lux=2")
        time.sleep(15)
        dark = jget(HOME, "/api/status")["brightnessPct"]
        ctl("/light?lux=800")
        time.sleep(15)
        bright = jget(HOME, "/api/status")["brightnessPct"]
        check("light: brighter room -> brighter screen", bright > dark, "%.1f%% at 2 lux, %.1f%% at 800 lux" % (dark, bright))
    finally:
        c.stop()


OLD_EXE = Path("C:/pio3/jc_clean/sim/versions/2.0.0-146-g08b7548/jcclock.exe")   # a version to update FROM
DIST = Path("C:/pio3/jc_clean/dist")


def newest_package():
    return max(DIST.glob("*.jcupdate"), key=lambda p: p.stat().st_mtime)


def upload(data, name="update.jcupdate", base=HOME):
    boundary = "----drill%d" % int(time.time() * 1000)
    body = (("--%s\r\nContent-Disposition: form-data; name=\"firmware\"; filename=\"%s\"\r\n"
             "Content-Type: application/octet-stream\r\n\r\n") % (boundary, name)).encode() + data + ("\r\n--%s--\r\n" % boundary).encode()
    return http(base, "/update", data=body, raw=True, timeout=120,
                headers={"Content-Type": "multipart/form-data; boundary=" + boundary})


def version():
    try:
        return jget(HOME, "/api/status").get("firmware")
    except Exception:
        return None


def drill_update_bad_files():
    c = Clock("update_bad_files")
    try:
        wait(synced, 90)
        before = version()
        pkg = newest_package().read_bytes()
        bad = bytearray(pkg); bad[len(bad) // 2] ^= 0xFF
        for label, data, want in [("damaged", bytes(bad), "damaged"), ("cut off", pkg[:len(pkg) // 2], "incomplete"),
                                  ("not an update", b"\x89PNG\r\n\x1a\n" + os.urandom(5000), "not a clock update"),
                                  ("empty", b"", "")]:
            code, msg = upload(data)
            text = msg.decode("utf-8", "replace")
            time.sleep(4)
            check("update %s: refused" % label, code != 200 and (want in text), "%s: %s" % (code, text[:110]))
            check("update %s: clock unchanged, no restart" % label, version() == before and state().get("millis", 0) > 10000)
        check("update: no leftovers waiting (state ok)", jget(HOME, "/api/status").get("updateState") == "ok")
    finally:
        c.stop()


def drill_update_trial_fail():
    """New version can't get on Wi-Fi: after its trial (5 min + 60 s) the clock goes back by itself."""
    c = Clock("update_trial_fail", exe=OLD_EXE)
    try:
        wait(synced, 90)
        old = version()
        pkg = newest_package()
        new = pkg.stem.replace("JewishClock-update-", "")
        code, msg = upload(pkg.read_bytes(), pkg.name)
        check("trial fail: package accepted", code == 200, msg.decode()[:80])
        wait(lambda: not up(), 30)
        w = default_world()   # the control port answers during the restart only once the new version runs
        for n in w["networks"]:
            n["up"] = False
        wait(lambda: ctl("/world", json.dumps(w).encode()) == b"ok", 60)
        ota = wait(lambda: (lambda o: o if o and o["slots"][o["boot"]]["state"] == "pending" else None)(state().get("ota")), 60)
        check("trial fail: new version runs on trial", ota and ota["slots"][ota["boot"]]["version"] == new, str(ota)[:120])
        back = wait(lambda: (lambda s: s.get("version") == old and s)(state()), 9 * 60, 5)
        check("trial fail: back to the old version by itself", back, "after the trial deadline")
        for n in w["networks"]:
            n["up"] = True
        ctl("/world", json.dumps(w).encode())
        check("trial fail: old version online again", wait(synced, 180) and version() == old, version())
        check("trial fail: logged", "failed its trial" in c.diag())
    finally:
        c.stop()


def drill_update_power_cut_in_trial():
    """Power cut while the new version is still on trial: the bootloader goes back (ESP-IDF rollback)."""
    c = Clock("update_power_cut_in_trial", exe=OLD_EXE)
    try:
        wait(synced, 90)
        old = version()
        pkg = newest_package()
        code, msg = upload(pkg.read_bytes(), pkg.name)
        check("power cut in trial: package accepted", code == 200, msg.decode()[:80])
        ota = wait(lambda: (lambda o: o if o and o["slots"][o["boot"]]["state"] == "pending" else None)(state().get("ota")), 90)
        check("power cut in trial: new version on trial", ota)
        ctl("/power-cut")
        time.sleep(3)
        back = wait(lambda: (lambda s: s.get("version") and s)(state()), 60)
        check("power cut in trial: bootloader went back to the old version", back and back["version"] == old, back and back.get("version"))
        check("power cut in trial: old version online", wait(synced, 120))
    finally:
        c.stop()


def bmp16(w, h, seed):
    """A 16-bit BMP as the event form makes them (72x96), different for each seed."""
    row = (w * 2 + 3) & ~3
    px = bytearray()
    for y in range(h):
        line = bytearray()
        for x in range(w):
            v = ((x * 7 + y * 3 + seed * 131) & 0xFFFF)
            line += struct.pack("<H", v)
        px += line + b"\0" * (row - len(line))
    hdr = struct.pack("<2sIHHI", b"BM", 54 + len(px), 0, 0, 54)
    dib = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 16, 0, len(px), 0, 0, 0, 0)
    return hdr + dib + bytes(px)


def upload_picture(data):
    boundary = "----pic%d" % int(time.time() * 1000)
    body = (("--%s\r\nContent-Disposition: form-data; name=\"image\"; filename=\"picture.bmp\"\r\n"
             "Content-Type: image/bmp\r\n\r\n") % boundary).encode() + data + ("\r\n--%s--\r\n" % boundary).encode()
    return http(HOME, "/api/images/upload", data=body, raw=True, timeout=60,
                headers={"Content-Type": "multipart/form-data; boundary=" + boundary})


def drill_pictures_fill():
    """Pictures until the flash is full: refused with a reason, nothing else breaks."""
    c = Clock("pictures_fill")
    try:
        wait(synced, 90)
        added, refusal = 0, None
        for i in range(400):
            code, msg = upload_picture(bmp16(72, 96, i + 1))
            if code != 200:
                refusal = msg.decode("utf-8", "replace"); break
            added += 1
        check("pictures: refused before the flash is full, with a reason", refusal is not None and added > 20,
              "%d added, then: %s" % (added, (refusal or "")[:100]))
        # Still works: a setting saves, the log writes, the clock restarts fine.
        api_post("/api/config", {"scrollSpeed": 3})
        time.sleep(4)
        ctl("/reset")
        check("pictures full: settings still saved", wait(synced, 90) and jget(HOME, "/api/config").get("scrollSpeed") == 3)
        check("pictures full: events intact", len(jget(HOME, "/api/events/list")) > 0)
        # Unused pictures are cleaned at start (main.cpp: ImageLibrary::removeUnused)
        left = len(jget(HOME, "/api/images/list"))
        check("pictures no event uses: removed at the next start", left <= 2, "%d left" % left)
    finally:
        c.stop()


def drill_events_many():
    """Many events at once (Excel import): accepted up to what the clock holds, refused cleanly beyond."""
    c = Clock("events_many")
    try:
        wait(synced, 90)
        for n in (300, 2000):
            evs = [{"name": "Drill %04d" % i, "type": "birthday", "hasG": True, "gD": 1 + i % 28, "gM": 1 + i % 12, "gY": 1950 + i % 60}
                   for i in range(n)]
            code, body = api_post("/api/events/import", evs)
            t = body.decode("utf-8", "replace")
            check("events: import %d" % n, code == 200 and '"saved":true' in t.replace(" ", ""), "%s %s" % (code, t[:120]))
            check("events: %d listed" % n, len(jget(HOME, "/api/events/list")) == n)
        ctl("/reset")
        wait(synced, 120)
        check("events: 2000 still there after a restart", len(jget(HOME, "/api/events/list")) == 2000)
        check("events: the page still loads", b"<html" in http(HOME, "/").lower())
        st = state()
        log("     PSRAM used with 2000 events: %.1f MB" % (st.get("psramUsed", 0) / 1e6))
    finally:
        c.stop()


def latest_backup_xlsx():
    files = sorted(Path("C:/pio3/jc_clean/ap_tests").glob("*/JewishClock-backup-*.xlsx"), key=lambda p: p.stat().st_mtime)
    return files[-1] if files else None


def drill_complaint_repro():
    """A complaint: put the family's backup on a factory-new clock, go to the date, see what it shows -
    and compare with the screen simulator at that moment."""
    xlsx = latest_backup_xlsx()
    if not check("complaint: a family backup (Excel) to start from", xlsx, str(xlsx)):
        return
    c = Clock("complaint_repro")
    try:
        wait(synced, 90)
        http(HOME, "/api/factory-reset", form={"body": "{}"})
        wait(in_setup_mode, 120, 2)
        from playwright.sync_api import sync_playwright
        with sync_playwright() as p:
            b = p.chromium.launch(channel="msedge", headless=True)
            pg = b.new_page()
            pg.goto(AP + "/restore", wait_until="load")
            pg.set_input_files("#restore-file", str(xlsx))
            msg = wait(lambda: (lambda t: t if t.startswith("Restored") or "not" in t else None)(pg.inner_text("#restore-msg")), 120)
            b.close()
        check("complaint: backup restored on the empty clock", msg and msg.startswith("Restored"), (msg or "")[:80])
        check("complaint: back online with the family's settings", wait(synced, 180))
        moment = "2027-04-21T20:30"   # e.g. "on the eve of Pesach it showed ..."
        api_post("/api/debug/time", {"local": moment})
        texts = set()
        end = time.time() + 75
        while time.time() < end:
            s = state().get("screen", "")
            if s:
                texts.add(s)
            time.sleep(0.4)
        log("     the full simulator showed: %s" % sorted(texts))
        # The screen simulator, same settings and events, same moment.
        fs = c.dir / "jcsim_fs"
        shutil.copytree(c.fs, fs, ignore=shutil.ignore_patterns("log"))
        out = c.dir / "jcsim_out"
        subprocess.run([str(Path("C:/pio3/jc_clean/sim/build/jcsim.exe")), "--fs", str(fs), "--time", moment.replace("T", " "),
                        "--seconds", "120", "--instant-boot", "--out", str(out)], capture_output=True)
        sim = {l.split("\t", 3)[3] for l in (out / "slides.txt").read_text(encoding="utf-8").splitlines()}
        missing = sorted(t for t in sim if t not in texts and not t.startswith("Connecting"))
        check("complaint: full simulator shows what the screen simulator shows", not missing, "not seen: %s" % missing[:3])
        api_post("/api/debug/time", {"offset": 0})
    finally:
        c.stop()


DRILLS = {f.__name__[6:]: f for f in [drill_boot_ntp, drill_rtc_memory, drill_wifi_failover, drill_wifi_all_down,
                                       drill_wrong_password, drill_no_internet, drill_settings_power_cut, drill_light,
                                       drill_update_bad_files, drill_update_power_cut_in_trial, drill_update_trial_fail,
                                       drill_pictures_fill, drill_events_many, drill_complaint_repro]}


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--exe", help="another build (e.g. C:/pio3/jc_clean/sim/versions/<version>/jcclock.exe) - before/after a fix")
    a = ap.parse_args()
    if a.exe:
        global EXE
        EXE = Path(a.exe)
        Clock.__init__.__defaults__ = (EXE, None)
    if a.list:
        print("\n".join(DRILLS))
        return
    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.run(["taskkill", "/F", "/IM", "jcclock.exe"], capture_output=True)
    for name, fn in DRILLS.items():
        if a.only and name not in a.only:
            continue
        log("== %s ==" % name)
        try:
            fn()
        except Exception as e:
            check(name + ": drill ran", False, "%s: %s" % (type(e).__name__, e))
        subprocess.run(["taskkill", "/F", "/IM", "jcclock.exe"], capture_output=True)
        time.sleep(1)
    bad = [r for r in results if not r[1]]
    log("== %d checks, %d failed ==" % (len(results), len(bad)))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
