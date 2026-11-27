"""
A simulated clock's own Wi-Fi as a REAL Wi-Fi (the user, 2026-11-12/13): phones and
other computers join it and use the clock's page as at the parents'.

    C:/pio3/jc_clean/venv/Scripts/python sim/hotspot.py family|dev     (start_clock.py --hotspot does this)
    C:/pio3/jc_clean/venv/Scripts/python sim/hotspot.py stop
    C:/pio3/jc_clean/venv/Scripts/python sim/hotspot.py status

Windows' Mobile Hotspot (sim/hotspot.ps1) follows the simulated clock: on, with the
clock's Wi-Fi name and password, while the clock's own Wi-Fi is up (its control port
/state "ap"); off while the clock is down or restarting - phones lose it then, as with
the real clock. This laptop stays on its home Wi-Fi meanwhile. Only one hotspot exists
per laptop, so one simulated clock at a time.

Phones get addresses from Windows (192.168.137.x by default; 192.168.4.x after the
one-time admin step, sim/hotspot_admin_setup.ps1); the laptop is .1 there. Requests to
it are passed to the clock's own-Wi-Fi port (8071 / 8081) with the address the clock
expects (Host 192.168.4.1), so any build works - the family's old ones too. Port 80
needs IIS off (the admin step); until then the page is at <address>:<own-Wi-Fi port>.
Differences from the real clock: Windows shares this laptop's internet on it, so phones
see internet there and don't pop the page up by themselves - open the address.
The hotspot's own name/password (Settings > Mobile hotspot) are put back at stop.
If this script dies without stopping (killed, window closed), a separate watcher it
starts ("guard", not in its process tree) stops the hotspot and puts them back; noted in
C:/pio3/jc_clean/sim/hotspot/guard.log. A clock that crashes or is killed: the hotspot goes off
within a second (its /state stops answering) and on again when it's back.
"""
import json
import os
import socket
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "sim"))
from sim_ports import ports  # noqa: E402

DIR = Path("C:/pio3/jc_clean/sim/hotspot")
SAVED = DIR / "saved_config.json"   # the laptop's own hotspot name/password, put back at stop
STOP = DIR / "stop"
PIDFILE = DIR / "pid.json"
CLOCK_AP = "192.168.4.1"            # the address the clock's own Wi-Fi has (WiFi.softAPIP())

# === TUNABLES ===
POLL_S = 1.0                         # how often the clock's state is read
CHECK_S = 10.0                       # how often Windows' hotspot state is checked (it switches it off by itself)


def ps(*args):
    r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(ROOT / "sim" / "hotspot.ps1")] + list(args),
                       capture_output=True, text=True, timeout=120)
    try:
        return json.loads(r.stdout.strip().splitlines()[-1])
    except Exception:
        return {"ok": False, "error": (r.stdout + r.stderr).strip()[-300:]}


def hotspot_addresses():
    """Where Windows' hotspot may be: what the admin step set (ScopeAddress /
    StandaloneDhcpAddress), else Windows' default 192.168.137.1. The forwarder takes
    whichever exists - Windows 11 doesn't always honour the setting (seen 2026-11-13)."""
    found = []
    r = subprocess.run(["reg", "query", r"HKLM\SYSTEM\CurrentControlSet\Services\SharedAccess\Parameters"],
                       capture_output=True, text=True)
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[0] in ("ScopeAddress", "StandaloneDhcpAddress") and parts[1] == "REG_SZ":
            found.append(parts[2])
    found.append("192.168.137.1")
    return list(dict.fromkeys(found))


def get_json(url):
    return json.loads(urllib.request.urlopen(url, timeout=2).read())


def log(msg):
    print(time.strftime("%H:%M:%S ") + msg, flush=True)
    try:
        with open(DIR / "hotspot.log", "a", encoding="utf-8") as f:   # its window closes with it
            f.write(time.strftime("%Y-%m-%d %H:%M:%S ") + msg + "\n")
    except OSError:
        pass


class Forwarder:
    """Phone -> laptop's hotspot address -> the clock's own-Wi-Fi port. One request per
    connection (the clock closes after each answer). Host made the clock's own address;
    a redirect to it is pointed back at the hotspot address."""

    def __init__(self, ips, port, ap_port):
        self.ips, self.port, self.ap_port = ips, port, ap_port
        self.public = None

    def where(self, ip):
        return ip if self.port == 80 else "%s:%d" % (ip, self.port)

    def run(self):
        while not STOP.exists():
            ls = None
            for ip in self.ips:   # an address exists only while the hotspot is on
                s = socket.socket()
                try:
                    s.bind((ip, self.port))
                    s.listen(16)
                    s.settimeout(1)
                    ls, self.public = s, self.where(ip)
                    break
                except OSError:
                    s.close()
            if ls is None:
                time.sleep(1)
                continue
            log("page for phones: http://%s" % self.public)
            while not STOP.exists():
                try:
                    c, _ = ls.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break   # the hotspot went off
                threading.Thread(target=self.one, args=(c,), daemon=True).start()
            ls.close()

    def one(self, c):
        try:
            c.settimeout(30)
            head = b""
            while b"\r\n\r\n" not in head:
                d = c.recv(65536)
                if not d:
                    return
                head += d
            h, rest = head.split(b"\r\n\r\n", 1)
            lines = [l if not l.lower().startswith(b"host:") else b"Host: " + CLOCK_AP.encode() for l in h.split(b"\r\n")]
            up = socket.create_connection(("127.0.0.1", self.ap_port), timeout=30)
            up.sendall(b"\r\n".join(lines) + b"\r\n\r\n" + rest)

            def pump_up():   # the rest of a request body (uploads)
                try:
                    while True:
                        d = c.recv(65536)
                        if not d:
                            break
                        up.sendall(d)
                except OSError:
                    pass
            threading.Thread(target=pump_up, daemon=True).start()
            resp = b""
            while b"\r\n\r\n" not in resp:
                d = up.recv(65536)
                if not d:
                    break
                resp += d
            if b"\r\n\r\n" in resp:
                rh, rb = resp.split(b"\r\n\r\n", 1)
                rh = rh.replace(b"Location: http://" + CLOCK_AP.encode(), b"Location: http://" + self.public.encode())
                resp = rh + b"\r\n\r\n" + rb
            c.sendall(resp)
            while True:
                d = up.recv(65536)
                if not d:
                    break
                c.sendall(d)
            up.close()
        except OSError:
            pass
        finally:
            c.close()


def alive(pid):
    import ctypes
    h = ctypes.windll.kernel32.OpenProcess(0x00100000, False, pid)   # SYNCHRONIZE
    if not h:
        return False
    try:
        return ctypes.windll.kernel32.WaitForSingleObject(h, 0) == 0x102   # WAIT_TIMEOUT: still running
    finally:
        ctypes.windll.kernel32.CloseHandle(h)


def running_pid():
    try:
        return json.loads(PIDFILE.read_text())["pid"]
    except Exception:
        return None


def start_guard():
    """A separate watcher that switches the hotspot off if this process dies without
    doing it itself (killed, window closed, crash). Started through a short-lived middle
    process so it isn't in this process's tree - 'taskkill /T' on this one spares it."""
    flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
    subprocess.Popen([sys.executable, str(Path(__file__)), "spawnguard", str(os.getpid())],
                     creationflags=flags, close_fds=True).wait(timeout=30)


def spawn_guard(pid):
    flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
    subprocess.Popen([sys.executable, str(Path(__file__)), "guard", str(pid)], creationflags=flags, close_fds=True)


def guard(pid):
    import ctypes
    h = ctypes.windll.kernel32.OpenProcess(0x00100000, False, pid)
    if h:
        ctypes.windll.kernel32.WaitForSingleObject(h, 0xFFFFFFFF)   # until it ends, however
        ctypes.windll.kernel32.CloseHandle(h)
    if running_pid() == pid:   # it ended without finish(): clean up after it
        with open(DIR / "guard.log", "a", encoding="utf-8") as f:
            f.write(time.strftime("%Y-%m-%d %H:%M:%S ") + "hotspot.py (pid %d) ended without stopping the hotspot - stopped it\n" % pid)
        finish()


def stop_running():
    pid = running_pid()
    if pid is None:
        if SAVED.exists():   # left over (e.g. the laptop restarted meanwhile): put the laptop's own settings back
            finish()
        return False
    if not alive(pid):
        finish()
        return True
    STOP.parent.mkdir(parents=True, exist_ok=True)
    STOP.write_text("stop")
    for _ in range(60):
        if not PIDFILE.exists():
            break
        time.sleep(1)
    else:   # it didn't end by itself
        try:
            pid = json.loads(PIDFILE.read_text())["pid"]
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)], capture_output=True)
        except Exception:
            pass
        finish()
    return True


def finish():
    """Hotspot off and the laptop's own hotspot name/password back."""
    ps("stop")
    if SAVED.exists():
        s = json.loads(SAVED.read_text(encoding="utf-8"))
        if s.get("ssid"):
            ps("configure", s["ssid"], s["pass"])
        SAVED.unlink()
    PIDFILE.unlink(missing_ok=True)
    STOP.unlink(missing_ok=True)


def follow(name):
    home, ap_port, ctl = ports(name)
    DIR.mkdir(parents=True, exist_ok=True)
    STOP.unlink(missing_ok=True)
    if not SAVED.exists():
        c = ps("config")
        if c.get("ok"):
            SAVED.write_text(json.dumps({"ssid": c["ssid"], "pass": c["pass"]}), encoding="utf-8")
    PIDFILE.write_text(json.dumps({"pid": os.getpid(), "clock": name}))
    start_guard()
    try:
        import ctypes
        ctypes.windll.kernel32.SetConsoleTitleW("Hotspot - follows the %s clock" % name)
    except Exception:
        pass
    ips = hotspot_addresses()
    port = 80
    s = socket.socket()
    try:
        s.bind(("0.0.0.0", 80))   # a test only: is port 80 free (IIS off)?
    except OSError:
        port = ap_port
    s.close()
    fwd = Forwarder(ips, port, ap_port)
    threading.Thread(target=fwd.run, daemon=True).start()
    log("following the %s clock (control port %d); page for phones will be http://%s" % (name, ctl, " or http://".join(fwd.where(i) for i in ips)))
    on, cur, checked = False, None, 0.0
    try:
        while not STOP.exists():
            want = None
            try:
                st = get_json("http://127.0.0.1:%d/state" % ctl)
                if st.get("ap"):
                    ssid, pw = st.get("apSsid"), st.get("apPass")
                    if not ssid:   # builds before 2026-11-13 don't say: the name the page shows, the password from Config.h
                        ssid = get_json("http://127.0.0.1:%d/api/status" % home).get("apSsid")
                        pw = config_ap_password()
                    want = (ssid, pw)
            except Exception:
                want = None   # down or restarting: the real clock's Wi-Fi is gone then too
            if want and (not on or want != cur):
                r = ps("start", want[0], want[1])
                if r.get("ok"):
                    on, cur = True, want
                    log('hotspot ON: "%s" (password %s)' % want)
                else:
                    log("hotspot could not start: %s" % r.get("error"))
                    time.sleep(5)
            elif not want and on:
                ps("stop")
                on = False
                log("hotspot OFF (the clock's own Wi-Fi is down)")
            elif on and time.time() - checked >= CHECK_S:
                checked = time.time()
                if ps("status").get("on") is False:   # Windows' power saving: off after ~5 min without a phone
                    on = False
                    log("Windows had switched the hotspot off (no device joined for a while) - on again")
                    continue
            time.sleep(POLL_S)
    finally:
        finish()
        log("stopped; the laptop's own hotspot settings are back")


def config_ap_password():
    for line in (ROOT / "include" / "Config.h").read_text(encoding="utf-8").splitlines():
        if "AP_PASSWORD_DEFAULT" in line and line.lstrip().startswith("#define"):
            return line.split('"')[1]
    return ""


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    arg = sys.argv[1] if len(sys.argv) > 1 else "status"
    if arg == "stop":
        print("stopped" if stop_running() else "not running")
    elif arg == "status":
        print(json.dumps(ps("status")), PIDFILE.read_text() if PIDFILE.exists() else "(not following a clock)")
    elif arg == "spawnguard":
        spawn_guard(int(sys.argv[2]))
    elif arg == "guard":
        guard(int(sys.argv[2]))
    elif arg in ("family", "dev"):
        stop_running()
        follow(arg)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
