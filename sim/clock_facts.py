"""What a test may take as given about the clock - checked, never assumed (the user, 2026-11-19/20).

Numbers: ask the running clock - /api/limits (what a setting or event may be) and /api/tunables
(its inner timings and thresholds that tests depend on, Tunables.h). Never a copy in a test.

Screen texts: ask the clock too - /api/texts (jcsim --texts): every system message the screen can
show, by situation (include/Texts.h, the one list the screen code itself uses). screen_text()
gives the exact words for a situation; a situation the clock doesn't list is a test failure,
reported through the caller's own check() - never a crash, never a silent pass. (Until 2026-11-20 a
test only checked that its words appeared somewhere in the code - right words in the wrong place
went unseen.)
"""
import json
import os
import socket
import struct
import subprocess
import time

MISSING = "\0(no such screen text)"   # never on any screen: a check with it fails, an absence check is
                                      # already failed by the missing-text report itself


def screen_text(texts, name, *fill, missing=None):
    """The clock's words for situation `name` (texts: /api/texts or jcsim_texts()). fill: what the
    clock puts in for each {} in turn; fewer given than there are {} -> the words up to the first
    one not given (e.g. "Clock's Wi-Fi: " for a line whose name and password vary).
    Not listed: missing(name) is called - the test's own check(..., False) - and MISSING returned."""
    if name not in texts:
        if missing:
            missing(name)
        return MISSING
    out, rest, args = "", texts[name], list(fill)
    while "{}" in rest:
        before, rest = rest.split("{}", 1)
        out += before
        if not args:
            return out
        out += str(args.pop(0))
    return out + rest


import re  # noqa: E402

# the parts the clock fills in: {} (Texts::fill) and printf specifiers (log lines)
_FILLED = re.compile(r"\{\}|%[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z|j|t|L)?[diouxXeEfgGcsp]")


class ClockWords:
    """The clock's words (/api/texts, jcsim/jcclock --texts) for checks against its screen, answers
    and log. Every method takes a situation's name; a name not listed = missing(name) - the
    test's own failed check - and a pattern nothing matches (never a crash, never a silent pass).
        W.text(name, *fill)          the words, filled in (or up to the first part not given)
        W.has(name, haystack)        does the haystack hold that message (filled parts: anything)
        W.count(name, haystack)      how often
        W.values(name, haystack)     what the clock filled in, each time: [("+204", "12"), ...]
    """

    def __init__(self, texts, missing=None):
        self.texts, self.missing, self._rx = texts, missing, {}

    def _get(self, name):
        if name not in self.texts:
            if self.missing:
                self.missing(name)
            return None
        return self.texts[name]

    def text(self, name, *fill):
        return MISSING if self._get(name) is None else screen_text(self.texts, name, *fill)

    def pattern(self, name, *fill):
        """The message as a pattern; fill: values for its filled parts in turn (None = anything)."""
        key = (name,) + tuple(fill)
        if key not in self._rx:
            t = self._get(name)
            if t is None:
                return re.compile(r"(?!x)x")   # matches nothing
            parts = _FILLED.split(t.replace("%%", "%"))
            rx = re.escape(parts[0])
            for i, p in enumerate(parts[1:]):
                given = fill[i] if i < len(fill) else None
                last = i == len(parts) - 2 and not p   # a filled part ending the message: the rest of its line
                rx += ("(.*)" if last else "(.*?)") if given is None else "(" + re.escape(str(given)) + ")"
                rx += re.escape(p)
            self._rx[key] = re.compile(rx)
        return self._rx[key]

    def has(self, name, haystack, *fill):
        return bool(self.pattern(name, *fill).search(haystack or ""))

    def count(self, name, haystack, *fill):
        return len(self.pattern(name, *fill).findall(haystack or ""))

    def last_at(self, name, haystack, *fill):
        """Where the last such message starts in the haystack, -1 if none (for checks of order)."""
        found = [m.start() for m in self.pattern(name, *fill).finditer(haystack or "")]
        return found[-1] if found else -1

    def values(self, name, haystack):
        return [m.groups() for m in self.pattern(name).finditer(haystack or "")]


def build_words(exe, missing=None):
    """ClockWords of a build (its --texts) - asked of the program itself, so it also serves while
    an older version runs on a test clock (an update drill)."""
    return ClockWords(_jcsim_ask(exe, "--texts"), missing)


def test_random(name):
    """The run's random choices (2026-11-20, the user: vary order, sequences, combinations - the
    kind that can happen for real): one seed per run (JC_TEST_SEED, set and printed by
    run_all_tests.py; a test run alone makes its own and prints it), mixed with `name` so each
    use has its own stream. Give the printed seed back (--seed) to run the same choices again."""
    import random
    if "JC_TEST_SEED" not in os.environ:
        os.environ["JC_TEST_SEED"] = str(int(time.time() * 1000) % 1000000)
        print("seed %s (again: JC_TEST_SEED=%s)" % (os.environ["JC_TEST_SEED"], os.environ["JC_TEST_SEED"]), flush=True)
    return random.Random("%s/%s" % (os.environ["JC_TEST_SEED"], name))


_JCSIM = {}


def _jcsim_ask(exe, flag):
    key = (str(exe), flag)
    if key not in _JCSIM:
        out = subprocess.run([str(exe), flag], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60).stdout
        lines = out.strip().splitlines()
        if not lines:
            raise RuntimeError("%s %s answered nothing - an older build? (python sim/build.py)" % (exe, flag))
        _JCSIM[key] = json.loads(lines[-1])
    return _JCSIM[key]


def jcsim_tunables(exe):
    """The screen simulator's answer to the clock's /api/tunables (jcsim --tunables) - the same
    code, so the same values; for tests that run jcsim, not a web clock."""
    return _jcsim_ask(exe, "--tunables")


def jcsim_texts(exe):
    """The screen simulator's answer to the clock's /api/texts (jcsim --texts)."""
    return _jcsim_ask(exe, "--texts")


_NTP_REST_FILE = "C:/pio3/jc_clean/ntp_reference_rest.json"   # server -> PC time before which no test process asks it


class _RestLock:
    """One test process at a time reads and writes _NTP_REST_FILE (a full run has several at once:
    the spacing is the PC's, in real time - the user, 2026-11-21)."""
    def __enter__(self):
        self.path = _NTP_REST_FILE + ".lock"
        t0 = time.time()
        while True:
            try:
                self.fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                return self
            except FileExistsError:
                if time.time() - t0 > 10:   # left by a killed process
                    try:
                        os.remove(self.path)
                    except OSError:
                        pass
                time.sleep(0.05)

    def __exit__(self, *a):
        os.close(self.fd)
        os.remove(self.path)


def _ntp_rest(server, until=None):
    """When `server` may be asked again (PC time); with `until`: set it (never shorter than it is)."""
    with _RestLock():
        try:
            rest = json.loads(open(_NTP_REST_FILE, encoding="utf-8").read())
        except (OSError, ValueError):
            rest = {}
        if until is not None:
            rest[server] = max(until, rest.get(server, 0))
            os.makedirs(os.path.dirname(_NTP_REST_FILE), exist_ok=True)
            with open(_NTP_REST_FILE, "w", encoding="utf-8") as f:
                f.write(json.dumps(rest))
        return rest.get(server, 0)


def ntp_now(same_server_min_s, _waited=False):
    """The one reference time for tests (resilience, soak). UTC from a real NTP server (independent of the PC's clock) - with the checks the clock
    makes (2026-11-20: a pool server answered with no time in it and this read 1900 - the clock,
    checking, was right): its own random stamp echoed back, a server's reply, stratum 1-15, a
    time in it; else the next server. The time at the middle of the round trip.
    Polite (2026-11-21, the user: "read the reply and act upon it always everywhere"): a server is never
    asked again within `same_server_min_s` - the clock's own SAME_SERVER_MIN_MS (/api/tunables), by every
    test process together (_NTP_REST_FILE) - and a kiss-o'-death rests it - RATE the longer of 5 min and its
    poll field (at most 2 h), DENY/RSTR a day."""
    for server in ("pool.ntp.org", "time.cloudflare.com", "time.google.com"):
        if time.time() < _ntp_rest(server):
            continue
        _ntp_rest(server, time.time() + same_server_min_s)
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.settimeout(5)
            stamp = os.urandom(8)
            t_send = time.time()
            s.sendto(b"\x23" + 39 * b"\0" + stamp, (server, 123))
            d = s.recv(48)
            t_got = time.time()
            s.close()
        except OSError:
            continue
        if len(d) >= 48 and d[1] == 0 and d[24:32] == stamp:   # a kiss-o'-death (to OUR question): obeyed - RFC 5905 s7.4
            code = d[12:16].decode("ascii", "replace").strip("\0")
            poll = struct.unpack("b", d[2:3])[0]   # RATE: the server's own spacing (log2 s), at most 2 h (RFC 8633)
            if code == "RATE":
                wait_s = min(max(300, 2 ** poll if 0 < poll < 14 else 0), 7200)
            elif code in ("DENY", "RSTR"):
                wait_s = 86400   # "stop sending packets to that server" - for the tests on this PC, a day
            else:
                wait_s = same_server_min_s   # no meaning: not used, nothing more
            _ntp_rest(server, time.time() + wait_s)
            print("     (reference time: %s answered kiss-o'-death %r - not asked for %d s)" % (server, code, wait_s), flush=True)
            continue
        mode, stratum = d[0] & 7, d[1]
        sec, frac = struct.unpack("!II", d[40:48])
        if len(d) < 48 or d[24:32] != stamp or mode != 4 or not 1 <= stratum <= 15 or sec == 0:
            print("     (reference time: %s's reply not usable - mode %d, stratum %d - the next server)" % (server, mode, stratum), flush=True)
            continue
        return sec - 2208988800 + frac / 2 ** 32, (t_send + t_got) / 2
    # every one resting (asked by another test process lately): wait for the first one free, once
    soonest = min(_ntp_rest(s) for s in ("pool.ntp.org", "time.cloudflare.com", "time.google.com"))
    if not _waited and soonest - time.time() <= same_server_min_s + 1:
        time.sleep(max(0.0, soonest - time.time()) + 0.1)
        return ntp_now(same_server_min_s, _waited=True)
    raise RuntimeError("no usable reference time from any NTP server (or all resting)")
