"""Moves this PC onto the clock's own network ("JewishClock", as the parents'
laptop reaches it), runs a test there, and always moves back to the home
network - even when the test fails. This PC has one Wi-Fi adapter: while it
is on the clock's network there is no internet at all.

    python utils/ap_trip.py            # trial: read /api/status, come back
    (or import it: with on_clock_ap(): ...)
"""
import contextlib
import json
import subprocess
import sys
import time
import urllib.request

CLOCK_AP = "JewishClock"
HOME = "MyNetwork2"
CLOCK_URL = "http://192.168.4.1"


def _connected_ssid():
    out = subprocess.run(["netsh", "wlan", "show", "interfaces"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        k, _, v = line.partition(":")
        if k.strip() == "SSID":
            return v.strip()
    return None


def _scan():
    """Asks Windows for a fresh Wi-Fi scan (WlanScan). Without one, the list
    of networks it knows can hold only the connected one, and "netsh wlan
    connect" to the clock then fails (2026-11-02: 60 s, nothing)."""
    import ctypes
    from ctypes import wintypes

    class GUID(ctypes.Structure):
        _fields_ = [("d1", wintypes.DWORD), ("d2", wintypes.WORD), ("d3", wintypes.WORD), ("d4", ctypes.c_ubyte * 8)]

    class INFO(ctypes.Structure):
        _fields_ = [("guid", GUID), ("desc", ctypes.c_wchar * 256), ("state", ctypes.c_int)]

    class LIST(ctypes.Structure):
        _fields_ = [("n", wintypes.DWORD), ("i", wintypes.DWORD), ("items", INFO * 8)]

    wlan = ctypes.windll.wlanapi
    h, ver, lst = wintypes.HANDLE(), wintypes.DWORD(), ctypes.POINTER(LIST)()
    if wlan.WlanOpenHandle(2, None, ctypes.byref(ver), ctypes.byref(h)):
        return
    try:
        if not wlan.WlanEnumInterfaces(h, None, ctypes.byref(lst)):
            for k in range(lst.contents.n):
                wlan.WlanScan(h, ctypes.byref(lst.contents.items[k].guid), None, None, None)
            wlan.WlanFreeMemory(lst)
    finally:
        wlan.WlanCloseHandle(h, None)


def _join(ssid, check, limit_s=60):
    t0 = time.time()
    while time.time() - t0 < limit_s:
        if _connected_ssid() != ssid:   # scan, then ask again - every 15 s until it's on
            _scan()
            time.sleep(5)
            subprocess.run(["netsh", "wlan", "connect", "name=" + ssid], capture_output=True, text=True)
        for _ in range(5):
            if _connected_ssid() == ssid and check():
                return time.time() - t0
            time.sleep(2)
    raise RuntimeError("could not join %s within %d s" % (ssid, limit_s))


def _reachable(url, timeout=4):
    try:
        urllib.request.urlopen(url, timeout=timeout).read(1)
        return True
    except Exception:
        return False


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


@contextlib.contextmanager
def on_clock_ap(clock_ap=CLOCK_AP, home=None):
    """home: the network to come back to - by default the one this PC is on now (at the parents':
    their Wi-Fi, not ours - 2026-11-19), else HOME."""
    home = home or (_connected_ssid() if _connected_ssid() not in (None, clock_ap) else HOME)
    log("will come back to %s" % home)
    try:
        took = _join(clock_ap, lambda: _reachable(CLOCK_URL + "/api/status"), 120)
        log("on %s after %.0f s" % (clock_ap, took))
        yield CLOCK_URL
    finally:
        for attempt in range(5):
            try:
                took = _join(home, lambda: _reachable("http://www.msftconnecttest.com/connecttest.txt"))
                log("back on %s after %.0f s" % (home, took))
                break
            except RuntimeError as e:
                log("%s (attempt %d)" % (e, attempt + 1))


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    with on_clock_ap() as url:
        st = json.loads(urllib.request.urlopen(url + "/api/status", timeout=10).read())
        log("clock status over its own network: version %s, ip %s, uptime %s" %
            (st.get("version"), st.get("ip"), st.get("uptime")))
