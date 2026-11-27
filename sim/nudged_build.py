"""
The numbers run (the user, 2026-11-20 - the compliance run's twin for numbers): every value a test
depends on changed a little, then the whole test suite run on it. A test that asks the clock for its
numbers (/api/tunables, /api/limits) still passes; one that keeps its own copy - or assumes one in a
wait, a date, a count - fails, and so shows where it is.
    C:/pio3/jc_clean/venv/Scripts/python sim/nudged_build.py [--seed N] [--show]   # builds jcclock.exe + jcsim.exe from it
    C:/pio3/jc_clean/venv/Scripts/python sim/run_all_tests.py --from 2.1.2 --skip build-jcsim build-clock dates simulator-validity
What is changed (each up or down, chosen by the seed - printed, and written to the tree's nudges.txt):
- every shared tunable (TUNABLE_SHARED in src/): times, percentages and other measures x5/4 or x4/5;
  counts, days, an hour, a precision exponent +-1. NUDGE_RULES below: the ones with a floor or a
  fixed direction, each with its reason.
- our own limits (Config.h LIMITS) in LIMIT_NUDGES below - never a fact of the hardware or a
  standard (Wi-Fi name 32 bytes, WPA2 8-63, the screen's pixels, the panel's frame time).
The copy lives in C:/pio3/jc_clean/nudged/ (never the project folder); a nudge the code's own
static_asserts refuse stops the build - the rule to add here, not a change to the code.
"""
import os
import random
import re
import shutil
import stat
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TREE = Path("C:/pio3/jc_clean/nudged/tree")
PY = sys.executable

# name -> "up" (only longer/larger: a floor below it), "keep" (a fact, not a choice), each with why
NUDGE_RULES = {
    "SAME_SERVER_MIN_MS": ("up", "RFC 4330: never sooner than 15 s to the same server"),
    "OWN_ROUNDING_MS": ("keep", "the sum of the clock's own three roundings - measured, not chosen"),
    "AUTO_INSTALL_HOUR": ("pm1", "an hour of the night: 2 or 4"),
    # "with:OTHER" = the same way as OTHER (its own draw still made, so every other nudge of a seed stays)
    "PENDING_FLUSH_BYTES": ("with:PENDING_BYTES", "DiagnosticLog.cpp static_assert: written out before the "
                            "waiting room is full - 2048 x5/4 is above 3072 x4/5"),
}
SMALL_UNITS = ("_DAYS", "_HOUR", "_ROUNDS", "_PREC", "_RETRIES", "_ATTEMPTS", "_AFTER")

# --all (2026-11-21): the tunables nobody outside their file asks about, nudged too - a test that
# silently depends on one (a wait, a signal level, a count) shows up. Not these (each why):
ALL_KEEP = {
    "SIMULATED_LUX": "the stand-in light level the brightness tests were built on",
    "SENSOR_MTREG": "the BH1750's measurement time register - a hardware setting",
    "PICTURE_MAX_H_ROWS": "the screen's rows",
    "TEXT_BELOW_MIDDLE_PX": "drawing: the screen tests compare pixels with recordings",
    "COLON_SIDE_PAD_PX": "drawing: the screen tests compare pixels with recordings",
    "SERIAL_TX_BUFFER_BYTES": "a buffer size, no behaviour",
    "MAX_LOG_BYTES": "DiagnosticLog::MAX_FILE_BYTES - its own place",
    "LINE_MAX_BYTES": "a buffer size",
    "WDT_TIMEOUT_SEC": "the watchdog: longer than any blocking step by design",
    "MDNS_STALE_MS": "a sum of two others (nudged themselves)",
    # facts of a protocol or a file format that live in a TUNABLES block (2026-11-21: nudged, they
    # broke time sync and every update outright - nothing learned)
    "NTP_PACKET_SIZE": "NTP: 48 bytes (RFC 5905)",
    "NTP_PORT": "NTP: port 123",
    "NTP_LOCAL_PORT": "the port the simulator's time servers answer",
    "NTP_EPOCH_OFFSET": "NTP: seconds 1900 -> 1970",
    "NTP_ERA_PIVOT_UNIX": "NTP era reading - a date, not a setting",
    "ESP_IMAGE_MAGIC": "ESP32 firmware image: first byte 0xE9",
}

# Config.h LIMITS: name -> (smaller, larger) - each a valid value; None = that way not allowed (why)
LIMIT_NUDGES = {
    "MESSAGE_DWELL_SEC_RANGE": ("{ 3, 50 }", "{ 2, 75 }"),
    "SCROLL_SPEED_RANGE": ("{ 1, 9 }", "{ 1, 12 }"),
    "EVENT_WARN_DAYS_RANGE": ("{ 0, 25 }", "{ 0, 40 }"),
    "FONT_CHAR_GAP_RANGE": ("{ 0, 25 }", "{ 0, 36 }"),
    "FONT_WORD_GAP_RANGE": ("{ 0, 64 }", "{ 0, 96 }"),
    "FONT_WIDTH_SCALE_RANGE": ("{ 60, 180 }", "{ 40, 220 }"),
    # the two event year ranges move together, 3760 apart (Config.h static_assert); 2239: where 5999
    # ends (the date maths' end)
    ("EVENT_GREG_YEAR_RANGE", "EVENT_HEB_YEAR_RANGE"): (("{ 1810, 2229 }", "{ 5570, 5989 }"), ("{ 1790, 2239 }", "{ 5550, 5999 }")),
    "DEBUG_TIME_YEAR_RANGE": ("{ 2001, 2290 }", "{ 2010, 2299 }"),  # 2299: the debug time's own end
    "EVENT_ICON_MAX_CHARS": ("2", "4"),
    "NTP_SERVER_MAX_CHARS": ("50", "80"),
    "EVENT_NAME_MAX_BYTES": ("119", "119"),     # its field is 128 bytes (static_assert): smaller only
    "SEPARATOR_MAX_BYTES": ("13", "13"),        # its field: smaller only
    "MAX_EVENTS": ("4000", "4000"),             # PSRAM: the drill measured 5000; smaller only
}


def tunable_names():
    """[(name, file it's defined in)] for every TUNABLE_SHARED / TUNABLE_SHARED_AS in src/."""
    out = []
    for p in sorted((ROOT / "src").glob("*.cpp")):
        text = p.read_text(encoding="utf-8")
        for m in re.finditer(r"TUNABLE_SHARED\((\w+)\)", text):
            out.append((m.group(1), p))
        for m in re.finditer(r"TUNABLE_SHARED_AS\((\w+),\s*[\w:]*?(\w+)\)", text):
            hdr = next((h for h in (ROOT / "include").glob("*.h") if re.search(r"\b%s\s*=" % m.group(2), h.read_text(encoding="utf-8"))), None)
            out.append((m.group(1), hdr))
    return [(n, f) for n, f in out if not n.startswith("name")]


def unshared_names():
    """[(name, file)] for the values in a TUNABLES block that aren't TUNABLE_SHARED (src/)."""
    shared = {n for n, _ in tunable_names()}
    out = []
    for p in sorted((ROOT / "src").glob("*.cpp")):
        on = False
        for line in p.read_text(encoding="utf-8").split("\n"):
            st = line.strip()
            if "=== TUNABLES" in line:
                on = True
                continue
            if not on or not st or st.startswith("//") or st.startswith("TUNABLE_SHARED"):
                continue
            m = re.match(r"(?:static\s+)?(?:constexpr|const)\s+[\w:<> ]+?\s([A-Z][A-Z0-9_]+)\s*=\s*([^;]+);", st)
            if not m:
                if st.endswith("{") or st.startswith(("namespace", "void ", "bool ", "struct ", "class ")):
                    on = False   # the block ends where code starts
                continue
            if m.group(1) not in shared and m.group(1) not in ALL_KEEP:
                out.append((m.group(1), p))
    return out


UP_CHOSEN = {}   # name -> went up (for the "with:" rule)
DBM_SHIFT = []   # every signal level moves the same way by the same amount (their order is a rule)


def nudge_expr(name, expr, rnd):
    rule = NUDGE_RULES.get(name, (None,))[0]
    if rule == "keep":
        return None
    if name.endswith("_DBM"):   # a level, not an amount: shifted, all together (2026-11-21: x5/4 and x4/5
        if not DBM_SHIFT:       # put "recovered" below "weak")
            DBM_SHIFT.append(5 if rnd.random() < 0.5 else -5)
        return "(%s) %+d" % (expr, DBM_SHIFT[0]), "%+d dB" % DBM_SHIFT[0]
    up = True if rule == "up" else rnd.random() < 0.5
    if rule and rule.startswith("with:"):
        up = UP_CHOSEN.get(rule[5:], up)
    UP_CHOSEN[name] = up
    if name.endswith(SMALL_UNITS) or rule == "pm1":
        return "(%s) %s 1" % (expr, "+" if up else "-"), ("+1" if up else "-1")
    return ("(%s) * 5 / 4" if up else "(%s) * 4 / 5") % expr, ("x5/4" if up else "x4/5")


def main():
    seed = os.environ.get("JC_TEST_SEED")
    if "--seed" in sys.argv:
        seed = sys.argv[sys.argv.index("--seed") + 1]
    seed = seed or str(random.randrange(1000000))
    rnd = random.Random("nudge/" + seed)
    def writable_then_again(func, path, _):   # files copied from the project can be read-only (OneDrive)
        os.chmod(path, stat.S_IWRITE)
        func(path)
    for old in [TREE] + list(TREE.parent.glob("tree_old_*")):
        if old.exists():
            shutil.rmtree(old, onerror=writable_then_again)
    for sub in ("src", "include"):
        shutil.copytree(ROOT / sub, TREE / sub)
    done = []
    names = tunable_names() + (unshared_names() if "--all" in sys.argv else [])
    for name, src in names:
        if src is None and any(re.search(r'#define\s+%s\s+"' % name, h.read_text(encoding="utf-8"))
                               for h in (ROOT / "include").glob("*.h")):
            done.append("%-32s kept (a text, not a number - e.g. the update feed's name)" % name)
            continue
        if src is None:
            sys.exit("nudged_build: %s - its definition not found" % name)
        dest = TREE / src.relative_to(ROOT)
        text = dest.read_text(encoding="utf-8")
        rx = re.compile(r"(\b%s\s*=\s*)([^;]+?)(\s*;)" % name)
        m = rx.search(text)
        if not m:
            sys.exit("nudged_build: %s - no 'NAME = value;' in %s" % (name, src.name))
        r = nudge_expr(name, m.group(2).strip(), rnd)
        if r is None:
            done.append("%-32s kept (%s)" % (name, NUDGE_RULES[name][1]))
            continue
        new, how = r
        text = text[:m.start(2)] + new + text[m.end(2):]
        dest.write_text(text, encoding="utf-8", newline="")
        done.append("%-32s %s  (%s)" % (name, how, m.group(2).strip()))
    cfg = TREE / "include" / "Config.h"
    text = cfg.read_text(encoding="utf-8")
    for names, (small, large) in LIMIT_NUDGES.items():
        pick = small if rnd.random() < 0.5 else large
        for name, new in (zip(names, pick) if isinstance(names, tuple) else [(names, pick)]):
            rx = re.compile(r"(\b%s\s*=\s*)([^;]+?)(\s*;)" % name) if name != "MAX_EVENTS" else re.compile(r"(#define MAX_EVENTS )(\d+)()")
            m = rx.search(text)
            if not m:
                sys.exit("nudged_build: limit %s not found in Config.h" % name)
            done.append("%-32s %s -> %s" % (name, m.group(2).strip(), new))
            text = text[:m.start(2)] + new + text[m.end(2):]
    cfg.write_text(text, encoding="utf-8", newline="")
    (TREE / "nudges.txt").write_text("seed %s\n" % seed + "\n".join(done) + "\n", encoding="utf-8")
    print("seed %s - %d values nudged:" % (seed, len(done)))
    print("\n".join("  " + d for d in done))
    if "--show" in sys.argv:
        return 0
    r = subprocess.run([PY, str(ROOT / "sim" / "build_clock.py"), "--tree", str(TREE), "--scrambled"])   # --scrambled: "a changed copy, where the tests take it"
    if r.returncode:
        return r.returncode
    return subprocess.run([PY, str(ROOT / "sim" / "build.py"), "--tree", str(TREE)]).returncode


if __name__ == "__main__":
    sys.exit(main())
