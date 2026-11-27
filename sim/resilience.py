"""
Drills on the full simulator (jcclock.exe): what the clock does when things
go wrong or change - for every new version, a fix, or a complaint to
reproduce, now that the clock itself is with the parents.

    C:/pio3/jc_clean/venv/Scripts/python sim/resilience.py [--only NAME ...] [--list]

Each drill starts a fresh simulated clock (the staged files, C:/pio3/jc_clean/fs,
plus the settings and events in Data/), acts on it through the page's own API
and the control port (Wi-Fi world, power, light), and checks the outcome in
its answers and its own log. Results: C:/pio3/jc_clean/sim/resilience/<time>/ (one
folder and run.log per drill), summary.txt.
"""
import argparse
import datetime
import gzip
import json
import os
import tempfile
import shutil
import socket
import stat
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = Path("C:/pio3/jc_clean/sim/build/jcclock.exe")
# The page files as uploadfs would put them - staged from Data/ at the start of each run
# (main()); until 2026-11-08 the last USB upload's folder was used: stale pages.
STAGED = None
# + the process: two runs started in the same second shared a folder - and their clocks' files (2026-11-11)
OUT = Path("C:/pio3/jc_clean/sim/resilience") / (time.strftime("%Y%m%d_%H%M%S") + "_%d" % os.getpid())
sys.path.insert(0, str(Path(__file__).resolve().parent))
from sim_ports import no_real_feed, urls  # noqa: E402
from clock_facts import build_words, ntp_now, screen_text, test_random  # noqa: E402 - the clock's words: asked (Texts.h)
from time_relay import ensure_relay  # noqa: E402 - the test clocks' time server
HOME, AP, CTL = urls("resilience")   # its own ports, not the work simulator's (sim_ports.py)
SUMMARY = None   # a drill run by --jobs: its own lines file (_logs/<drill>.txt), merged by the parent
SLOT_PORTS = 20  # --jobs: drill clock k runs on the drills' ports + k * this (8180..8330 for 8 at once)
results = []


# ── plumbing ──────────────────────────────────────────────────────────────────
def log(msg):
    line = time.strftime("%H:%M:%S ") + msg
    print(line, flush=True)
    with open(SUMMARY or OUT / "summary.txt", "a", encoding="utf-8") as f:
        f.write(line + "\n")


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    log("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


def net_snapshot(port):
    """The PC's TCP connections when a request failed (investigation 2026-11-15): totals by state,
    and the clock's port's own (is it listening, what is queued on it)."""
    try:
        out = subprocess.run(["netstat", "-ano", "-p", "TCP"], capture_output=True, text=True, timeout=30).stdout
    except Exception as e:
        return "netstat failed: %s" % e
    total, mine = {}, {}
    for line in out.splitlines():
        f = line.split()
        if len(f) < 4 or f[0] != "TCP":
            continue
        total[f[3]] = total.get(f[3], 0) + 1
        if f[1].endswith(":%d" % port) or f[2].endswith(":%d" % port):
            k = ("local " if f[1].endswith(":%d" % port) else "remote ") + f[3]
            mine[k] = mine.get(k, 0) + 1
    return "all %s; port %d: %s" % (total, port, mine)


def http(base, path, data=None, timeout=20, raw=False, form=None, headers=None):
    if form is not None:
        data = urllib.parse.urlencode(form).encode()
    req = urllib.request.Request(base + path, data=data, headers=headers or {})
    if form is not None:
        req.add_header("Content-Type", "application/x-www-form-urlencoded")
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            body, code = r.read(), r.status
            if r.headers.get("Content-Encoding") == "gzip":
                body = gzip.decompress(body)
    except urllib.error.HTTPError as e:
        body, code = e.read(), e.code
    except Exception as e:
        # No answer at all - said with what the PC's network looked like then (investigation
        # 2026-11-15: an upload got WinError 10060 after 21 s, the clock running on). Not for the
        # control port's quick polls of a clock that is restarting (ctl() expects those).
        if base != CTL and time.time() - t0 > 5:
            port = int(base.rsplit(":", 1)[1])
            log("   NO ANSWER %s%s after %.1f s (%d bytes sent): %r - %s" % (base, path, time.time() - t0, len(data or b""), e, net_snapshot(port)))
        raise
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


_WORDS = []


def W():
    """The words of the build under test (its --texts: Texts.h) - for its log and answers, also
    while an older version runs on a drill clock. A name not listed: a failed check saying so."""
    if not _WORDS:
        _WORDS.append(build_words(EXE, missing=lambda n: check("the clock lists a text %r (--texts)" % n, False)))
    return _WORDS[0]


def stext(name, *fill, base=None):
    """The clock's words for a screen situation (/api/texts, Texts.h), once it answers; not
    listed = a failed check saying so, and words no screen shows (clock_facts.screen_text()).
    base: AP when the clock is off its home network."""
    def ask():
        try:
            return jget(base or HOME, "/api/texts")
        except Exception:
            return None
    texts = wait(ask, 60, 1) or {}
    return screen_text(texts, name, *fill, missing=lambda n: check("the clock lists a screen text %r (/api/texts)" % n, False))


def tunables(base=None):
    """The clock's own numbers a drill depends on (/api/tunables, Tunables.h - never a copy here,
    the user 2026-11-19), once the clock answers: up to a minute, as right after a start. base:
    HOME as it is now, or AP when the clock is off its home network (no Wi-Fi, on trial)."""
    def ask():
        try:
            return jget(base or HOME, "/api/tunables")
        except Exception:
            return None
    got = wait(ask, 60, 1)
    if not got:
        raise RuntimeError("the clock didn't answer /api/tunables within 60 s")
    return got


def api_post(path, obj, base=None, timeout=60):
    """As the page's apiPost(): form field body=<JSON>. base=None: HOME as it is NOW
    (a default of HOME was fixed at import, before --port-offset - drills then
    changed another simulator's settings, 2026-11-06)."""
    return http(base or HOME, path, form={"body": json.dumps(obj, ensure_ascii=False)}, raw=True, timeout=timeout)


def ctl(path, body=None):
    try:
        return http(CTL, path, data=body if body is not None else (b"" if path != "/state" and path != "/world" else None), timeout=10)
    except Exception as e:
        # power-cut / reset / stop: the child is gone before it answers - normal for those; any other
        # command said so (2026-11-21: a lost /ntp-port went unnoticed for a whole drill)
        if not path.startswith(("/state", "/power-cut", "/reset", "/stop")):
            print("     (control %s: no answer - %s)" % (path.split("?")[0], type(e).__name__), flush=True)
        return b""


def state():
    return json.loads(ctl("/state") or b"{}")


def point_ntp_at(srv):
    """The simulated clock's time questions (127.x port 123) to the drill's FakeTimeServers - asked
    until the simulator says "ok" (2026-11-21, a heavily loaded run: one call before its control port
    answered was lost, and the whole NTP drill ran against nothing)."""
    ok = wait(lambda: ctl("/ntp-port?port=%d" % srv.port) == b"ok", 60, 0.5)
    check("the drill's time servers in place (the simulator answered)", ok)
    return ok


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


def stays(cond, seconds, step=0.5):
    """A "must not change" check: cond watched for the whole time, False at the first moment it
    isn't true (an error counts) - not one look after a fixed wait (the user, 2026-11-14)."""
    end = time.time() + seconds
    while True:
        try:
            if not cond():
                return False
        except Exception:
            return False
        if time.time() > end:
            return True
        time.sleep(step)


def up(base=None):
    try:
        return http(base or HOME, "/api/status", timeout=3) and True
    except Exception:
        return False


def synced():
    try:
        return jget(HOME, "/api/status").get("synced")
    except Exception:
        return False


def rmtree(p):
    def clear(func, path, _):
        os.chmod(path, stat.S_IWRITE)
        func(path)
    if Path(p).exists():
        shutil.rmtree(p, onerror=clear)


def port_taken(url):
    host, port = url.rsplit("/", 1)[-1].rsplit(":", 1)
    try:
        socket.create_connection((host, int(port)), timeout=1).close()
        return True
    except OSError:
        return False


def ports_free(seconds):
    """This drill's ports free within `seconds` (a clock just stopped: Windows ends it a moment
    after taskkill returns - the next drill in the same slot found its port taken, 2026-11-15)."""
    return wait(lambda: not port_taken(HOME) and not port_taken(CTL), seconds, 0.25)


class Clock:
    """One fresh simulated clock for one drill."""
    def __init__(self, name, exe=EXE, world=None, cfg=None, priority=None, real_ntp=False):
        # Never beside another clock on the same ports: its notes went to the work
        # simulator and stop() switched that one off (2026-11-07). A clock still ending
        # (the drill before, same slot) gets a few seconds.
        if not ports_free(10):
            sys.exit("port %s or %s is taken (a simulator running there?) - run with --port-offset 100" % (HOME, CTL))
        self.dir = OUT / name
        self.fs, self.state = self.dir / "fs", self.dir / "state"
        rmtree(self.dir)
        shutil.copytree(STAGED, self.fs)
        for f in ("config.json", "events.json"):   # the clock's live settings and events (synced)
            shutil.copy2(ROOT / "Data" / f, self.fs / f)
        no_real_feed(self.fs)
        for d in ("images", "custom"):
            if (ROOT / "Data" / d).exists():
                shutil.copytree(ROOT / "Data" / d, self.fs / d, dirs_exist_ok=True)
        self.state.mkdir(parents=True)
        if world is not None:
            (self.state / "world.json").write_text(json.dumps(world), encoding="utf-8")
        if exe == OLD_EXE and not (cfg or {}).get("ntpServers"):
            # an older version asks only its FIRST time server, and the drill is about something else:
            # one pool.ntp.org member was silent for minutes twice in a night (2026-11-20 21:56,
            # 11-21 08:57 - 185.18.207.158) and an old version's drill failed on it. Cloudflare's
            # anycast first; today's version, with its three, keeps the normal list.
            cfg = dict(cfg or {}, ntpServers=["time.cloudflare.com", "pool.ntp.org", "time.google.com"])
        if cfg:   # settings of its own, from the start
            f = self.fs / "config.json"
            f.write_text(json.dumps(dict(json.loads(f.read_text(encoding="utf-8")), **cfg), ensure_ascii=False), encoding="utf-8")
        self.exe, self.name, self.priority = exe, name, priority
        self.real_ntp = real_ntp or exe == OLD_EXE   # the real time servers (an older version knows no relay)
        self.logf = open(self.dir / "run.log", "wb")
        self.start()

    def start(self):
        """Starts it (again, after stop(): its flash and state as they were)."""
        if self.logf.closed:
            self.logf = open(self.dir / "run.log", "ab")
        exe, name = self.exe, self.name
        ports = ["--home-port", HOME.rsplit(":", 1)[1], "--ap-port", AP.rsplit(":", 1)[1], "--control-port", CTL.rsplit(":", 1)[1]]
        env = dict(os.environ)
        if self.real_ntp:   # this drill asks the real time servers (sim/time_relay.py: all others the run's own)
            env.pop("JC_NTP_RELAY", None)
        self.p = subprocess.Popen([str(exe), "--fs", str(self.fs), "--state", str(self.state), "--verbose"] + ports,
                                  stdout=self.logf, stderr=subprocess.STDOUT, cwd=str(self.dir),
                                  env=dict(env, JC_TITLE="Test (resilience)", **({"JC_PRIORITY": self.priority} if self.priority else {}),
                                           # code coverage (run_all_tests --coverage): each drill's counts apart
                                           **({"LLVM_PROFILE_FILE": os.environ["JC_COV_DIR"] + "/resilience." + name + "/%p.profraw"}
                                              if os.environ.get("JC_COV_DIR") else {})))

    def log_text(self):
        self.logf.flush()
        with open(self.dir / "run.log", encoding="utf-8", errors="replace", newline="") as f:   # as written: a CR
            return f.read()                                                               # inside a line stays one

    def diag(self):
        return http(HOME if up() else AP, "/api/debug/diagnostic-log").decode("utf-8", "replace")

    def stop(self):
        ctl("/stop")
        if self.logf.closed:
            return
        try:
            self.p.wait(15)
        except subprocess.TimeoutExpired:
            pass
        # This clock only (supervisor and its child) - never every jcclock.exe:
        # another simulator may be running beside the drills (sim/soak.py).
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(self.p.pid)], capture_output=True)
        ports_free(15)   # really gone before the next drill takes this slot
        self.logf.close()


def default_world():
    """The world the simulator makes from the known networks (sim_wifi.cpp), read back - once the
    control port answers: right after a restart (an update installing, a reset) it is closed
    until the new program runs (2026-11-22, a loaded run: the drill read it a moment too early and
    ended on an empty answer, ConnectionResetError)."""
    got = wait(lambda: json.loads(ctl("/world")), 60, 0.5)
    if not got:
        raise RuntimeError("the simulator's control port gave no /world within 60 s")
    return got


# ── the drills ────────────────────────────────────────────────────────────────
def drill_boot_ntp():
    c = Clock("boot_ntp", real_ntp=True)   # one of the few that ask the real servers (time_relay.py)
    t0 = time.time()
    try:
        ok = wait(synced, 90)
        check("boot: time synced from NTP", ok, "after %.0f s" % (time.time() - t0))
        st = jget(HOME, "/api/status")
        st_at = time.time()
        ref, at = ntp_now(tunables()["SAME_SERVER_MIN_MS"] / 1000)   # the reference's time at `at` (a silent server first can take 5 s)
        clock_utc = st["utcTime"] + (at - st_at)   # the clock's, carried to the same moment (2026-11-20: it was
        #                                            carried from `at` to now - 5 s wrong after a silent server)
        check("boot: clock's time = a separate NTP query", abs(clock_utc - ref) < 2.5, "difference %.2f s" % (clock_utc - ref))
        check("boot: no debug offset", jget(HOME, "/api/config").get("debugTimeOffset") == 0)
        txt = c.log_text()
        check("boot: NTP first sync is a jump from 1970, logged", W().has("ntpStep", txt, W().text("ntpFirst")))
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


def drill_no_address():
    """Networks that take the clock but give it no address (2026-11-18, from the coverage list - no
    DHCP, or its pool full): the address taken back while on one -> noticed, connecting again; none
    on any network -> three tries, then its own Wi-Fi (setup mode); addresses again -> back on a
    network by itself (it tries every 2 minutes) and online."""
    c = Clock("no_address")
    try:
        wait(synced, 90)
        w = default_world()
        for n in w["networks"]:
            n["dhcp"] = False
        ctl("/world", json.dumps(w).encode())
        check("its address taken back while on the network: noticed, connecting again",
              wait(lambda: W().has("ipLoss", c.log_text()), 60, 1))
        check("no address on any network: after its tries, its own Wi-Fi (setup mode)",
              wait(lambda: W().has("revertToAp", c.log_text()), 180, 2) and wait(lambda: state().get("ap"), 30, 1))
        for n in w["networks"]:
            n["dhcp"] = True
        ctl("/world", json.dumps(w).encode())
        check("addresses again: back on a network by itself", wait(lambda: state().get("sta"), 200, 2))
        check("... and online (time, page)", wait(synced, 90) and wait(up, 30))
    finally:
        c.stop()


def drill_low_memory():
    """PSRAM almost gone (2026-11-18, from the coverage list - the simulator takes it on request,
    /psram-hog): a new event saved with a warning below 20% free, refused below 10%; an Excel
    import refused; a picture that can't be held in memory doesn't stop the clock; memory back ->
    all as before. The clock runs on throughout (page, screen)."""
    c = Clock("low_memory")
    try:
        wait(synced, 90)
        names = lambda: sorted(e["name"] for e in jget(HOME, "/api/events/list"))
        ev = lambda n: {"name": n, "hasG": True, "gD": 4, "gM": 4, "gY": 0, "type": "other"}
        ctl("/psram-hog?free_pct=15")
        code, said = api_post("/api/events/upsert", ev("UIT Mem A"))
        t = said.decode("utf-8", "replace")
        check("15% of PSRAM free: a new event saved, with a warning", code == 200 and '"saved":true' in t.replace(" ", "") and W().has("psramSavedLow", t), t[:160])
        ctl("/psram-hog?free_pct=5")
        before = names()
        code, said = api_post("/api/events/upsert", ev("UIT Mem B"))
        t = said.decode("utf-8", "replace")
        check("5% free: a new event refused, says why", '"saved":false' in t.replace(" ", "") and W().has("psramEventRefused", t) and names() == before, t[:160])
        code, said = api_post("/api/events/import", jget(HOME, "/api/events/list") + [ev("UIT Mem C")])
        t = said.decode("utf-8", "replace")
        check("5% free: an Excel import refused, says why, nothing changed", code == 400 and W().has("psramImportRefused", t) and names() == before, (code, t[:160]))
        ctl("/psram-hog?free_pct=0.5")
        code, msg = upload_picture(bmp16(72, 96, 7))
        check("0.5% free: a picture upload answered (refused or kept, not shown) - the clock runs on",
              code in (200, 400, 500) and wait(up, 10) and state().get("screen") is not None, (code, msg[:100]))
        check("... the page still loads", b"<html" in http(HOME, "/").lower())
        ctl("/psram-hog?free_pct=-1")
        code, said = api_post("/api/events/upsert", ev("UIT Mem D"))
        check("memory back: a new event saved", code == 200 and "UIT Mem D" in names(), said[:120])
        for e in jget(HOME, "/api/events/list"):
            if e["name"].startswith("UIT Mem"):
                http(HOME, "/api/events/delete", form={"id": e["id"]})
    finally:
        c.stop()


def drill_weak_signal():
    """The signal weak, then good again (2026-11-18, the last open item of the coverage list): one
    log line when it drops below the clock's WIFI_RSSI_WEAK_DBM, one when it is back above WIFI_RSSI_RECOVERED_DBM (asked) - how long and how low.
    Every network weak, so roaming has nowhere better to go."""
    c = Clock("weak_signal")
    try:
        wait(synced, 90)
        T = tunables()   # the clock's levels (2026-11-21: -88 / -60 were copies - the numbers run moved them)
        low, good = T["WIFI_RSSI_WEAK_DBM"] - 3, T["WIFI_RSSI_RECOVERED_DBM"] + 20
        sample_s = T["WIFI_RSSI_SAMPLE_MS"] / 1000
        w = default_world()
        for n in w["networks"]:
            n["rssi"] = low
        ctl("/world", json.dumps(w).encode())
        check("signal weak: logged with its reading", wait(lambda: W().has("signalWeak", c.diag(), low), 6 * sample_s, 1))
        for n in w["networks"]:
            n["rssi"] = good
        ctl("/world", json.dumps(w).encode())
        check("signal good again: 'recovered', how long and how low", wait(lambda: W().has("signalRecovered", c.diag(), good, None, low), 6 * sample_s, 1))
        check("... online throughout", synced() and up())
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
        check("Wi-Fi: the drop is logged (reason 200)", W().has("wifiDisconnected", txt, 200))
    finally:
        c.stop()


def to_friday_morning():
    """The drill clock to the coming Friday, 10:00 (debug time), its calendar for that day in:
    a drill that needs candle lighting, or the full set of slides, must not depend on the
    day it runs - two failed on a Sunday night (2026-11-08)."""
    import datetime
    d = datetime.date.today()
    d += datetime.timedelta(days=(4 - d.weekday()) % 7 or 7)   # the next Friday, never today
    api_post("/api/debug/time", {"local": d.isoformat() + "T10:00"})
    return wait(lambda: jget(HOME, "/api/calendar").get("candleTime"), 120, 2)


def drill_wifi_all_down():
    """No known network around: setup mode; back when one returns (PORTAL_RETRY_MS)."""
    c = Clock("wifi_all_down")
    t_boot = time.time()
    try:
        wait(synced, 90)
        # the clock's own words, asked before its home network goes (Texts.h)
        UNREACH, SETUP_LINE = stext("noInternet"), stext("setupLine")   # the setup line: up to its first variable part
        T = tunables()   # the clock's own numbers, never copies (the user, 2026-11-19)
        EVERY_N, STARTUP_S = T["OFFLINE_NOTICE_EVERY_N_ROUNDS"], T["BOOT_OVERLAY_MS"] / 1000
        check("a Friday morning on the drill clock (its calendar in)", to_friday_morning())
        w = default_world()
        for n in w["networks"]:
            n["up"] = False
        ctl("/world", json.dumps(w).encode())
        # Once online, the clock's own Wi-Fi stays its synchronized one ("JewishClock"):
        # the full page must work there (a redirect loop before 2.1.1), and the screen
        # keeps its slides plus "Internet unreachable" - every round in the startup
        # minutes, then every 4th (the user, 2026-11-06).
        first = wait(lambda: UNREACH in state().get("screen", ""), T["OFFLINE_NOTICE_AFTER_MS"] / 1000 + 40, 0.5)
        check("Wi-Fi all gone: 'Internet unreachable' on screen within %.0f s" % (T["OFFLINE_NOTICE_AFTER_MS"] / 1000 + 40), first)
        # The coming rounds asked of the clock (/state?rounds - its own round code, live state), not
        # watched for minutes (2026-11-18: the drill took 10 min, most of it watching the screen)
        def rounds(n=8):
            return json.loads(ctl("/state?rounds=%d" % n) or b"{}").get("nextRounds", [])
        def notice_in(rs):
            return [any(UNREACH in sl for sl in r) for r in rs]
        rs = rounds()
        check("Wi-Fi all gone, startup minutes: notice every round, slides between",
              rs and all(notice_in(rs)) and all(len(r) >= 2 for r in rs), [len(r) for r in rs])
        setup_seen = any(SETUP_LINE in sl for r in rs for sl in r)
        code, page = http(AP, "/", raw=True)
        check("Wi-Fi all gone: the page works on the clock's own Wi-Fi", code == 200 and b"<html" in page.lower(), "status %s" % code)
        check("Wi-Fi all gone: its settings answer there", "wifiSSID" in jget(AP, "/api/config"))
        time.sleep(max(0, t_boot + STARTUP_S + 20 - time.time()))   # past the startup minutes
        rs = rounds(3 * EVERY_N)
        on = notice_in(rs)
        check("Wi-Fi all gone, later: notice every %dth round" % EVERY_N, 2 <= sum(on) <= 4 and any(len(r) >= 2 for r in rs),
              "%d of %d rounds" % (sum(on), len(on)))
        setup_seen |= any(SETUP_LINE in sl for r in rs for sl in r)
        check("Wi-Fi all gone: never the setup line (its Wi-Fi is JewishClock)", not setup_seen)
        for n in w["networks"]:
            n["up"] = True
        ctl("/world", json.dumps(w).encode())
        back = wait(lambda: state().get("sta"), T["PORTAL_RETRY_MS"] / 1000 + 80, 3)
        check("Wi-Fi back: rejoins by itself within its retry (%.0f s)" % (T["PORTAL_RETRY_MS"] / 1000), back)
        check("Wi-Fi back: notice gone", wait(lambda: not any(notice_in(rounds(6))), 30, 1))
    finally:
        c.stop()


def drill_setup_then_back():
    """Started with no network it can join: setup mode, its own Wi-Fi "...-Setup". The network
    comes back: it rejoins - and its own Wi-Fi must be called by the clock's name again. Found at
    the parents' 2026-11-20: 2.1.2, 9 days up, on the home Wi-Fi, still broadcasting
    "JewishClock-Setup" (its page said "JewishClock"); a power cycle fixed it."""
    c = Clock("setup_then_back")
    try:
        wait(synced, 90)
        name = jget(HOME, "/api/config")["projectName"]
        retry_s = tunables()["PORTAL_RETRY_MS"] / 1000
        set_world(networks_up=False)
        ctl("/power-cut")
        check("no network at the start: setup mode", wait(in_setup_mode, 180, 2))
        check("... its own Wi-Fi under the setup name", wait(lambda: state().get("apSsid", "").endswith("Setup"), 30, 1),
              state().get("apSsid"))
        set_world(networks_up=True)
        check("the network back: rejoined by itself (its retry, %.0f s)" % retry_s,
              wait(lambda: state().get("sta"), retry_s + 90, 2) and wait(synced, 90, 1))
        check("... its own Wi-Fi called by the clock's name again (%r), not the setup name" % name,
              wait(lambda: state().get("apSsid") == name, 60, 1), state().get("apSsid"))
        check("... and its page says what the radio says", jget(HOME, "/api/status").get("apSsid") == state().get("apSsid"),
              (jget(HOME, "/api/status").get("apSsid"), state().get("apSsid")))
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
        check("wrong password: logged as auth failure (202)", W().has("wifiDisconnected", c.log_text(), 202))
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
        check("no internet: page = 'initializing'", W().has("initializing", page.decode("utf-8", "replace")))
        for n in w["networks"]:
            n["internet"] = True
        ctl("/world", json.dumps(w).encode())
        # (2026-11-21, the user: a silent time server may be no internet at all - checked elsewhere, the
        # servers not held to blame, and the moment the internet answers again, the time)
        check("no internet: the clock checked the internet and logged it gone, not the time servers",
              W().has("logFailing", c.diag(), W().text("netStreak")) and not W().has("ntpRest", c.diag()), c.diag()[-300:])
        gap = tunables()["PROBE_MIN_GAP_MS"] / 1000
        check("internet back: the time within the internet check's spacing (%.0f s), logged" % gap,
              wait(synced, gap + 30) and wait(lambda: W().has("ntpInternetBack", c.diag()), 10, 0.5))
    finally:
        c.stop()


def drill_settings_power_cut():
    """A change survives a power cut once saved (CONFIG_SAVE_QUIET_MS of quiet); within that it's lost - by design."""
    c = Clock("settings_power_cut")
    try:
        wait(synced, 90)
        quiet_s = tunables()["CONFIG_SAVE_QUIET_MS"] / 1000
        api_post("/api/config", {"scrollSpeed": 5})
        time.sleep(2 * quiet_s)
        ctl("/power-cut")
        wait(synced, 90)
        check("settings: saved change survives a power cut", jget(HOME, "/api/config").get("scrollSpeed") == 5)
        api_post("/api/config", {"scrollSpeed": 7})
        ctl("/power-cut")
        wait(up, 90)
        sp = jget(HOME, "/api/config").get("scrollSpeed")
        check("settings: change cut within its %.0f s of quiet - old value, file intact" % quiet_s, sp == 5, "scrollSpeed=%s" % sp)
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


SETTINGS_CHANGE = {   # every setting the page posts to /api/config (WebUI.cpp), each to a value it doesn't have
    "rolloverMode": 0, "use24Hour": False, "rotationMode": 1, "messageDwellSec": 7, "separator": " ~ ",
    "scrollSpeed": 6, "fontCharGapPx": 2, "fontWordGapPx": 12, "fontWidthScalePct": 90, "brightPct": 60,
    "useLightSensor": False, "nightPct": 20.5, "dayPct": 80, "roomDarkPct": 3, "roomLightPct": 70, "learnRoomLight": False,
    "calibX": 2, "calibY": 30, "calibW": 470, "calibH": 95, "calibTearUs": 9000,
    "showHebrewYear": False, "showShabbatShalom": True, "showRoshChodesh": False, "showFasts": False, "omerMode": 1,
    "showSpecialShabbat": False, "showLiturgical": False, "showSwitchover": False, "prayerSwitchDays": 1, "nusach": 1,
    "eventDayChange": 1,
}


def same(a, b):
    if isinstance(a, float) or isinstance(b, float):
        try:
            return abs(float(a) - float(b)) < 0.06
        except (TypeError, ValueError):
            return False
    return a == b


def drill_settings_roundtrip():
    """Every setting: changed from the page's API, read back, still so after a restart; nothing else moves."""
    c = Clock("settings_roundtrip")
    try:
        wait(synced, 90)
        before = jget(HOME, "/api/config")
        order = before.get("contentOrder")
        new_order = list(reversed(order)) if isinstance(order, list) else list(reversed(order.split()))
        freq = before.get("contentFrequency") or {}
        new_freq = {k: (0 if v else 1) for k, v in freq.items()}
        change = dict(SETTINGS_CHANGE, contentOrder=new_order, contentFrequency=new_freq,
                      wifiSSID3="DrillNet", wifiPassword3="drill-pass-1")
        code, said = api_post("/api/config", change)
        check("settings: all accepted in one go", code == 200, said.decode()[:40])

        def compare(cfg, when):
            bad = [k for k, v in SETTINGS_CHANGE.items() if not same(cfg.get(k), v)]
            got_order = cfg.get("contentOrder"); got_order = got_order.split() if isinstance(got_order, str) else got_order
            if got_order != new_order: bad.append("contentOrder")
            if {k: v for k, v in (cfg.get("contentFrequency") or {}).items() if k in new_freq} != new_freq: bad.append("contentFrequency")
            if cfg.get("wifiSSID3") != "DrillNet": bad.append("wifiSSID3")
            check("settings: every change %s" % when, not bad, "differ: %s" % bad if bad else "%d settings" % (len(SETTINGS_CHANGE) + 3))
            untouched = [k for k in before if k not in change and k not in ("wifiPassword", "wifiPassword2", "wifiPassword3", "debugTimeOffset")
                         and not same(before[k], cfg.get(k))]
            check("settings: nothing else changed %s" % when, not untouched, "changed: %s" % untouched)
        compare(jget(HOME, "/api/config"), "read back at once")
        time.sleep(4)   # the save lands 2 s after the last change
        ctl("/reset")
        wait(up, 90)
        time.sleep(3)
        compare(jget(HOME, "/api/config"), "after a restart")
        api_post("/api/config", {"wifiSSID3": ""})
        time.sleep(4)
        check("settings: a network removed again", jget(HOME, "/api/config").get("wifiSSID3", "") == "")
    finally:
        c.stop()


def drill_day_change():
    """The Hebrew date changes at nightfall (צאת הכוכבים), live, as the clock runs into it."""
    c = Clock("day_change")
    try:
        wait(synced, 90)
        # Midday first: after nightfall the calendar has no "tzeit" left for the day.
        today = time.strftime("%Y-%m-%d")
        api_post("/api/debug/time", {"local": today + "T12:00"})
        cal = wait(lambda: (lambda x: x if x.get("tzeit") else None)(jget(HOME, "/api/calendar")), 90, 2)
        tzeit, before = cal["tzeit"], cal["hebrewDate"]
        h, m = map(int, tzeit.split(":"))
        t = h * 60 + m - 2   # two minutes before nightfall
        api_post("/api/debug/time", {"local": "%sT%02d:%02d" % (today, t // 60, t % 60)})
        wait(lambda: jget(HOME, "/api/calendar")["hebrewDate"] == before, 60)
        seen_before = jget(HOME, "/api/calendar")["hebrewDate"]
        after = wait(lambda: (lambda d: d if d != seen_before else None)(jget(HOME, "/api/calendar")["hebrewDate"]), 300, 5)
        check("day change: Hebrew date moves on at nightfall (%s)" % tzeit, after, "%s -> %s" % (seen_before, after))
        shown = wait(lambda: (lambda s: s if after and after in s else None)(state().get("screen", "")), 120, 0.5)
        check("day change: the screen shows the new date", shown, (shown or "")[:60])
        api_post("/api/debug/time", {"offset": 0})
    finally:
        c.stop()


def drill_place_follows_network():
    """Each network has its place: moving to the other one switches city (and candle times)."""
    c = Clock("place_follows_network")
    try:
        wait(synced, 90)
        cfg = jget(HOME, "/api/config")
        ssids = [cfg.get("wifiSSID"), cfg.get("wifiSSID2")]
        places = [p.get("city") for p in cfg.get("places", [])]
        here = state().get("ssid")
        other = ssids[1] if here == ssids[0] else ssids[0]
        want = places[ssids.index(other)]
        check("a Friday morning on the drill clock (its calendar in)", to_friday_morning())
        city0, candle0 = cfg.get("city"), jget(HOME, "/api/calendar").get("candleTime")
        w = default_world()
        for n in w["networks"]:
            n["up"] = n["ssid"] == other
        ctl("/world", json.dumps(w).encode())
        wait(lambda: state().get("ssid") == other and state().get("sta"), 120)
        city = wait(lambda: (lambda x: x if x == want else None)(jget(HOME, "/api/config").get("city")), 60)
        check("place: on %s the place is %s" % (other, want), city, "%s -> %s" % (city0, jget(HOME, "/api/config").get("city")))
        # The calendar is fetched for the new place (its latitude in Hebcal's request) and
        # gives a candle time. Not "the minute changed": two cities 30 km apart often light at
        # the same minute (the home city and the home city on 2026-11-20: both 16:18 - failed 2026-11-13).
        lat = "latitude=%.4f" % cfg["places"][ssids.index(other)]["lat"]
        asked = wait(lambda: lat in c.log_text().rsplit("[sim] JewishClock ", 1)[-1], 90, 3)
        candle = asked and wait(lambda: jget(HOME, "/api/calendar").get("candleTime"), 60, 3)
        check("place: candle lighting follows (%s: its calendar fetched, %s)" % (want, lat), city0 == want or (asked and candle),
              "%s -> %s, asked=%s" % (candle0, candle, bool(asked)))
    finally:
        c.stop()


def drill_log_rotation():
    """The diagnostic log at its cap (128 KB per file since 2026-11-07): moves to .old, keeps writing, stays readable."""
    c = Clock("log_rotation")
    try:
        wait(synced, 90)
        cap = jget(HOME, "/api/limits")["logFileMaxBytes"]
        lines = int(cap * 1.3 / 170) + 1   # each line ~170 bytes with its time and category: past one file's cap
        for i in range(lines):
            http(HOME, "/api/debug/diagnostic-log/note", form={"msg": "drill line %05d " % i + "x" * 130})
        time.sleep(5)
        log = c.diag()
        cur = c.fs / "log" / "diagnostic_log.txt"
        old = c.fs / "log" / "diagnostic_log.old.txt"
        check("log: rotated at the cap (.old exists)", old.exists(), "%d / %d bytes" % (cur.stat().st_size if cur.exists() else -1, old.stat().st_size if old.exists() else -1))
        check("log: current file stays within its cap (%d bytes) + one line" % cap, cur.exists() and cur.stat().st_size <= cap + 1024,
              cur.stat().st_size if cur.exists() else None)
        check("log: the last line is in what the page gets", "drill line %05d" % (lines - 1) in log)
    finally:
        c.stop()


def damaged_pictures_and_files(c):
    """Part of drill_corrupt_files (2026-11-18, from the coverage list): event pictures broken in
    every way the loader checks - the event shows its type icon instead, the clock runs on - and a
    valid xRGB1555 picture shows; a garbage time file, settings starting with a BOM, a calendar
    file with an impossible Hebrew date for today."""
    import struct
    src = (ROOT / "Data" / "IMG" / "Baby.bmp").read_bytes()   # 80x96, 16-bit BI_BITFIELDS 565

    def patched(at, fmt, value, data=src):
        b = bytearray(data)
        struct.pack_into(fmt, b, at, value)
        return bytes(b)
    pics = {   # id (8 hex) -> (what, bytes, shows)
        "0ad0b001": ("24 bits a pixel", patched(28, "<H", 24), False),
        "0ad0b002": ("unknown compression (2)", patched(30, "<I", 2), False),
        "0ad0b003": ("height 0", patched(22, "<i", 0), False),
        "0ad0b004": ("height 5000", patched(22, "<i", 5000), False),
        "0ad0b005": ("cut short", src[:len(src) // 3], False),
        "0ad0b006": ("unknown colour masks", patched(54, "<I", 0x00FF), False),
        "0ad0b555": ("xRGB1555 (valid)", patched(62, "<I", 0x001F, patched(58, "<I", 0x03E0, patched(54, "<I", 0x7C00))), True),
    }
    today = jget(HOME, "/api/status")["date"]
    y, m, d = (int(x) for x in today.split("-"))
    for pid, (what, _, _) in pics.items():
        api_post("/api/events/upsert", {"name": "UIT Pic " + pid, "hasG": True, "gD": d, "gM": m, "gY": 0, "hasJ": False,
                                        "type": "birthday", "icon": "", "warn": 0, "image": pid})
    c.stop()
    (c.fs / "images").mkdir(exist_ok=True)
    for pid, (_, data, _) in pics.items():
        (c.fs / "images" / (pid + ".bmp")).write_bytes(data)
    c.start()
    check("damaged pictures: the clock starts and runs", wait(synced, 120))
    # the events' day held at its noon (debug time): a run across midnight moved "today" away from
    # them mid-check (2026-11-21 00:00, the night's uptime-wrap run - every picture "not shown")
    api_post("/api/debug/time", {"local": today + "T12:00"})
    wait(lambda: jget(HOME, "/api/status").get("date") == today, 30, 1)
    rounds = json.loads(ctl("/state?rounds=4") or b"{}").get("nextRounds", [])
    slides = [s for r in rounds for s in r]
    for pid, (what, _, shows) in pics.items():
        mine = [s for s in slides if ("UIT Pic " + pid) in s]
        own = any(("/images/%s.bmp" % pid) in s for s in mine)
        check("event picture %s: %s" % (what, "shown" if shows else "not used - the event shows its icon"),
              mine and own == shows, mine[:1])
    api_post("/api/debug/time", {"offset": 0})   # back to real time
    log = c.log_text()
    for words in ("bmpNot16Bit", "bmpCompression", "bmpHeight", "bmpMasks"):
        check("... the log says why (%s)" % words, W().has(words, log))
    for e in jget(HOME, "/api/events/list"):
        if e["name"].startswith("UIT Pic "):
            http(HOME, "/api/events/delete", form={"id": e["id"]})

    # garbage time file, settings with a BOM, today's Hebrew date impossible in the calendar file
    # (the reference once the clock has today's date - taken while it still said "Loading..." once, 2026-11-20)
    loading = stext("loading")
    before = wait(lambda: (lambda s: s if s.get("hebrewDate") and s["hebrewDate"] != loading else None)(jget(HOME, "/api/status")), 60, 1) or jget(HOME, "/api/status")
    name0 = jget(HOME, "/api/config")["projectName"]
    check("... the calendar file written (after the download, on a still screen)",
          wait(lambda: (c.fs / "jewish_calendar_data.json").exists(), 120, 1))
    c.stop()
    (c.fs / "time_seen.txt").write_text("not a time" + chr(10), encoding="utf-8")
    cfg_bytes = (c.fs / "config.json").read_bytes()
    (c.fs / "config.json").write_bytes(b"\xef\xbb\xbf" + cfg_bytes.lstrip(b"\xef\xbb\xbf"))
    cal = json.loads((c.fs / "jewish_calendar_data.json").read_text(encoding="utf-8"))
    for day in cal:
        if day.get("g") == today:
            day["hd"], day["hm"] = 99, 42
    (c.fs / "jewish_calendar_data.json").write_text(json.dumps(cal, ensure_ascii=False), encoding="utf-8")
    c.start()
    check("garbage time file, BOM settings, impossible Hebrew date: the clock starts", wait(synced, 120))
    check("... settings with a BOM read (its name kept)", jget(HOME, "/api/config")["projectName"] == name0)
    check("... today's Hebrew date right (worked out from the Gregorian day)",
          wait(lambda: jget(HOME, "/api/status")["hebrewDate"] == before["hebrewDate"], 30, 1),
          (jget(HOME, "/api/status")["hebrewDate"], before["hebrewDate"]))


def drill_service_garbage():
    """Hebcal answering 200 with nonsense (a captive portal's page) or with an empty object (2026-11-18,
    from the coverage list): the calendar it has is kept, nothing written over its file, each
    tried again; right answers again -> a normal download."""
    c = Clock("service_garbage")
    try:
        wait(synced, 90)
        check("garbage: today's calendar there first", wait(lambda: jget(HOME, "/api/calendar").get("dafYomi"), 90, 2))
        daf = jget(HOME, "/api/calendar")["dafYomi"]
        wait(lambda: (c.fs / "jewish_calendar_data.json").exists(), 120, 1)   # written after the times, on a still screen
        cache = (c.fs / "jewish_calendar_data.json").read_bytes()
        set_world(blocked=["garbage:hebcal.com/hebcal", "garbage:hebcal.com/zmanim"])
        api_post("/api/calendar/refresh", {})
        check("Hebcal answers a web page: 'no usable calendar', the old one kept",
              wait(lambda: W().has("hebcalUnusable", c.log_text()), 60, 1) and jget(HOME, "/api/calendar").get("dafYomi") == daf)
        check("... nothing written over the calendar file", (c.fs / "jewish_calendar_data.json").read_bytes() == cache)
        set_world(blocked=["emptyjson:hebcal.com/zmanim"])
        api_post("/api/calendar/clear-cache", {})
        check("the times answer '{}': 'no sunset times - will retry'", wait(lambda: W().has("zmanimNoSunset", c.log_text()), 90, 1))
        n0 = W().count("zmanimNoSunset", c.log_text())
        set_world(blocked=["garbage:hebcal.com/zmanim"])
        # read through a filter, a web page parses as an empty document (ArduinoJson: Ok) - the "no
        # sunset times" check is what catches it (tried 2026-11-18)
        check("the times answer a web page: 'no sunset times', tried again",
              wait(lambda: W().count("zmanimNoSunset", c.log_text()) > n0, 120, 1))
        set_world(blocked=["cut:hebcal.com/zmanim"])
        check("the times answer cut off in the middle: a parse error, tried again", wait(lambda: W().has("zmanimParseError", c.log_text()), 150, 1))
        set_world(blocked=[])
        check("right answers again: the times arrive (logged as working again)",
              wait(lambda: W().has("logWorksAgain", c.diag(), W().text("zmanimStreak")), 150, 2))
        check("... the calendar as before", jget(HOME, "/api/calendar").get("dafYomi") == daf)
    finally:
        c.stop()


def drill_corrupt_files():
    """Flash damage: event pictures, the time file, settings with a BOM, the calendar file (the clock
    copes, see damaged_pictures_and_files()); then settings or events unreadable - the clock still
    starts, no restart loop."""
    c = Clock("corrupt_files")
    try:
        wait(synced, 90)
        damaged_pictures_and_files(c)
        (c.fs / "events.json").write_bytes(b"\x00\xff{ not json")
        ctl("/reset")
        check("damaged events: clock comes back online", wait(synced, 120))
        # (the live output reaches its file in chunks - by now, after a restart, the start before is in it)
        check("garbage time file: ignored, said so", wait(lambda: W().has("timeSeenIgnored", c.log_text(), "/time_seen.txt"), 30, 1))   # (a CR follows: the file was written on Windows)
        check("damaged events: running, events list answers", isinstance(jget(HOME, "/api/events/list"), list),
              "%d events" % len(jget(HOME, "/api/events/list")))
        (c.fs / "config.json").write_bytes(b"{\"wifiSSID\": tr")
        starts0 = c.log_text().count("[sim] JewishClock")   # the starts before this one (the parts above restart it too)
        ctl("/reset")
        time.sleep(30)
        starts = c.log_text().count("[sim] JewishClock") - starts0
        check("damaged settings: no restart loop (one start, at most one more)", starts <= 2, "%d starts since the reset" % starts)
        check("damaged settings: reachable (setup mode or online)", in_setup_mode() or up(), "")
        # no settings file at all, and a calendar file of an older format (2026-11-18)
        c.stop()
        (c.fs / "config.json").unlink(missing_ok=True)
        calf = c.fs / "jewish_calendar_data.json"
        if calf.exists():
            cal = json.loads(calf.read_text(encoding="utf-8"))
            if cal and "v" in cal[0]:
                cal[0]["v"] = 1
                calf.write_text(json.dumps(cal, ensure_ascii=False), encoding="utf-8")
        c.start()
        check("no settings file: the defaults, the setup page", wait(in_setup_mode, 120, 2) and W().has("noConfigFile", c.log_text()))
        check("... an older calendar file: ignored, said so", W().has("cacheOldFormat", c.log_text()))
    finally:
        c.stop()


def drill_roaming():
    """Mesh: the access point it's on fades - after 30 s weak it moves to a clearly stronger one (serviceRoaming)."""
    c = Clock("roaming")
    try:
        wait(synced, 90)
        w = default_world()
        here = state().get("ssid")
        net = next(n for n in w["networks"] if n["ssid"] == here)
        T = tunables()   # the clock's roaming levels and waits (2026-11-21: -86 was a copy - the numbers run moved them)
        weak_rssi = T["ROAM_WEAK_RSSI_DBM"] - 8
        weak = dict(net, rssi=weak_rssi)
        # Clearly the strongest of all: roaming goes to any known network's best
        # access point (WiFiManager.cpp serviceRoaming), not only this network's.
        # the strongest of ALL (2026-11-21: -74 was "clearly stronger" than the weak one, but another
        # network's -55 was stronger still - the clock rightly went there)
        best_other = max(n["rssi"] for n in w["networks"] if n is not net)
        strong = dict(net, rssi=min(-20, max(best_other + 10, weak_rssi + T["ROAM_MIN_GAIN_DB"] + 25)), bssid="5A:1E:00:00:AA:BB", channel=11)
        w["networks"] = [n for n in w["networks"] if n["ssid"] != here] + [weak, strong]
        strong["up"] = False
        ctl("/world", json.dumps(w).encode())          # first: only the weak one (same address = the same access point)
        time.sleep(3)
        strong["up"] = True
        ctl("/world", json.dumps(w).encode())          # a stronger node appears
        moved = wait(lambda: W().has("wifiReconnected", c.log_text(), None, None, "5A:1E:00:00:AA:BB"),
                     (T["ROAM_WEAK_FOR_MS"] + 4 * T["ROAM_SAMPLE_MS"]) / 1000 + 120, 3)   # weak long enough, then a scan
        check("roaming: moves to the clearly stronger access point", moved)
        check("roaming: the roam scan is logged", W().has("roamScan", c.log_text()))
        check("roaming: online again after the move", wait(lambda: state().get("sta"), 60))
    finally:
        c.stop()


def drill_setup_portal_save():
    """First setup: a factory-new clock, the home Wi-Fi typed into the setup page - it restarts and joins."""
    c = Clock("setup_portal_save")
    try:
        wait(synced, 90)
        w = default_world()
        target = w["networks"][0]
        http(HOME, "/api/factory-reset", form={"body": "{}"})
        check("setup: factory reset -> setup page", wait(in_setup_mode, 120, 2))
        code, html = http(AP, "/", raw=True)
        check("setup: the setup page itself", code == 200 and b"Setup" in html and b"/save" in html, "status %s" % code)
        nets = wait(lambda: (lambda j: j if j.get("networks") else None)(json.loads(http(AP, "/wifi-scan"))), 30, 2)
        check("setup: it lists the networks around", nets and any(n["ssid"] == target["ssid"] for n in nets["networks"]),
              str([n["ssid"] for n in (nets or {}).get("networks", [])]))
        # plain characters only (2026-11-21): a Hebrew name refused here too, the setup page stays
        code, said = http(AP, "/save", form={"ssid": "רשת הבית", "password": target["password"]}, raw=True)
        check("setup: a network name in Hebrew - refused, says why", code == 400
              and W().has("wifiNotPlain", said.decode("utf-8", "replace"), W().text("labelSetupNetName")), (code, said[:160]))
        code, said = http(AP, "/save", form={"ssid": target["ssid"], "password": target["password"]}, raw=True)
        check("setup: saved", code == 200 and W().has("setupSaved", said.decode("utf-8", "replace")))
        check("setup: restarts and joins the home Wi-Fi", wait(lambda: state().get("sta") and state().get("ssid") == target["ssid"], 120, 2))
        check("setup: time and page there", wait(synced, 90) and b"<html" in http(HOME, "/").lower())
    finally:
        c.stop()


def drill_ap_own_password():
    """The clean copy (2026-11-14): no fixed password for the clock's own Wi-Fi - each
    clock makes its own at random, keeps it, shows it (setup screen, Settings); a
    factory reset makes a new one. Built as the clean copy is (build_clock.py --clean-ap)."""
    exe = EXE.parent / "jcclock_cleanap.exe"
    r = subprocess.run([sys.executable, str(ROOT / "sim" / "build_clock.py"), "--clean-ap"], capture_output=True, text=True)
    check("built as the clean copy (no fixed password)", r.returncode == 0 and exe.exists(), (r.stdout + r.stderr)[-300:])
    fixed = next((ln.split('"')[1] for ln in (ROOT / "include" / "Config.h").read_text(encoding="utf-8").splitlines()
                  if ln.lstrip().startswith("#define AP_PASSWORD_DEFAULT")), "")
    c = Clock("ap_own_password", exe=exe)
    try:
        wait(synced, 90)
        lim = jget(HOME, "/api/limits")
        n_chars, letters = lim["apOwnPasswordChars"], set(lim["apOwnPasswordLetters"])
        wait(lambda: state().get("ap"), 60)
        pw = state().get("apPass", "")
        check("its own Wi-Fi: a password of its own, %d of its letters/digits, not the fixed one" % n_chars,
              len(pw) == n_chars and set(pw) <= letters and pw != fixed, pw)
        check("... Settings is told it", jget(HOME, "/api/config").get("apPassword") == pw)
        time.sleep(4)   # saved at the next quiet moment
        ctl("/power-cut")
        wait(synced, 90)
        wait(lambda: state().get("ap"), 60)
        check("... kept over a power cut", state().get("apPass") == pw, state().get("apPass"))
        http(HOME, "/api/factory-reset", form={"body": "{}"})
        check("factory reset -> setup mode", wait(in_setup_mode, 120, 2))
        new = state().get("apPass", "")
        check("... a new password, not the old one", len(new) == n_chars and set(new) <= letters and new != pw, new)
        check("... shown on the setup screen", wait(lambda: new in state().get("screen", ""), 60, 0.5), state().get("screen", "")[:200])
    finally:
        c.stop()


OLD_EXE = Path("C:/pio3/jc_clean/sim/versions/2.0.0-146-g08b7548/jcclock.exe")   # a version to update FROM
DIST = Path("C:/pio3/jc_clean/dist")


PACKAGE = None   # --package: this build's (run_all_tests passes the run's own) - else the newest in dist


def newest_package():
    """--package, else the last full run's own package (run_all_tests.py) - never dist's newest:
    that is an older release (2.1.2: not signed), and the update drills failed on it (2026-11-16)."""
    if PACKAGE:
        return Path(PACKAGE)
    found = sorted(Path("C:/pio3/jc_clean/sim/all_tests").glob("*_package/*.jcupdate"), key=lambda p: p.stat().st_mtime)
    if not found:
        sys.exit("no update package of this build - run sim/run_all_tests.py once, or give --package")
    return found[-1]


def upload(data, name="update.jcupdate", base=None, path="/update"):
    boundary = "----drill%d" % int(time.time() * 1000)
    body = (("--%s\r\nContent-Disposition: form-data; name=\"firmware\"; filename=\"%s\"\r\n"
             "Content-Type: application/octet-stream\r\n\r\n") % (boundary, name)).encode() + data + ("\r\n--%s--\r\n" % boundary).encode()
    return http(base or HOME, path, data=body, raw=True, timeout=120,
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
        # Signed packages only (2026-11-14, UpdateSignature.h): without the signature, with
        # it altered, or signed with another key - refused, nothing installed
        body = pkg[:pkg.rindex(b"JCSIG1")]
        sig_bad = bytearray(pkg); sig_bad[-3] ^= 0x01
        sys.path.insert(0, str(ROOT / "utils"))
        import update_key
        with tempfile.TemporaryDirectory() as d:
            other = Path(d) / "other.pem"
            subprocess.run([update_key.openssl(), "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(other)], check=True)
            osig = update_key.sign(body, other)
        other_key = body + b"JCSIG1" + struct.pack("<H", len(osig)) + osig
        fw = pkg[pkg.index(b"firmware.bin") + len(b"firmware.bin") + 8:]   # a plain firmware file (starts 0xE9)
        # a part of a kind the clock doesn't know (2026-11-18): the first entry's type byte (JCUPD1 | u8 vlen |
        # version | u16 count | u8 type ...) made 7, signed again with the real key - refused at that part
        first_type = 6 + 1 + pkg[6] + 2
        unk = bytearray(body); unk[first_type] = 7
        usig = update_key.sign(bytes(unk))
        unknown_part = bytes(unk) + b"JCSIG1" + struct.pack("<H", len(usig)) + usig
        # want: the clock's message for it (Texts.h) - None: refused, whatever it says
        for label, data, want in [("damaged", bytes(bad), "updDamagedFile"), ("cut off", pkg[:len(pkg) // 2], "updIncomplete"),
                                  ("not signed", body, "updNotSigned"), ("signature altered", bytes(sig_bad), "updBadSignature"),
                                  ("signed with another key", other_key, "updBadSignature"),
                                  ("plain firmware file", fw[:fw.index(b"JCEND!")], "updPlainFirmware"),
                                  ("not an update", b"\x89PNG\r\n\x1a\n" + os.urandom(5000), "updNotAnUpdate"),
                                  ("with a part of an unknown kind", unknown_part, "updUnknownEntry"),
                                  ("empty", b"", None)]:
            ms0 = state().get("millis", 0)
            code, msg = upload(data)
            text = msg.decode("utf-8", "replace")
            check("update %s: refused, says why (%s)" % (label, want or "-"), code != 200 and (want is None or W().has(want, text)),
                  "%s: %s" % (code, text[:110]))
            # No restart: its uptime keeps counting from before the upload, watched for 4 s
            # (a restart starts it over) - not one look after a fixed wait (2026-11-15).
            last = [ms0]
            def counting():
                ms = state().get("millis", 0)
                ok = ms >= last[0] and version() == before
                last[0] = ms
                return ok
            check("update %s: clock unchanged, no restart (watched 4 s)" % label, stays(counting, 4), "uptime %d -> %d ms" % (ms0, last[0]))
        check("update: no leftovers waiting (state ok)", jget(HOME, "/api/status").get("updateState") == "ok")
        # An upload that stalls halfway (2026-11-18): half the package sent, then nothing, the connection
        # left open - the clock gives up after UPLOAD_STALLED_MS (Updater.cpp), installs nothing
        host, port = HOME.rsplit("/", 1)[-1].rsplit(":", 1)
        boundary = "----stall%d" % int(time.time() * 1000)
        head = (("--%s\r\nContent-Disposition: form-data; name=\"firmware\"; filename=\"update.jcupdate\"\r\n"
                 "Content-Type: application/octet-stream\r\n\r\n") % boundary).encode()
        tail = ("\r\n--%s--\r\n" % boundary).encode()
        sock = socket.create_connection((host, int(port)), timeout=30)
        try:
            sock.sendall(("POST /update HTTP/1.1\r\nHost: %s\r\nContent-Type: multipart/form-data; boundary=%s\r\n"
                          "Content-Length: %d\r\n\r\n" % (host, boundary, len(head) + len(pkg) + len(tail))).encode())
            sock.sendall(head + pkg[:len(pkg) // 2])
            stalled_s = tunables()["UPLOAD_STALLED_MS"] / 1000
            check("an upload that stalls halfway: given up after %.0f s, said so" % stalled_s,
                  wait(lambda: W().has("updStalled", c.diag()), stalled_s + 30, 1))
            check("... nothing installed, no restart, nothing left waiting",
                  version() == before and jget(HOME, "/api/status").get("updateState") in ("ok", "failed"), jget(HOME, "/api/status").get("updateState"))
        finally:
            sock.close()
    finally:
        c.stop()


def drill_update_trial_fail():
    """New version can't get on Wi-Fi: after its trial deadline (TRIAL_CONNECT_DEADLINE_MS) the clock goes back by itself."""
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
        t_trial = time.time()
        deadline_s = tunables(AP)["TRIAL_CONNECT_DEADLINE_MS"] / 1000   # the version on trial's own (no Wi-Fi: its own)
        back = wait(lambda: (lambda s: s.get("version") == old and s)(state()), deadline_s + 4 * 60, 5)
        took = time.time() - t_trial
        check("trial fail: back to the old version by itself, at its deadline (%.0f s; took %.0f s)" % (deadline_s, took),
              back and deadline_s - 60 <= took <= deadline_s + 3 * 60, "%.0f s" % took)
        for n in w["networks"]:
            n["up"] = True
        ctl("/world", json.dumps(w).encode())
        check("trial fail: old version online again", wait(synced, 180) and version() == old, version())
        check("trial fail: logged", W().has("updTrialFailed", c.diag()))
    finally:
        c.stop()


def drill_update_failed_notice():
    """An update that goes back by itself is said (2026-11-14): this build installs 2.1.2 (its
    package signed here), 2.1.2 finds no Wi-Fi on trial and the clock comes back to this
    build - which then shows "Update to 2.1.2 failed" in red and says it in /api/status."""
    c = Clock("update_failed_notice")
    try:
        wait(synced, 90)
        mine = version()
        pkg = DIST / "2.1.2" / "JewishClock-update-2.1.2.jcupdate"
        data = pkg.read_bytes()
        body = data[:data.rindex(b"JCSIG1")] if b"JCSIG1" in data[-200:] else data   # 2.1.2's is unsigned: all of it
        sys.path.insert(0, str(ROOT / "utils"))
        import update_key
        sig = update_key.sign(body)
        older = body + b"JCSIG1" + struct.pack("<H", len(sig)) + sig
        code, msg = upload(older, pkg.name)
        check("an older version (2.1.2): refused, says why and how", code == 400 and W().has("updOlderVersion", msg.decode("utf-8", "replace")), msg.decode()[:160])
        code, msg = upload(older, pkg.name, path="/update?erase=1")
        check("failed notice: 2.1.2, signed here, accepted with erase", code == 200, msg.decode()[:80])
        wait(lambda: not up(), 30)
        w = default_world()
        for n in w["networks"]:
            n["up"] = False
        wait(lambda: ctl("/world", json.dumps(w).encode()) == b"ok", 60)
        # The trial is 2.1.2's own (its Updater.cpp at tag v2.1.2: 5 min to join + 60 s) - a release
        # never changes, and it has no /api/tunables to ask; 9 min = that + the rollback and a margin.
        back = wait(lambda: (lambda s: s.get("version") == mine and s)(state()), 9 * 60, 5)
        check("failed notice: back on this build by itself", back, "after the trial deadline")
        for n in w["networks"]:
            n["up"] = True
        ctl("/world", json.dumps(w).encode())
        check("failed notice: online again", wait(synced, 180))
        check("failed notice: /api/status says which update failed", jget(HOME, "/api/status").get("updateFailed") == "2.1.2",
              jget(HOME, "/api/status").get("updateFailed"))
        FAILED_LINE = stext("updateFailed", "2.1.2")   # the clock's words, asked (Texts.h)
        seen = wait(lambda: FAILED_LINE in (state().get("screen") or ""), 120, 1)
        check("failed notice: on the screen - 'Update to 2.1.2 failed'", seen, state().get("screen"))
        # At most UPDATE_FAILED_SCREEN_DAYS on the screen (from the day it first showed); Settings keeps it until "OK, seen"
        days = tunables()["UPDATE_FAILED_SCREEN_DAYS"]
        f = c.fs / "update_failed.txt"
        lines = f.read_text(encoding="utf-8").split(chr(10))
        f.write_text(lines[0] + chr(10) + str(int(lines[1]) - days) + chr(10), encoding="utf-8")
        # the next 5 rounds asked of the clock (its own round code), not watched (2026-11-18: up to 10 min)
        def next5():
            return [sl for r in json.loads(ctl("/state?rounds=5") or b"{}").get("nextRounds", []) for sl in r]
        gone = wait(lambda: (lambda sl: sl if sl and not any(FAILED_LINE in x for x in sl) else None)(next5()), 180, 1)
        check("failed notice: %d days later off the screen (the next 5 rounds without it)" % days, gone, next5()[:6])
        check("failed notice: ... still in Settings", jget(HOME, "/api/status").get("updateFailed") == "2.1.2")
        code, _ = http(HOME, "/api/update/failed-seen", form={"body": "{}"}, raw=True)
        check("failed notice: 'OK, seen' ends it", code == 200 and jget(HOME, "/api/status").get("updateFailed") == "" and not f.exists())
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


def fill_flash(fs):
    """A file taking every block the simulated LittleFS has left (sim_platform.cpp's count: 4 KB
    blocks, files up to 512 bytes inline, 2 blocks a folder and 4 for the file system)."""
    BLOCK, TOTAL = 4096, 4063232 // 4096
    used = 4
    for p in Path(fs).rglob("*"):
        if p.is_dir():
            used += 2
        elif p.is_file() and p.stat().st_size > 512:
            used += (p.stat().st_size + BLOCK - 1) // BLOCK
    free = TOTAL - used
    (Path(fs) / "filler.bin").write_bytes(b"\0" * (free * BLOCK))
    return free


def drill_flash_full():
    """The flash full to its last block (2026-11-18, from the coverage list): every save is refused
    before it begins - "the clock's storage is full" (the user: check first, then save), a settings
    change too (it used to say "Saved" and lose the change at the next restart) - the clock's events
    as its files hold them, nothing half-written; with room again, everything as before. (A save that
    fails while writing: FileTools, and the live list put back - tested here before the room check.)"""
    c = Clock("flash_full")
    try:
        wait(synced, 90)
        def names():
            return sorted(e["name"] for e in jget(HOME, "/api/events/list"))
        def bin_names():
            return sorted(e["name"] for e in jget(HOME, "/api/events/deleted"))
        for n in ("UIT Full A", "UIT Full B"):
            api_post("/api/events/upsert", {"name": n, "hasG": True, "gD": 1, "gM": 1, "gY": 0, "type": "other"})
        b = next(e for e in jget(HOME, "/api/events/list") if e["name"] == "UIT Full B")
        http(HOME, "/api/events/delete", form={"id": b["id"]})
        wait(lambda: (c.fs / "jewish_calendar_data.json").exists(), 120, 1)
        time.sleep(3)   # the last automatic writes (log, settings) done
        ev_before, bin_before, speed = names(), bin_names(), jget(HOME, "/api/config")["scrollSpeed"]
        c.stop()
        files_before = {p.name: p.read_bytes() for p in (c.fs / "config.json", c.fs / "events.json", c.fs / "deleted_events.json") if p.exists()}
        free = fill_flash(c.fs)
        check("flash filled to the last block", free > 0, "%d blocks were free" % free)
        c.start()
        check("full flash: the clock starts and runs", wait(synced, 120))
        a = next(e for e in jget(HOME, "/api/events/list") if e["name"] == "UIT Full A")

        def refused(what, code_said, words=None):   # checked before the save (refusedForRoom)
            code, said = code_said
            said = said.decode("utf-8", "replace")
            check("full flash, %s: refused (%d), says why" % (what, code), code == 507 and (W().has("storageFull", said) if words is None else words in said), (code, said[:120]))
            check("... the clock's events as its file has them", names() == ev_before and bin_names() == bin_before, (names(), bin_names()))
        refused("a new event", api_post("/api/events/upsert", {"name": "UIT Full C", "hasG": True, "gD": 2, "gM": 2, "type": "other"}))
        refused("an event changed", api_post("/api/events/upsert", dict(a, name="UIT Full A2")))
        refused("an event deleted", http(HOME, "/api/events/delete", form={"id": a["id"]}, raw=True))
        refused("an Excel import", api_post("/api/events/import", jget(HOME, "/api/events/list") + [{"name": "UIT Full D", "hasG": True, "gD": 3, "gM": 3}]))
        bin_b = next(e for e in jget(HOME, "/api/events/deleted") if e["name"] == "UIT Full B")
        refused("an event restored from the bin", http(HOME, "/api/events/restore", form={"id": bin_b["id"]}, raw=True))
        refused("a setting changed (the page shows the clock's answer)", api_post("/api/config", {"scrollSpeed": speed % 9 + 1}))
        check("... the setting not taken either", jget(HOME, "/api/config")["scrollSpeed"] == speed)
        refused("a place chosen", api_post("/api/location/place", {"slot": 0, "city": "Haifa", "country": "Israel", "tzid": "Asia/Jerusalem",
                                                                     "lat": 32.79, "lon": 34.99, "offsetSec": 7200}))
        check("... no half-written file left", not list(c.fs.rglob("*.tmp")), [str(p) for p in c.fs.rglob("*.tmp")])
        check("... the files as they were", all((c.fs / n).read_bytes() == v for n, v in files_before.items()))
        c.stop()
        (c.fs / "filler.bin").unlink()
        c.start()
        check("room again: the clock starts with its files as they were", wait(synced, 120) and names() == ev_before and bin_names() == bin_before
              and jget(HOME, "/api/config")["scrollSpeed"] == speed, (names(), jget(HOME, "/api/config")["scrollSpeed"], speed))
        code, _ = api_post("/api/events/upsert", {"name": "UIT Full C", "hasG": True, "gD": 2, "gM": 2, "type": "other"})
        check("... and saves again", code == 200 and "UIT Full C" in names())
    finally:
        c.stop()


def drill_events_many():
    """Many events at once (Excel import): accepted up to what the clock holds, refused cleanly beyond."""
    c = Clock("events_many")
    try:
        wait(synced, 90)
        most = jget(HOME, "/api/limits")["maxEvents"]
        for n in (300, most):
            evs = [{"name": "Drill %04d" % i, "type": "birthday", "hasG": True, "gD": 1 + i % 28, "gM": 1 + i % 12, "gY": 1950 + i % 60}
                   for i in range(n)]
            code, body = api_post("/api/events/import", evs)
            t = body.decode("utf-8", "replace")
            check("events: import %d" % n, code == 200 and '"saved":true' in t.replace(" ", ""), "%s %s" % (code, t[:120]))
            check("events: %d listed" % n, len(jget(HOME, "/api/events/list")) == n)
        # one more than it holds: refused, nothing changed (the docstring's "refused cleanly beyond" -
        # not tested until 2026-11-19)
        evs.append({"name": "Drill one too many", "type": "birthday", "hasG": True, "gD": 1, "gM": 1, "gY": 2000})
        code, body = api_post("/api/events/import", evs)
        said = body.decode("utf-8", "replace")
        check("events: %d (one more than it holds) refused, says why" % len(evs), code == 400 and W().has("eventsTooMany", said, most),
              (code, said[:160]))
        check("... the %d before still there" % most, len(jget(HOME, "/api/events/list")) == most)
        code, body = api_post("/api/events/check", evs)   # what the page asks before an import
        check("... the page's check says so too", code == 200 and W().has("eventsTooMany", body.decode("utf-8", "replace"), most),
              (code, body[:160]))
        ctl("/reset")
        wait(synced, 120)
        check("events: %d still there after a restart" % most, len(jget(HOME, "/api/events/list")) == most)
        # its own events file with one more (edited by hand, another version): kept as far as it goes,
        # said in Settings - never dropped silently (2026-11-20)
        c.stop()
        f = c.fs / "events.json"
        lst = json.loads(f.read_text(encoding="utf-8"))
        lst.append(dict(lst[-1], id="extra0001", name="Drill one too many"))
        f.write_text(json.dumps(lst, ensure_ascii=False), encoding="utf-8")
        c.start()
        wait(synced, 120)
        check("its own file with %d: the first %d kept" % (most + 1, most), len(jget(HOME, "/api/events/list")) == most)
        notes = json.dumps(jget(HOME, "/api/config").get("textNotes", []), ensure_ascii=False)
        check("... and said in Settings", W().has("eventsFileTooMany", notes, most + 1), notes[:200])
        check("events: the page still loads", b"<html" in http(HOME, "/").lower())
        st = state()
        log("     PSRAM used with %d events: %.1f MB" % (most, st.get("psramUsed", 0) / 1e6))
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


# ── Long outages and odd starts (2026-11-09) ──────────────────────────────────
HEB_YEAR = "תשפ"                    # in every Hebrew date slide of these years
PARASHA_ICON = "Torah.bmp"


def set_world(**changes):
    """The world as it is now, with: networks_up / internet (all networks), blocked (list)."""
    w = default_world()
    for n in w["networks"]:
        if "networks_up" in changes:
            n["up"] = changes["networks_up"]
        if "internet" in changes:
            n["internet"] = changes["internet"]
    if "blocked" in changes:
        w["blocked"] = changes["blocked"]
    ctl("/world", json.dumps(w).encode())


def coming(rounds=4):
    """Every slide of the clock's next `rounds` rounds, asked of the clock (/state?rounds - its own
    round code with its live settings, events and state; web_ui checks once a run that the screen
    then shows exactly that) - not watched in real time (2026-11-18: screens_for() watched 30-40 s
    a look, several minutes a drill)."""
    return [sl for r in json.loads(ctl("/state?rounds=%d" % rounds) or b"{}").get("nextRounds", []) for sl in r]


def drill_offline_scroll():
    """Scroll mode (one long ticker): all networks gone -> 'Internet unreachable' in the
    ticker, the page on the clock's own Wi-Fi, gone again once a network is back."""
    c = Clock("offline_scroll")
    try:
        wait(synced, 90)
        check("scroll: a Friday morning on the drill clock (its calendar in)", to_friday_morning())
        api_post("/api/config", {"rotationMode": 1})
        after_s = tunables()["OFFLINE_NOTICE_AFTER_MS"] / 1000   # asked before its home network goes
        UNREACH_HE = stext("noInternet")   # English in every language, Hebrew settings too (2026-11-14)
        set_world(networks_up=False)
        on = wait(lambda: UNREACH_HE in state().get("screen", ""), after_s + 60, 0.5)
        check("scroll, all networks gone: 'Internet unreachable' in the ticker within %.0f s" % (after_s + 60), on)
        s = state().get("screen", "")
        check("scroll, all networks gone: the rest of the ticker stays", HEB_YEAR in s and PARASHA_ICON in s, s[:160])
        code, page = http(AP, "/", raw=True)
        check("scroll, all networks gone: the page works on the clock's own Wi-Fi", code == 200 and b"<html" in page.lower(), "status %s" % code)
        set_world(networks_up=True)
        check("scroll, network back: rejoins by itself", wait(lambda: state().get("sta"), 200, 3))
        gone = wait(lambda: UNREACH_HE not in state().get("screen", ""), 90, 1)
        check("scroll, network back: the notice leaves the ticker", gone)
        api_post("/api/debug/time", {"offset": 0})
    finally:
        c.stop()


def drill_ntp_blocked_at_start():
    """Power on with NTP out of reach (port 123 blocked), the rest of the internet working:
    no time - and the clock must neither hang nor restart; time comes once NTP answers."""
    c = Clock("ntp_blocked_at_start")
    try:
        wait(lambda: state().get("sta"), 60)
        set_world(blocked=["ntp"])
        ctl("/power-cut")
        wait(lambda: state().get("sta"), 60)
        time.sleep(60)
        st = jget(HOME, "/api/status")
        check("NTP blocked at start: Wi-Fi up, no time after 60 s", not st.get("synced"), "synced=%s" % st.get("synced"))
        code, page = http(HOME, "/", raw=True)
        check("NTP blocked at start: the page says 'initializing' (no wrong date)", W().has("initializing", page.decode("utf-8", "replace")))
        s = state().get("screen", "")
        check("NTP blocked at start: the screen shows no date from 1970", "1970" not in s and HEB_YEAR not in s, s[:120])
        waiting = stext("waitingForTime")
        check("NTP blocked at start: the screen says it waits for the time (Wi-Fi is up)", waiting in s, (waiting, s[:120]))
        # The log hears of it after NO_REPLY_LOG_AFTER lost replies - asked, not assumed to happen
        # within the 60 s (2026-11-22, numbers run: at 4 with three servers, the 4th question came only
        # after their rest, past 60 s - never logged, the drill failed). Each lost reply can take up to
        # the longest rest + the reply's wait.
        t = tunables()
        longest = t["SILENT_REST_MAX_MS"] / 1000   # silent servers rest, doubling up to this (RFC 4330 s10)
        per_loss = max(longest, t["SAME_SERVER_MIN_MS"] / 1000, t["PROBE_MIN_GAP_MS"] / 1000) + t["REPLY_TIMEOUT_MS"] / 1000
        logged = wait(lambda: W().has("logFailing", c.diag(), W().text("ntpReplyStreak")),
                      t["NO_REPLY_LOG_AFTER"] * per_loss + 30, 2)
        check("NTP blocked at start: logged once as failing (after %d lost replies)" % t["NO_REPLY_LOG_AFTER"],
              logged and W().count("logFailing", c.diag(), W().text("ntpReplyStreak")) == 1)
        boots = c.log_text().count("[sim] JewishClock ")
        set_world(blocked=[])
        ok = wait(synced, longest + 30)
        check("NTP back: time syncs by itself within the clock's longest rest (%.0f s)" % longest, ok)
        check("NTP back: logged as working again", wait(lambda: W().has("logWorksAgain", c.diag(), W().text("ntpReplyStreak")), 30))
        check("NTP blocked at start: no restart meanwhile", c.log_text().count("[sim] JewishClock ") == boots, "reset lines %d -> %d" % (boots, c.log_text().count("[sim] JewishClock ")))
    finally:
        c.stop()


def drill_ntp_next_server():
    """Power on with the first two time servers out of reach (2026-11-13): the clock moves
    on to the third by itself and has the time - no waiting for the first to come back."""
    c = Clock("ntp_next_server")
    try:
        wait(lambda: state().get("sta"), 60)
        servers = jget(HOME, "/api/config")["ntpServers"]
        check("time servers: three in the settings (an old one-server list got the defaults)",
              len(servers) == 3 and all(servers), servers)
        set_world(blocked=["ntp:" + servers[0], "ntp:" + servers[1]])
        ctl("/power-cut")
        wait(lambda: state().get("sta"), 60)
        ok = wait(synced, 60)
        check("first two time servers silent: time from the third within a minute", ok)
        check("... it is the one in use", jget(HOME, "/api/status").get("ntpServer") == servers[2], jget(HOME, "/api/status").get("ntpServer"))
        log = c.log_text().rsplit("[sim] JewishClock ", 1)[-1]
        check("... the log says why it moved on", W().has("ntpNextServer", log, None, None, servers[1]) and W().has("ntpNextServer", log, None, None, servers[2]))
    finally:
        set_world(blocked=[])
        c.stop()


def hebcal_tries(c, start=None):
    """When the clock asked Hebcal for its calendar since its last start: seconds (log time).
    start (a date): only its asks for the calendar from that day on - the asks a debug-time jump
    made, told apart by what was asked, not by a count taken before it (see drill_calendar_runs_out)."""
    import re
    log = c.log_text().rsplit("[sim] JewishClock ", 1)[-1]
    rx = re.compile(r"\[(\d\d):(\d\d):(\d\d\.\d+)\] \[Calendar\] " + W().pattern("calFetching").pattern)   # the clock's words
    return [int(h) * 3600 + int(m) * 60 + float(s) for h, m, s, url in rx.findall(log)
            if url.startswith("https://www.hebcal.com") and (start is None or "start=%s&" % start.isoformat() in url)]


def drill_hebcal_down_at_start():
    """Power on with no saved calendar and Hebcal down (NTP fine): the clock shows the
    dates it computes itself, logs one line, and gets the calendar once Hebcal is back."""
    c = Clock("hebcal_down_at_start")
    try:
        wait(synced, 90)
        # its calendar downloaded and saved first: nothing of it still on the way when Hebcal goes
        # down and the file goes (else the cut raced the clock's first request - 2026-11-22)
        check("before the cut: the calendar downloaded and saved",
              wait(lambda: any(c.fs.glob("jewish_calendar*.json")), 90))   # (*.json: not a half-written .tmp)
        set_world(blocked=["hebcal.com"])
        for f in c.fs.glob("jewish_calendar*"):
            f.unlink()
        boots = c.log_text().count("[sim] JewishClock ")
        ctl("/power-cut")
        wait(synced, 90)
        # hebcal_tries() reads only since the last start - so the cut must have been one
        # (2026-11-22: a pre-cut count n0 sliced off this start's FIRST try whenever the clock had
        # asked Hebcal before the cut - a race - and the drill saw 3 tries, gaps [25, 45]. The
        # [5, 15, 24] it was meant to fix was the clock's own lost-result race of 2026-11-13,
        # fixed 2026-11-14 in Calendar.cpp startAsyncFetch().)
        check("Hebcal down: the power cut started the clock anew",
              c.log_text().count("[sim] JewishClock ") == boots + 1, "start lines %d -> %d" % (boots, c.log_text().count("[sim] JewishClock ")))
        time.sleep(30)
        seen = coming()
        heb = any(HEB_YEAR in s for s in seen)
        check("Hebcal down: the Hebrew date shows (computed by the clock)", heb, " / ".join(s[:40] for s in seen[:6]))
        check("Hebcal down: no parasha (it comes only from Hebcal)", not any(PARASHA_ICON in s for s in seen))
        # Four failed tries since the power cut: "logged once, not per try", and their spacing
        # (2026-11-11: 11 min fixed wait before - and the tries came every 5 s, unnoticed).
        # Try to try = the 5 s connect timeout + the wait (Calendar.cpp retryWaitMs: RETRY_FIRST_MS,
        # doubling up to RETRY_MAX_MS) - the whole schedule over hours: test_retry.py, on simulated time.
        T = tunables()
        first_s, max_s = T["RETRY_FIRST_MS"] / 1000, T["RETRY_MAX_MS"] / 1000
        waits = [min(first_s * 2 ** k, max_s) for k in range(3)]
        wait(lambda: len(hebcal_tries(c)) >= 4, sum(waits) + 4 * 15 + 60, 5)
        t = hebcal_tries(c)   # this start's tries only
        n = W().count("logFailing", c.diag(), W().text("hebcalStreak"))
        check("Hebcal down, %d tries: logged once, not per try" % len(t), len(t) >= 4 and n == 1, "%d lines" % n)
        gaps = [b - a for a, b in zip(t, t[1:])][:3]
        check("Hebcal down: waits growing %s s (Calendar.cpp retryWaitMs)" % [round(w) for w in waits],
              len(gaps) == 3 and all(w + 4 <= g <= w + 12 for g, w in zip(gaps, waits)), "gaps %s s" % [round(g) for g in gaps])
        set_world(blocked=[])
        back = wait(lambda: W().has("logWorksAgain", c.diag(), W().text("hebcalStreak")), max_s + 60, 5)
        check("Hebcal back: the calendar comes within the retry wait (at most %.0f s)" % max_s, back)
        check("Hebcal back: the parasha shows", wait(lambda: any(PARASHA_ICON in s for s in coming()), 120, 1))
    finally:
        c.stop()


def drill_calendar_runs_out():
    """Hebcal down for longer than the saved calendar lasts (21 days) - the date jumped 25
    days ahead: the clock keeps showing the right Hebrew date, no parasha, no restart;
    once Hebcal is back, that day's calendar comes."""
    import datetime
    c = Clock("calendar_runs_out")
    try:
        wait(synced, 90)
        check("runs out: a Friday morning (its calendar in)", to_friday_morning())
        boots = c.log_text().count("[sim] JewishClock ")
        set_world(blocked=["hebcal.com"])
        T = tunables()
        warn = max([e.get("warn", 0) for e in jget(HOME, "/api/events/list")] + [T["CALENDAR_DAYS_AHEAD_MIN"]])
        span = warn + T["CALENDAR_DAYS_MARGIN"]   # what the saved calendar reaches (Calendar.cpp)
        friday = datetime.date.today()
        friday += datetime.timedelta(days=(4 - friday.weekday()) % 7 or 7)
        later = friday + datetime.timedelta(days=7 * (span // 7 + 1))   # whole weeks on: a Friday past the saved span
        api_post("/api/debug/time", {"local": later.isoformat() + "T10:00"})
        time.sleep(20)
        st = jget(HOME, "/api/status")
        seen = coming()
        check("runs out: a Hebrew date on screen", any(HEB_YEAR in s for s in seen), " / ".join(s[:40] for s in seen[:6]))
        day = str(later.day)
        check("runs out: the Gregorian date is that day's", any(day in s and str(later.year) in s for s in seen))
        check("runs out: the clock's Hebrew day is known (computed)", st.get("todayHDay"), "todayHDay=%s %s" % (st.get("todayHDay"), st.get("todayHMonth")))
        check("runs out: no parasha without its calendar", not any(PARASHA_ICON in s for s in seen))
        # the try at the jump, the next after the 5-minute pause since the last success, then
        # the failure waits (Calendar.cpp retryWaitMs) - before 2026-11-11 the third came 5 s later
        # only the asks for that later day's calendar (2026-11-22: a count taken before the jump raced
        # the clock's own ask for to_friday_morning()'s Friday, made ~3 s after that debug time was set -
        # counted as this jump's, gaps [14, 281])
        wait(lambda: len(hebcal_tries(c, later)) >= 3, T["MAIN_FETCH_COOLDOWN_MS"] / 1000 + 150, 5)
        t = hebcal_tries(c, later)
        gaps = [b - a for a, b in zip(t, t[1:])]
        check("runs out, Hebcal down: tries waited for, not one after another",
              len(t) >= 3 and min(gaps) >= T["RETRY_FIRST_MS"] / 1000 + 4, "gaps %s s" % [round(g) for g in gaps])
        set_world(blocked=[])
        got = wait(lambda: any(PARASHA_ICON in s for s in coming()), 7 * 60, 1)
        check("runs out, Hebcal back: that Friday's parasha comes within the retry", got)
        # the whole window comes, not just that day: every day from the jump's day to its end (what the
        # clock asks for: the longer of CALENDAR_DAYS_AHEAD_MIN and an event's warning days, plus
        # CALENDAR_DAYS_MARGIN - `span`, all from the clock) is in the calendar file the clock saves
        # (written on a still screen, at the latest after FLASH_WRITE_MAX_WAIT_MS)
        def window_saved():
            try:
                days = {d.get("g") for d in json.loads((c.fs / "jewish_calendar_data.json").read_text(encoding="utf-8"))}
            except (OSError, ValueError):
                return None
            want = {(later + datetime.timedelta(days=k)).isoformat() for k in range(span + 1)}
            return sorted(want - days) or True
        missing = wait(lambda: window_saved() is True, T["FLASH_WRITE_MAX_WAIT_MS"] / 1000 + 60, 2)
        check("runs out, Hebcal back: the whole window is in, not only that day", missing, window_saved())
        check("runs out: no restart meanwhile", c.log_text().count("[sim] JewishClock ") == boots)
        api_post("/api/debug/time", {"offset": 0})
    finally:
        c.stop()


def drill_power_cut_router_down():
    """A power cut while the router is off too (the house's power): the clock starts with no
    time and no network - the page on its own Wi-Fi, and everything back by itself
    once the router is."""
    c = Clock("power_cut_router_down")
    try:
        wait(synced, 90)
        set_world(networks_up=False)
        ctl("/power-cut")
        time.sleep(60)
        st = state()
        check("router down + power cut: not joined, its own Wi-Fi on", not st.get("sta") and st.get("ap"), "sta=%s ap=%s" % (st.get("sta"), st.get("ap")))
        code, page = http(AP, "/", raw=True)
        check("router down + power cut: a page answers on its own Wi-Fi", code == 200, "status %s" % code)
        s = st.get("screen", "")
        check("router down + power cut: the screen says something (no blank, no 1970)", s and "1970" not in s, s[:120])
        set_world(networks_up=True)
        back = wait(lambda: state().get("sta"), 240, 3)
        check("router back: joins by itself", back)
        check("router back: time syncs", wait(synced, 90))
        seen = wait(lambda: (lambda x: x if any(HEB_YEAR in t for t in x) else None)(coming()), 90, 2) or coming()   # the calendar a moment after the time
        check("router back: the normal slides", any(HEB_YEAR in s for s in seen), " / ".join(s[:40] for s in seen[:5]))
    finally:
        c.stop()


class FakeTimeServers:
    """Time servers on this PC for the NTP drills (2026-11-17): 127.0.0.1-3 (IPS), more by fresh(), one port
    (the simulator sends 127.x port 123 there - Sim::localNtpPort). Each answers as `how[ip]` says: the PC's
    time plus `off` ms, its travel times, and whatever wrong a reply can carry. `asked[ip]` = when each
    question came (PC time)."""
    IPS = ("127.0.0.1", "127.0.0.2", "127.0.0.3")

    def __init__(self):
        self.socks, self.how, self.asked, self.default = {}, {}, {}, {}
        self.port = 0
        self.next_ip = 1
        self.stop_ = False
        self.fresh(3)

    def fresh(self, n):
        """n more servers never asked before (127.0.0.4, 5, ...), answering as set() last said."""
        ips = []
        for _ in range(n):
            ip = "127.0.0.%d" % self.next_ip
            self.next_ip += 1
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.bind((ip, self.port))
            self.port = s.getsockname()[1]
            self.socks[ip] = s
            self.how[ip] = dict(self.default)
            self.asked[ip] = []
            threading.Thread(target=self._serve, args=(ip,), daemon=True).start()
            ips.append(ip)
        return ips

    def set(self, **how):
        """The same behaviour on all of them (and the ones fresh() makes next)."""
        self.default = dict(how)
        for ip in list(self.how):
            self.how[ip] = dict(how)

    @staticmethod
    def _ts(t):
        """PC time (s) -> NTP timestamp bytes."""
        sec = int(t) + 2208988800
        return struct.pack(">II", sec & 0xFFFFFFFF, int((t - int(t)) * 2 ** 32) & 0xFFFFFFFF)

    def _serve(self, ip):
        s = self.socks[ip]
        s.settimeout(0.5)
        while not self.stop_:
            try:
                q, addr = s.recvfrom(512)
                t_in = time.time()   # stamped at once: a slow thread start isn't the network's time
            except socket.timeout:
                continue
            except OSError:
                return
            self.asked[ip].append(time.time())
            h = self.how[ip]
            if h.get("silent"):
                continue
            threading.Thread(target=self._answer, args=(ip, q, addr, dict(h), t_in), daemon=True).start()

    def _answer(self, ip, q, addr, h, t_in):
        off = h.get("off", 0) / 1000.0
        arrive = t_in + h.get("up_ms", 0) / 1000.0           # the question's (simulated) travel time
        time.sleep(max(0.0, arrive - time.time()))
        t2 = arrive + off - h.get("claim_held_ms", 0) / 1000.0   # claims it held the question longer than it did
        time.sleep(h.get("hold_ms", 0) / 1000.0)             # the server's own time on it
        t3 = time.time() + off
        li, vn, mode, stratum = h.get("li", 0), h.get("vn", 4), h.get("mode", 4), h.get("stratum", 2)
        ref_id = h.get("kiss", "GPS\0").encode("ascii")[:4].ljust(4, b"\0")
        root_disp = int(h.get("disp_ms", 1) / 1000.0 * 65536)
        origin = q[40:48]
        if h.get("origin") == "zero":
            origin = b"\0" * 8
        elif h.get("origin") == "wrong":
            origin = bytes((b ^ 0x55) for b in origin)
        p = bytes([(li << 6) | (vn << 3) | mode, stratum, h.get("poll", 6) & 0xFF, h.get("prec", -20) & 0xFF]) + struct.pack(">iI", 0, root_disp) + ref_id
        p += self._ts(t3 - 1) + origin + self._ts(t2) + (b"\0" * 8 if h.get("no_time") else self._ts(t3))
        time.sleep(h.get("down_ms", 0) / 1000.0)             # the answer's travel time
        sender = self.socks["127.0.0.2" if h.get("from_other") and ip != "127.0.0.2" else ip]
        sender.sendto(p, addr)
        if h.get("twice"):
            sender.sendto(p, addr)

    def close(self):
        self.stop_ = True
        for s in self.socks.values():
            s.close()


def drill_ntp():
    """The clock's time sync against time servers that lie, stall and misbehave (2026-11-17, after
    RFC 4330/5905/8633): left alone / corrected gradually (never backwards) / a jump only when a
    second reply agrees; every kind of unusable reply ignored; travel time taken off; a
    kiss-o'-death obeyed; a server never asked sooner than SAME_SERVER_MIN_MS; a failing one marked not
    recommended (and cleared after its good days); a jump over midnight and back followed by the date; a
    first time far from the last seen one confirmed."""
    # Measured to the millisecond while a full run loads the PC (2026-11-18: ~130 ms off with six drills
    # at once - replies read late by a slow loop): this drill's clock and its time servers above normal.
    if os.name == "nt":
        import ctypes
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x8000)   # ABOVE_NORMAL
    srv = FakeTimeServers()
    c = Clock("ntp", cfg={"ntpServers": list(FakeTimeServers.IPS), "ntpServerCount": 3}, priority="above")
    pc_ms = lambda: time.time() * 1000

    def err(off):
        """The clock's time minus the true time (the PC's plus `off`), ms - against the PC's time
        halfway through the request (a slow request read as a clock error of up to 40 ms)."""
        best = None
        for _ in range(5):
            t0 = pc_ms()
            st = state()
            t1 = pc_ms()
            if not st.get("utcMs"):
                return None
            e = st["utcMs"] - ((t0 + t1) / 2 + off)
            if best is None or t1 - t0 < best[0]:
                best = (t1 - t0, e)
            if t1 - t0 < 20:
                break
        last_u[0] = best[0] / 2   # this reading's own uncertainty: half its request's round trip
        return best[1]

    last_u = [0.0]

    def moved_from(e0, u0, allow):
        """True while the clock is still where e0 said (reading uncertainties u0 and this one's
        allowed - 2026-11-20: a flat 15 ms failed on one noisy reading of an unmoved clock)."""
        e = err(0)
        return e is not None and abs(e - e0) < allow + u0 + last_u[0]

    def sync():
        ctl("/ntp-sync")

    def tol(base, trip=0):
        """How far the clock may be off: `base` (the drill's reading of it), or - when larger - half
        the round trip the clock measured plus 15 ms: its own error bound (RFC 5905). The clock reads
        a reply when its loop gets to it; on a loaded PC that was 124 ms late once - 62 ms off,
        within its bound (2026-11-20). /api/status ntpRoundTripMs. trip: the travel time the fake servers
        add on purpose - only what the round trip has beyond it counts (else 0.9 s would allow 0.45)."""
        try:
            rt = jget(HOME, "/api/status").get("ntpRoundTripMs", -1)
        except Exception:
            rt = -1
        return max(base, (rt - trip) / 2 + 15) if rt and rt > 0 else base

    def unmoved(e0, secs, since_s=0, u0=None):
        """The clock didn't take the reply: no step from where it was (e0 = err(0) taken before the
        sync), watched for `secs`. It may still be finishing a gradual correction - at most SLEW ms a
        second since e0 was taken (since_s before the watch) - plus 15 ms of reading it; a jump is far
        beyond that. Not "within N ms of the truth": the clock leaves an offset under IGNORE alone
        on purpose (2026-11-20: 37 ms off, right, failed that)."""
        allow = SLEW * (since_s + secs + 1) + 5
        u0 = last_u[0] if u0 is None else u0
        return stays(lambda: moved_from(e0, u0, allow), secs, 0.5)

    def settle(off, within, timeout=40, every=None, trip=0):
        """After a sync: the clock within `within` ms (or its own bound, tol()) of the time `off`;
        asks again every `every` s."""
        def ok():
            if every:
                sync()
            e = err(off)
            return e is not None and abs(e) <= tol(within, trip)
        return wait(ok, timeout, every or 0.5)

    def diag():
        return c.diag()

    try:
        point_ntp_at(srv)
        # The clock's own numbers (/api/tunables), never copies (the user, 2026-11-19)
        T = tunables()
        IGNORE, STEP, SLEW = T["IGNORE_THRESHOLD_MS"], T["STEP_THRESHOLD_MS"], T["MAX_SLEW_MS_PER_SEC"]
        REPLY_TIMEOUT, SAME_MIN_S, FAILED_S = T["REPLY_TIMEOUT_MS"], T["SAME_SERVER_MIN_MS"] / 1000, T["FAILED_RETRY_MS"] / 1000

        def tolerance(prec):
            """How far below zero a round trip may come out for a server stating `prec` (NTPManager checkReply)."""
            prec = min(prec, T["COARSEST_SERVER_PREC"])
            return T["OWN_ROUNDING_MS"] + 2 * ((1000 >> -prec) if prec >= -10 else 0)
        TOL_MAX = tolerance(0)
        srv.set(off=0)
        check("NTP: the first time from the test servers", wait(lambda: abs(err(0) or 1e9) < tol(30), 90, 0.5), err(0))
        check("NTP: logged as the first time", W().has("ntpStep", diag(), W().text("ntpFirst")))
        # the regular wait drawn anew, 5.5-6 h - never the same beat (the NTP Pool's rule; the user, 2026-11-21)
        waits = [int(v[0]) for v in W().values("ntpNextRegular", c.log_text())]
        lo, hi = (T["SYNC_INTERVAL_MS"] - T["SYNC_JITTER_MS"]) / 1000, T["SYNC_INTERVAL_MS"] / 1000
        check("NTP: the next regular question %.1f-%.1f h after a sync, drawn each time" % (lo / 3600, hi / 3600),
              bool(waits) and all(lo <= w <= hi for w in waits), waits[:5])

        def use_fresh(first=None, second=None):
            """Three time servers never asked before as the clock's list - its best asked at once (2026-11-21:
            the clock asks no server twice within SAME_SERVER_MIN_MS and rests and marks one that failed; each
            part on servers of its own, not waiting out the last part's). `first`/`second`: how those answer."""
            ips = srv.fresh(3)
            if first is not None:
                srv.how[ips[0]] = dict(first)
            if second is not None:
                srv.how[ips[1]] = dict(second)
            api_post("/api/config", {"ntpServers": ips})
            return ips

        def asked_since(ip, t0):
            return [t for t in srv.asked[ip] if t >= t0]

        # ── left alone, corrected gradually (never backwards), both ways ──
        # From where the clock IS (within its own bound - not exactly on the servers' time, 2026-11-20):
        # "left" = it doesn't move; "gradually" = as long as the offset it measured takes at its rate.
        small = int(IGNORE * 0.6)
        e0, n_left = err(0), W().count("ntpLeft", c.log_text())
        u0 = last_u[0]
        # from where the clock IS: it may sit up to IGNORE off, left alone (2026-11-21, a loaded run: 31 ms
        # behind, the servers +30 -> 61 ms apart, rightly corrected - "small" must be small for the clock)
        srv.set(off=int(round(e0 or 0)) + small)
        use_fresh()
        check("NTP +%d ms (below %d): left as it is - the clock doesn't move" % (small, IGNORE),
              wait(lambda: W().count("ntpLeft", c.log_text()) > n_left, 10, 0.25) and stays(lambda: moved_from(e0, u0, 5), 4, 0.25),
              (e0, err(0)))
        # +200 ms, then back to 0 - which IS the -200 ms case (2026-11-18: 4 slews of 80 s were the
        # longest part of the longest drill; 2 of 40 s check the same)
        mid = min(4 * IGNORE, STEP // 2)   # between "left" and "a jump"
        for off, label in ((mid, +mid), (0, -mid)):
            n_grad = W().count("ntpGradual", c.log_text())
            srv.set(off=off)
            use_fresh()
            wait(lambda: W().count("ntpGradual", c.log_text()) > n_grad, 10, 0.1)
            said = [v[0] for v in W().values("ntpGradual", c.log_text())]   # the offset the clock measured
            d_ms = int(said[-1]) if said else off   # the offset the clock measured
            want_s = abs(d_ms) / SLEW
            # watch its rate: the clock minus the PC's time moves while it corrects, stands still after
            seen, back, t0 = [], False, time.time()
            while time.time() < t0 + want_s * 1.5 + 10:
                st = state()
                now = time.time()
                seen.append((now, st.get("utcMs", 0), st.get("utcMs", 0) - now * 1000))
                if len(seen) > 1 and seen[-1][1] < seen[-2][1]:
                    back = True
                # done: no change of more than 4 ms over the last 2 s (reading it jitters a little)
                last = [x for x in seen if x[0] >= now - 2]
                if now - t0 > 3 and len(last) >= 6 and max(x[2] for x in last) - min(x[2] for x in last) < 4:
                    break
                time.sleep(0.25)
            done_at = next((x[0] for x in seen if max(y[2] for y in seen if y[0] >= x[0]) - min(y[2] for y in seen if y[0] >= x[0]) < 4), seen[-1][0])
            took = done_at - t0
            moved = seen[-1][2] - seen[0][2]
            check("NTP %+d ms (the clock measured %+d): corrected gradually in %.0f s (%d ms a second: %.0f s), moved %+.0f ms"
                  % (label, d_ms, took, SLEW, want_s, moved),
                  abs(moved - d_ms) < 20 and abs(took - want_s) <= max(3, want_s * 0.25), (d_ms, moved, took))
            check("NTP %+d ms: the clock never went backwards meanwhile" % label, not back)

        # ── jumps: only when a second reply (the next server) agrees ──
        for off, name in ((5 * STEP, "+%d ms" % (5 * STEP)), (-5 * STEP, "-%d ms" % (5 * STEP)), (3600 * 1000, "+1 hour"),
                          (-86400 * 1000, "-1 day"), (10 * 365 * 86400 * 1000, "+10 years")):
            srv.set(off=off)
            ips = use_fresh()
            check("NTP %s: a jump after a second reply agreed" % name, settle(off, 30, 30), err(off))
            check("... the second reply from the next server - the first not asked again within %.0f s" % SAME_MIN_S,
                  len(srv.asked[ips[0]]) == 1 and srv.asked[ips[1]], (len(srv.asked[ips[0]]), len(srv.asked[ips[1]])))
            srv.set(off=0)
            use_fresh()
            settle(0, 30, 30)
        check("NTP: jumps logged as confirmed", W().count("ntpStep", diag(), W().text("ntpClockSet")) >= 10)

        # ── one server wrong: the next one says otherwise -> no jump ──
        srv.set(off=0)
        e0 = err(0)
        u0 = last_u[0]
        use_fresh(first={"off": 3600 * 1000})
        check("NTP: one server an hour off, the next one right: no jump", unmoved(e0, 12, 0, u0), (e0, err(0)))
        check("... said so in the log", wait(lambda: W().has("ntpJumpNotConfirmed", diag()), 10, 0.5))
        # two servers far off, but not the same: no jump
        e0 = err(0)
        u0 = last_u[0]
        use_fresh(first={"off": 3600 * 1000}, second={"off": 7200 * 1000})
        check("NTP: two servers far off but disagreeing (1 h / 2 h): no jump", unmoved(e0, 12, 0, u0), (e0, err(0)))
        check("... said so in the log", wait(lambda: W().has("logFailing", diag(), W().text("ntpAgreeStreak")), 10, 0.5))
        # ... and asks again by itself soon - not after its 6-hour interval (2026-11-21: after two replies
        # disagreed, the next ask waited SYNC_INTERVAL_MS; a needed jump with it)
        n_sent = W().count("ntpSent", c.log_text())
        check("... and asks again by itself within its failed-ask wait (%.0f s), not after %.0f h" % (FAILED_S, T["SYNC_INTERVAL_MS"] / 3600000),
              wait(lambda: W().count("ntpSent", c.log_text()) > n_sent, FAILED_S + 30, 1))

        # ── the list changed while a jump waits for its second opinion (2026-11-27, a full run under load:
        #    the first opinion's server was kept as a slot number - the new list's first server, left out) ──
        srv.set(off=0)
        use_fresh()
        settle(0, 30, 30)
        a_ips = use_fresh(first={"off": 3600 * 1000}, second={"silent": True})
        t_b = time.time()
        b_asked = wait(lambda: asked_since(a_ips[1], t_b), 30, 0.1)   # the second opinion asked (it stays silent)
        srv.set(off=3600 * 1000)
        new = use_fresh()
        t_new = time.time()
        check("NTP: the list changed while a jump waited for its second opinion: the new list's first server asked",
              b_asked and wait(lambda: asked_since(new[0], t_new), 20, 0.25), [len(asked_since(ip, t_new)) for ip in new])
        check("... and the jump made (its answer agreed with the first opinion)", settle(3600 * 1000, 30, 30), err(3600 * 1000))
        srv.set(off=0)
        use_fresh()
        settle(0, 30, 30)

        # ── replies that must not be used (all servers +5 s, but each reply wrong) ──
        bad = [("its clock not set (LI=3)", {"li": 3}), ("stratum 16", {"stratum": 16}),
               # (kiss-o'-death RATE / DENY: their own part below - the server then rests)
               ("mode 3 (a client's)", {"mode": 3}), ("version 7", {"vn": 7}), ("no time in it", {"no_time": True}),
               ("a wrong origin (not our question)", {"origin": "wrong"}), ("a zero origin", {"origin": "zero"}),
               ("unsure of its time (dispersion twice the most allowed)", {"disp_ms": 2 * T["MAX_ROOT_DISTANCE_MS"]}),
               ("from another address", {"from_other": True}),
               ("late (after the clock gave up)", {"down_ms": REPLY_TIMEOUT + 600}),
               # Round trips below zero (2026-11-19): the server claims it held the question longer than the trip took.
               # The simulator's own delay only adds to a trip, so a claim of CLAIM gives a trip of -CLAIM + that delay:
               # beyond a precise server's tolerance, within the coarsest one's (next part) - the same claim both ways.
               ("holding time past the trip (a precise server, 1 us steps)", {"claim_held_ms": TOL_MAX - 2 * T["OWN_ROUNDING_MS"], "prec": -20}),
               # a forged precision (1 s steps) is taken as the coarsest allowed: well past that - refused
               ("holding time far past the trip (precision forged as 1 s)", {"claim_held_ms": TOL_MAX + 200, "prec": 0})]

        for what, h in bad:
            srv.set(off=5 * STEP, **h)
            srv.how["127.0.0.2"] = {"silent": True}   # the "other address" of from_other (never in a fresh list)
            n_trip = W().count("ntpImpossibleTrip", c.log_text())
            n_marked = W().count("ntpMarked", c.diag())
            e0, t_e0 = err(0), time.time()
            u0 = last_u[0]
            ips = use_fresh()
            # each failing server marked and the next asked at once - all three within a few replies' waits
            # (and the internet checks for the silent ones)
            all3 = wait(lambda: all(asked_since(ip, t_e0) for ip in ips) and W().count("ntpMarked", c.diag()) >= n_marked + 3, 60, 0.5)
            check("NTP reply %s: not used" % what, unmoved(e0, 3, time.time() - t_e0, u0), (e0, err(0)))
            check("... each of the three servers asked once, at once after the last one failed, and marked not recommended",
                  all3 and all(len(asked_since(ip, t_e0)) == 1 for ip in ips),
                  ([len(asked_since(ip, t_e0)) for ip in ips], W().count("ntpMarked", c.diag()) - n_marked))
            if "claim_held_ms" in h:   # refused for its round trip, not by chance for something else
                check("... refused as an impossible travel time", W().count("ntpImpossibleTrip", c.log_text()) > n_trip)
        # the same claim from servers stating the coarsest steps the clock allows: within their rounding - used
        srv.set(off=2 * STEP, claim_held_ms=TOL_MAX - 2 * T["OWN_ROUNDING_MS"], prec=T["COARSEST_SERVER_PREC"])
        use_fresh()
        check("NTP holding time %d ms past the trip, servers with the coarsest steps (tolerance %d ms): used (a jump)"
              % (TOL_MAX - 2 * T["OWN_ROUNDING_MS"], TOL_MAX), settle(2 * STEP, 50, 30), err(2 * STEP))
        srv.set(off=0)
        use_fresh()
        settle(0, 50, 30)
        srv.set(off=0, twice=True)
        e0 = err(0)
        u0 = last_u[0]
        use_fresh()
        check("NTP reply sent twice: the clock fine (the second dropped)", unmoved(e0, 5, 0, u0), (e0, err(0)))
        lt = c.log_text()
        check("... the dropped replies said why (live output)", W().has("ntpNotAnswer", lt) and W().has("ntpClockUnset", lt)
              and W().has("ntpNotServerReply", lt) and W().has("ntpDroppedPacket", lt))

        # ── travel time: 400 ms each way, the server 100 ms on it -> taken off ──
        way, hold = REPLY_TIMEOUT // 5, REPLY_TIMEOUT // 20   # a long round trip, still well inside the clock's wait
        srv.set(off=0, up_ms=way, hold_ms=hold, down_ms=way)
        use_fresh()
        check("NTP slow network (%.1f s round trip): the time right within 50 ms" % ((2 * way + hold) / 1000),
              stays(lambda: abs(err(0)) < tol(50, 2 * way), 8, 0.5), err(0))
        srv.set(off=2 * STEP, up_ms=way, hold_ms=hold, down_ms=way)
        use_fresh()
        check("NTP slow network: a jump lands right (travel time off)", settle(2 * STEP, 50, 30, trip=2 * way), err(2 * STEP))
        srv.set(off=0)
        use_fresh()
        settle(0, 30, 30)

        # ── a jump over midnight and back: the date follows ──
        before = jget(HOME, "/api/status")
        srv.set(off=86400 * 1000)
        use_fresh()
        settle(86400 * 1000, 50, 30)
        tomorrow = (datetime.date.fromisoformat(before["date"]) + datetime.timedelta(days=1)).isoformat()
        after = wait(lambda: (lambda s: s if s["date"] == tomorrow and s["hebrewDate"] != before["hebrewDate"] else None)(jget(HOME, "/api/status")), 20, 0.5) or {}
        check("NTP +1 day: the date is tomorrow's, the Hebrew date changed", after.get("date") == tomorrow and after.get("hebrewDate") != before["hebrewDate"],
              (before.get("date"), after.get("date")))
        srv.set(off=0)
        use_fresh()
        settle(0, 50, 30)
        back = wait(lambda: (lambda s: s if s["date"] == before["date"] and s["hebrewDate"] == before["hebrewDate"] else None)(jget(HOME, "/api/status")), 20, 0.5) or {}
        check("NTP back a day: today's dates again", back.get("date") == before["date"] and back.get("hebrewDate") == before["hebrewDate"],
              (back.get("date"), back.get("hebrewDate")))

        # ── a kiss-o'-death from the server asked (2026-11-21, the user: "read the reply and act upon it always"):
        # not used, logged; THAT server rests - RATE: the longer of KOD_RATE_WAIT_MS and the kiss's own poll field
        # (2^9 = 512 s here: longer, so taken; RFC 5905 s7.4, the reference implementation), DENY: for good
        # (RFC 5905: "stop sending packets to that server") - and is marked not recommended; the next asked at once
        rate_s = max(T["KOD_RATE_WAIT_MS"] // 1000, 2 ** 9)
        for code, poll, rest_s in (("RATE", 9, rate_s), ("DENY", 6, None)):
            srv.set(off=0)
            e0, u0, tk = err(0), last_u[0], time.time()
            ips = use_fresh(first={"stratum": 0, "kiss": code, "poll": poll})
            cur = ips[0]
            kissed = wait(lambda: asked_since(cur, tk), 20, 0.25)
            nxt = wait(lambda: asked_since(ips[1], tk), 20, 0.25)
            t1 = time.time()
            for _ in range(4):   # asked again and again meanwhile: the others answer, the kissing one rests
                sync()
                time.sleep(5)
            check("NTP kiss-o'-death %s from %s: not used, logged" % (code, cur),
                  bool(kissed) and unmoved(e0, 1, time.time() - tk, u0) and W().has("ntpKissLog", diag(), cur, code), (e0, err(0)))
            said = W().has("ntpRest", diag(), cur, rest_s) if rest_s else W().has("ntpDenied", diag(), cur, code)
            check("... %s %s (logged), not asked during the watch; the next one asked at once"
                  % (cur, "rests %d s (the kiss's own %d s)" % (rest_s, 2 ** poll) if rest_s else "never asked again"),
                  said and nxt and nxt[0] - kissed[0] < 5 and not asked_since(cur, t1 + 0.5),
                  (len(asked_since(cur, t1)), nxt[:1], kissed[:1]))
            tr = {x["server"]: x for x in jget(HOME, "/api/status").get("ntpTrouble", [])}
            check("... marked not recommended (/api/status ntpTrouble: %s)" % tr.get(cur, {}).get("why"),
                  cur in tr and W().has("ntpKiss", tr[cur]["why"], code) and tr[cur]["goodDays"] == 0, tr)
        srv.set(off=0)   # the kisses over: the next parts' servers answer

        # ── synced, the only server silent (the internet there): checked, marked, asked again after its rest ──
        one = srv.fresh(1)[0]
        tl = time.time()
        api_post("/api/config", {"ntpServers": [one, "", ""]})
        check("NTP synced, the server list changed: the new list asked at once (its address looked up first)",
              wait(lambda: asked_since(one, tl), 10, 0.25), srv.asked[one][-1:])
        srv.how[one] = {"silent": True}
        n_check = W().count("ntpNetCheck", c.log_text())
        t0 = time.time()
        sync()   # not before SAME_SERVER_MIN_MS since the list change's question
        wait(lambda: len(asked_since(one, t0)) >= 2, 3 * SAME_MIN_S + 30, 0.5)
        asked = asked_since(one, t0)
        gaps = [b - a for a, b in zip([srv.asked[one][0]] + asked, asked)]
        check("NTP synced, the only server silent: the internet checked first, then the server marked (%s)" % W().text("ntpWhySilent"),
              W().count("ntpNetCheck", c.log_text()) > n_check and W().has("ntpMarked", diag(), one, W().text("ntpWhySilent")))
        check("... asked again only after %.0f s each time (its rest after a failure; the forced sync waited too)" % SAME_MIN_S,
              len(asked) >= 2 and all(g >= SAME_MIN_S - 0.5 for g in gaps) and gaps[-1] <= SAME_MIN_S + REPLY_TIMEOUT / 1000 + 15,
              ["%.1f" % g for g in gaps])
        srv.how[one] = {}

        # ── restarted (unsynced), one server: never asked sooner than SAME_SERVER_MIN_MS; less often while silent ──
        c.stop()
        f = c.fs / "config.json"
        cfg = json.loads(f.read_text(encoding="utf-8"))
        cfg["ntpServers"] = ["127.0.0.1", "", ""]
        f.write_text(json.dumps(cfg, ensure_ascii=False), encoding="utf-8")
        srv.set(off=0, silent=True)
        t0 = time.time()
        c.start()
        point_ntp_at(srv)
        # silent with the internet there: a rest of SAME_SERVER_MIN_MS, doubling while it stays silent (RFC 4330 s10 -
        # 2026-11-21: the test clocks' asking every 16 s for hours got the home address rate-limited)
        wait(lambda: len(asked_since("127.0.0.1", t0)) >= 3, (1 + 2) * SAME_MIN_S + 60, 0.5)
        asked = asked_since("127.0.0.1", t0)
        gaps = [b - a for a, b in zip(asked, asked[1:])]
        check("NTP unsynced, the only server silent: asked again, never sooner than %.0f s (RFC 4330 s5)" % SAME_MIN_S,
              len(asked) >= 3 and all(g >= SAME_MIN_S - 0.5 for g in gaps), ["%.1f" % g for g in gaps])
        check("... and less often while it stays silent - its rest doubling",
              len(gaps) >= 2 and gaps[1] >= 1.8 * gaps[0] - 2, ["%.1f" % g for g in gaps])
        check("... its rest logged", W().has("ntpRest", diag(), "127.0.0.1"))
        c.stop()   # rests are the running clock's: a restart starts clean for the RATE case
        c.start()
        point_ntp_at(srv)
        srv.set(off=0, stratum=0, kiss="RATE")
        n = len(srv.asked["127.0.0.1"])
        wait(lambda: len(srv.asked["127.0.0.1"]) > n, 20, 0.25)
        t1 = time.time()
        assert 20 < T["KOD_RATE_WAIT_MS"] / 1000, "the drill watches 20 s of the RATE wait"
        for _ in range(4):
            sync()   # even asked to: the wait holds
            time.sleep(5)
        check("NTP the only server: kiss-o'-death RATE - not asked again for its wait", not asked_since("127.0.0.1", t1 + 0.5),
              len(asked_since("127.0.0.1", t1)))

        # ── restarted with the saved "latest time seen" FIRST_SYNC_FAR_DAYS + 100 days ahead (wrong): the real time -
        # not 136 years on (the old era rule read it as 2162) - and only once a second reply agrees; the saved time
        # follows. And two servers not recommended in the file, one good day short: the one whose day comes (asked
        # for the second opinion, the last good day 21 h ago) is recommended again; the other stays, as the file says ──
        c.stop()
        far_days = T["FIRST_SYNC_FAR_DAYS"] + 100
        (c.fs / "time_seen.txt").write_text(str(int(time.time() + far_days * 86400)) + chr(10), encoding="utf-8")
        cfg = json.loads(f.read_text(encoding="utf-8"))
        cfg["ntpServers"] = list(FakeTimeServers.IPS)
        f.write_text(json.dumps(cfg, ensure_ascii=False), encoding="utf-8")
        need = T["TROUBLE_GOOD_DAYS"]
        day_ago = int(time.time() - T["TROUBLE_CHECK_MIN_MS"] / 1000 - 3600)
        week_ago = int(time.time() - 8 * 86400)
        (c.fs / "ntp_trouble.json").write_text(json.dumps({"servers": [
            {"name": ip, "why": "a drill's", "since": week_ago, "good": need - 1, "lastGood": day_ago}
            for ip in FakeTimeServers.IPS[1:]]}), encoding="utf-8")
        srv.set(off=0)
        c.start()
        point_ntp_at(srv)
        check("NTP first time %d days before the saved 'latest seen': the real time, set after a second reply agreed" % far_days,
              wait(lambda: abs(err(0) or 1e9) < tol(30), 90, 0.5) and wait(lambda: W().has("ntpStep", diag(), W().text("ntpFirstConfirmed")), 10, 0.5), err(0))
        check("... the saved time follows it", wait(lambda: abs(int((c.fs / "time_seen.txt").read_text().split()[0]) - time.time()) < 3600, 60, 1),
              (c.fs / "time_seen.txt").read_text().strip())
        two, three = FakeTimeServers.IPS[1], FakeTimeServers.IPS[2]
        tr = {x["server"]: x for x in jget(HOME, "/api/status").get("ntpTrouble", [])}
        check("not recommended, %d good days: %s (asked for the second opinion) recommended again, logged; %s still marked (%d of %d)"
              % (need, two, three, need - 1, need),
              W().has("ntpTroubleCleared", diag(), two, need) and two not in tr and tr.get(three, {}).get("goodDays") == need - 1
              and tr.get(three, {}).get("needDays") == need, tr)
        check("... the file says so too (written on a still screen)",
              wait(lambda: [s["name"] for s in json.loads((c.fs / "ntp_trouble.json").read_text(encoding="utf-8"))["servers"]] == [three], 30, 1),
              (c.fs / "ntp_trouble.json").read_text(encoding="utf-8"))
    finally:
        srv.close()
        c.stop()

def drill_ntp_outages():
    """Outages that start and end at real-life moments (the user, 2026-11-21): a time server silent while the
    internet works -> the internet checked first, then that server marked not recommended and the next asked at
    once, and from then on asked last; the whole internet gone in the middle of a question -> nobody blamed, the
    internet watched; back at an odd moment between two of its checks -> the time at once."""
    srv = FakeTimeServers()
    c = Clock("ntp_outages", cfg={"ntpServers": list(FakeTimeServers.IPS)})
    try:
        point_ntp_at(srv)
        T = tunables()
        reply_s, same_s = T["REPLY_TIMEOUT_MS"] / 1000, T["SAME_SERVER_MIN_MS"] / 1000
        gap = T["PROBE_MIN_GAP_MS"] / 1000
        srv.set(off=0)
        check("outages: the first time", wait(synced, 90))

        # 1. one server silent, the internet working
        ips = srv.fresh(3)
        srv.how[ips[0]] = {"silent": True}
        n_check, t0 = W().count("ntpNetCheck", c.log_text()), time.time()
        api_post("/api/config", {"ntpServers": ips})
        nxt = wait(lambda: [t for t in srv.asked[ips[1]] if t >= t0], 30, 0.25)
        first = [t for t in srv.asked[ips[0]] if t >= t0]
        check("a server silent, the internet up: the internet checked first (logged)",
              W().count("ntpNetCheck", c.log_text()) > n_check and W().has("ntpNetCheck", c.log_text(), ips[0], W().text("ntpWhySilent")))
        check("... then the server marked not recommended, and the next asked at once (%.1f s after the silent one; its wait %.0f s)"
              % ((nxt[0] - first[0]) if nxt and first else -1, reply_s),
              bool(nxt and first) and nxt[0] - first[0] <= reply_s + 10 and W().has("ntpMarked", c.diag(), ips[0], W().text("ntpWhySilent")))
        tr = {x["server"]: x for x in jget(HOME, "/api/status").get("ntpTrouble", [])}
        check("... /api/status lists it (why, since today, 0 of %d good days)" % T["TROUBLE_GOOD_DAYS"],
              ips[0] in tr and tr[ips[0]]["why"] == W().text("ntpWhySilent") and abs(tr[ips[0]]["since"] - time.time()) < 120
              and tr[ips[0]]["goodDays"] == 0, tr)
        srv.how[ips[0]] = {}   # answering again - still asked last
        # 2. the next questions go to the recommended ones - the marked one only if none of them may be asked
        t1 = time.time()
        for _ in range(2):
            ctl("/ntp-sync")
            wait(lambda: len([t for ip in ips[1:] for t in srv.asked[ip] if t >= t1]) >= 2, same_s + 20, 0.5)
        check("... the next questions to the recommended servers, not to the marked one",
              len([t for ip in ips[1:] for t in srv.asked[ip] if t >= t1]) >= 2 and not [t for t in srv.asked[ips[0]] if t >= t1],
              {ip: len([t for t in srv.asked[ip] if t >= t1]) for ip in ips})

        # 3. the whole internet gone while a question is on its way (the answer 1.5 s late, the cut in between)
        srv.set(off=0, up_ms=1500)
        n_marked, n_rest = W().count("ntpMarked", c.diag()), W().count("ntpRest", c.diag())
        t2 = time.time()
        ctl("/ntp-sync")
        asked = wait(lambda: [(t, ip) for ip in ips for t in srv.asked[ip] if t >= t2], same_s + 20, 0.05)
        set_world(internet=False)
        cut = time.time()
        server = asked[0][1] if asked else None
        check("internet cut %.2f s after a question to %s left (its answer due 1.5 s after)" % ((cut - asked[0][0]) if asked else -1, server),
              bool(asked) and cut - asked[0][0] < 1.5)
        down = wait(lambda: W().has("ntpNotBlamed", c.diag(), server), reply_s + 30, 0.5)
        check("... the question unanswered, the internet checked, found gone: the server not blamed (logged)", down)
        check("... not marked, not rested", W().count("ntpMarked", c.diag()) == n_marked and W().count("ntpRest", c.diag()) == n_rest)
        # 4. back at an odd moment between two internet checks (each at most every PROBE_MIN_GAP_MS): the time
        # asked at once "as if nothing happened" - from the best server that may be asked
        srv.set(off=0)
        time.sleep(gap * 0.4)
        n_after = {ip: len(srv.asked[ip]) for ip in ips}
        set_world(internet=True)
        tb = time.time()
        firsts = lambda: sorted(srv.asked[ip][n_after[ip]] for ip in ips if len(srv.asked[ip]) > n_after[ip])
        again = wait(firsts, gap + 20, 0.25)
        took = (firsts()[0] - tb) if again else -1
        check("internet back: the time asked at once - %.0f s after (the internet checked at most every %.0f s)" % (took, gap),
              bool(again) and took <= gap + 10, took)
        check("... logged", W().has("ntpInternetBack", c.diag()))
        check("... and the clock's time right", wait(lambda: synced() and abs(jget(HOME, "/api/status")["utcTime"] - time.time()) < 2, 20, 0.5))
    finally:
        set_world(internet=True)
        srv.close()
        c.stop()


def drill_country_pool():
    """NTP pool substitution by country code (NTPManager.cpp s_countryPool, 2026-11-21):
    pool.ntp.org -> cc.pool.ntp.org when a network's place has a country code.
    Two cases: (1) the common restart - last-used place unchanged, pool must still be set;
    (2) a network switch to a place with a different country code."""

    base_cfg = json.loads((ROOT / "Data" / "config.json").read_text(encoding="utf-8"))
    places = [dict(p) for p in base_cfg.get("places", [{}, {}, {}])]
    # Inject country codes: slot 0 = "il", slot 1 = "de" so the switch is visible in the log.
    # Places' city/lat/timezone stay as-is so the test works with any real config.
    cc_by_slot = ["il", "de", ""]
    for i, cc in enumerate(cc_by_slot):
        if i < len(places):
            places[i]["countryCode"] = cc

    # Case 1: start with the country code already in the saved config.
    # Covers the common restart: the saved location matches the joined network's place
    # (activate() used to return early without calling setCountryPool - 2026-11-21 fix).
    c = Clock("country_pool", cfg={"places": places})
    try:
        wait(synced, 90)
        log0 = c.log_text()

        # Which network is it on? -> which slot -> which country code was set.
        here = state().get("ssid")
        ssids = [base_cfg.get("wifiSSID", ""), base_cfg.get("wifiSSID2", ""), base_cfg.get("wifiSSID3", "")]
        here_idx = ssids.index(here) if here in ssids else 0
        here_cc = cc_by_slot[here_idx]

        check("country pool set on startup even when saved location matches (%s.pool.ntp.org)" % here_cc,
              bool(here_cc) and W().has("ntpCountryPool", log0, here_cc))
        check("NTP synced with country pool on startup", synced())

        # Case 2: switch to the other network; the different country code must update the pool.
        other_idx = 1 - here_idx   # toggle between slot 0 and slot 1
        other_ssid = ssids[other_idx]
        other_cc = cc_by_slot[other_idx]

        if other_ssid:
            w = default_world()
            for n in w["networks"]:
                n["up"] = n["ssid"] == other_ssid
            ctl("/world", json.dumps(w).encode())
            wait(lambda: state().get("ssid") == other_ssid and state().get("sta"), 120)
            changed = wait(lambda: W().has("ntpCountryPool", c.log_text(), other_cc), 60)
            check("network switch: country pool updated (%s.pool.ntp.org)" % other_cc, changed)
            check("NTP still syncing on switched pool", wait(synced, 90))
        else:
            check("network switch: (only one network available, skipped)", True)
    finally:
        c.stop()


def drill_ip_api_hold():
    """ip-api.com answers with its quota used up (X-Rl: 0, X-Ttl: 30 s): the clock
    waits before retrying - no hammering during the hold."""
    c = Clock("ip_api_hold")
    try:
        wait(synced, 90)
        set_world(blocked=["ratelimit:ip-api.com"])
        http(HOME, "/api/location/detect")  # trigger a geo lookup; the quota reply holds ip-api.com
        held = wait(lambda: W().has("svcWait", c.diag(), "ip-api.com"), 30, 1)
        check("ip-api quota used up: hold set (svcWait logged for ip-api.com)", held, c.diag()[-500:])
        n0 = W().count("svcWait", c.diag())   # 1 ratelimit response; none during the hold
        n_looked = W().count("placeLookedUp", c.diag())
        set_world(blocked=[])
        # hold: X-Ttl 30 s -> held for 31 s (Calendar.cpp: X-Ttl + 1); the failed lookup is asked again after
        # the clock's own LOOKUP_RETRY_MS (never a copy of it here)
        success = wait(lambda: W().count("placeLookedUp", c.diag()) > n_looked, tunables()["LOOKUP_RETRY_MS"] / 1000 + 90, 2)
        check("ip-api hold expired: geo retry succeeds after the hold", success)
        check("... no second ratelimit hit (hold prevented hammering during those 31 s)",
              W().count("svcWait", c.diag()) == n0, W().count("svcWait", c.diag()))
    finally:
        c.stop()


DRILLS = {f.__name__[6:]: f for f in [drill_boot_ntp, drill_rtc_memory, drill_wifi_failover, drill_wifi_all_down,
                                       drill_setup_then_back, drill_wrong_password, drill_no_internet, drill_settings_power_cut, drill_light,
                                       drill_update_bad_files, drill_update_power_cut_in_trial, drill_update_trial_fail, drill_update_failed_notice,
                                       drill_pictures_fill, drill_events_many, drill_complaint_repro,
                                       drill_settings_roundtrip, drill_day_change, drill_place_follows_network,
                                       drill_log_rotation, drill_corrupt_files, drill_roaming, drill_setup_portal_save,
                                       drill_offline_scroll, drill_ntp_blocked_at_start, drill_ntp_next_server, drill_hebcal_down_at_start,
                                       drill_calendar_runs_out, drill_power_cut_router_down, drill_ap_own_password, drill_ntp, drill_ntp_outages,
                                       drill_service_garbage, drill_flash_full, drill_no_address,
                                       drill_low_memory, drill_weak_signal, drill_country_pool,
                                       drill_ip_api_hold]}


def last_durations():
    """Each drill's seconds in the newest earlier run (durations.json) - to start the long ones first."""
    for d in sorted(OUT.parent.glob("*/durations.json"), reverse=True):
        try:
            return json.loads(d.read_text(encoding="utf-8"))
        except ValueError:
            pass
    return {}


def run_side_by_side(names, jobs, a):
    """Each drill as its own process with its own clock on its own ports, `jobs` at a time,
    the longest first; their lines merged into this run's summary in the drills' order."""
    import re
    took = last_durations()
    # longest first (the run's length), each time with other neighbours: the last run's time
    # +-40 % at random (the run's seed, 2026-11-20) - drills meant to be independent, tested so
    rnd = test_random("resilience order")
    todo = sorted(names, key=lambda n: -took.get(n, 60) * rnd.uniform(0.6, 1.4))
    log("start order: %s" % " ".join(todo))
    logs = OUT / "_logs"
    logs.mkdir(exist_ok=True)
    free, running, done = list(range(jobs)), {}, {}
    log("== %d drills, %d at a time ==" % (len(names), jobs))
    while todo or running:
        while todo and free:
            n, slot = todo.pop(0), free.pop(0)
            cmd = [sys.executable, str(Path(__file__).resolve()), "--only", n, "--port-offset", str(a.port_offset + SLOT_PORTS * slot),
                   "--staged", str(STAGED), "--out", str(OUT)] + (["--exe", a.exe] if a.exe else []) + (["--package", a.package] if a.package else [])
            out = open(logs / (n + ".out"), "wb")
            running[n] = (subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT), slot, time.time(), out)
        time.sleep(1)
        for n, (p, slot, t0, out) in list(running.items()):
            if p.poll() is None:
                continue
            out.close()
            del running[n]
            free.append(slot)
            done[n] = time.time() - t0
            f = logs / (n + ".txt")
            lines = f.read_text(encoding="utf-8").splitlines() if f.exists() else []
            if not any(re.match(r"\d\d:\d\d:\d\d (PASS|FAIL) ", l) for l in lines):
                lines.append(time.strftime("%H:%M:%S ") + "FAIL %s: drill ran (exit %s, see %s)" % (n, p.returncode, logs / (n + ".out")))
            print("\n".join(lines), flush=True)
            for l in lines:
                m = re.match(r"\d\d:\d\d:\d\d (PASS|FAIL) (.*)", l)
                if m:
                    results.append((m.group(2), m.group(1) == "PASS", ""))
            log("   %s: %.0f s" % (n, done[n]))
    (OUT / "durations.json").write_text(json.dumps({n: round(s) for n, s in done.items()}, indent=1), encoding="utf-8")
    with open(OUT / "summary.txt", "a", encoding="utf-8") as f:   # each drill's lines together, in the drills' order
        for n in names:
            p = logs / (n + ".txt")
            f.write(p.read_text(encoding="utf-8") if p.exists() else "== %s == no lines\n" % n)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ensure_relay()   # the test clocks' own time server (run alone; under run_all_tests: its)
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--exe", help="another build (e.g. C:/pio3/jc_clean/sim/versions/<version>/jcclock.exe) - before/after a fix")
    ap.add_argument("--package", help="the update package of this build (default: the newest in C:/pio3/jc_clean/dist - may be an older one)")
    ap.add_argument("--port-offset", type=int, default=0, help="shifts the drills' ports (sim_ports.py) - beside another run")
    ap.add_argument("--jobs", type=int, default=1, help="drills side by side, each with its own clock and ports")
    ap.add_argument("--staged", help=argparse.SUPPRESS)   # set by --jobs for its drill processes
    ap.add_argument("--out", help=argparse.SUPPRESS)
    a = ap.parse_args()
    global HOME, AP, CTL, OUT, SUMMARY, STAGED
    if a.port_offset:
        HOME, AP, CTL = urls("resilience", a.port_offset)
    if a.package:
        global PACKAGE
        PACKAGE = a.package
    if a.exe:
        global EXE
        EXE = Path(a.exe)
        Clock.__init__.__defaults__ = (EXE, None)
    if a.list:
        print("\n".join(DRILLS))
        return
    unknown = [n for n in a.only or [] if n not in DRILLS]
    if unknown:
        sys.exit("no such drill: %s (--list)" % ", ".join(unknown))
    names = [n for n in DRILLS if not a.only or n in a.only]
    if a.out:   # one drill of a --jobs run
        OUT, STAGED, SUMMARY = Path(a.out), Path(a.staged), Path(a.out) / "_logs" / (names[0] + ".txt")
    else:
        OUT.mkdir(parents=True, exist_ok=True)
        sys.path.insert(0, str(ROOT / "utils"))
        import web_files
        STAGED = OUT / "_staged"
        web_files.stage(ROOT / "Data", STAGED)
    if a.jobs > 1 and len(names) > 1:
        run_side_by_side(names, a.jobs, a)
        bad = [r for r in results if not r[1]]
        log("== %d checks, %d failed ==" % (len(results), len(bad)))
        sys.exit(1 if bad else 0)
    for name, fn in DRILLS.items():
        if a.only and name not in a.only:
            continue
        log("== %s ==" % name)
        try:
            fn()   # each drill stops its own clock (Clock.stop) - other simulators keep running
        except Exception as e:
            check(name + ": drill ran", False, "%s: %s" % (type(e).__name__, e))
        time.sleep(1)
    bad = [r for r in results if not r[1]]
    log("== %d checks, %d failed ==" % (len(results), len(bad)))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
