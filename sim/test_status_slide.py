"""
The status slide on simulated time (2026-11-14) - system messages, English in every display
language:
  - the clock's own Wi-Fi and its password: every round in the 3 minutes after a start
    (no lockout: unplug, read it off the screen), then not;
  - online, but no time server for 3 days: "No NTP time server since <day> <month>" (the
    longest wording that fits the window, never scrolling) - and not at 2.9 days;
  - a text too long in the clock's own files: "Error - see Settings", in red,
    for 3 days from the day first shown (2026-11-15).

    C:/pio3/jc_clean/venv/Scripts/python sim/test_status_slide.py      (after sim/build.py)
"""
import json
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from clock_facts import ClockWords, jcsim_texts, jcsim_tunables, screen_text  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
WORK = Path("C:/pio3/jc_clean/sim/work/test_status_slide")
EXE = Path("C:/pio3/jc_clean/sim/build/jcsim.exe")
MOMENT = "2026-11-17 12:00"   # a plain Tuesday
fails = []


def check(what, ok, detail=""):
    print("  %s  %s%s" % ("ok  " if ok else "FAIL", what, "" if ok else "  -> %s" % (detail,)))
    if not ok:
        fails.append(what)


def run(name, extra, seconds, cfg_extra=None, files=None):
    fs = WORK / name / "fs"
    shutil.rmtree(WORK / name, ignore_errors=True)
    shutil.copytree(ROOT / "Data", fs, dirs_exist_ok=True)
    (fs / "jewish_calendar_data.json").unlink(missing_ok=True)
    for rel, text in (files or {}).items():
        (fs / rel).write_text(text, encoding="utf-8")
    cfg = json.loads((fs / "config.json").read_text(encoding="utf-8"))
    cfg.update({"language": "he", "rotationMode": 0})   # Hebrew slides, alternating - as the family's clock
    cfg.update(cfg_extra or {})
    (fs / "config.json").write_text(json.dumps(cfg, ensure_ascii=False), encoding="utf-8")
    out = WORK / name / "out"
    subprocess.run([str(EXE), "--fs", str(fs), "--time", MOMENT, "--seconds", str(seconds), "--out", str(out)] + extra, capture_output=True)
    rows = []
    for line in (out / "slides.txt").read_text(encoding="utf-8").splitlines():
        parts = line.split("\t")
        if len(parts) >= 4 and not parts[1].startswith("1970"):
            rows.append((datetime.strptime(parts[1], "%Y-%m-%d %H:%M:%S"), parts[-1]))
            COLOURS[(name, parts[-1])] = colour(out / (parts[0] + ".bmp"))
    return rows


COLOURS = {}


def colour(bmp):
    """'red', 'gold' or 'other': the text's colour in a slide's picture (most bright pixels)."""
    from PIL import Image
    if not bmp.exists():
        return "none"
    red = gold = other = 0
    px = Image.open(bmp).convert("RGB").tobytes()
    for i in range(0, len(px), 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        if r > 150 and g < 80 and b < 80:
            red += 1
        elif r > 150 and g > 120 and b < 80:
            gold += 1
        elif r > 150 or g > 150 or b > 150:
            other += 1
    best = max(red, gold, other)
    return "red" if best == red and red else "gold" if best == gold and gold else "other"


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    if not EXE.exists():
        sys.exit("build the screen simulator first: python sim/build.py")
    # The clock's numbers and words, asked (the user, 2026-11-19)
    T = jcsim_tunables(EXE)
    STARTUP_S = T["BOOT_OVERLAY_MS"] / 1000
    NTP_H = T["NTP_NOTICE_AFTER_MS"] / 3600000
    EVERY_N = T["OFFLINE_NOTICE_EVERY_N_ROUNDS"]
    TX = jcsim_texts(EXE)   # the screen's own words (Texts.h)
    say = lambda name, *fill: screen_text(TX, name, *fill, missing=lambda n: check("the clock lists a screen text %r (--texts)" % n, False))
    WIFI_LINE, NTP_LINE = say("clockWifi"), say("noNtp")   # each up to its first variable part
    long_s = STARTUP_S + 240
    print("1. after a start: the clock's Wi-Fi, every round for %.0f s, then not" % STARTUP_S)
    rows = run("start", [], long_s)
    t0 = rows[0][0] if rows else None
    wifi = [(t - t0).total_seconds() for t, s in rows if s.startswith(WIFI_LINE)]
    check("shown in the startup minutes, in English, with its name and password",
          any(x < STARTUP_S - 30 for x in wifi) and all(ClockWords(TX).has("clockWifi", s) for t, s in rows if s.startswith(WIFI_LINE)), wifi[:5])
    check("not after them", not any(x > STARTUP_S + 20 for x in wifi), wifi)
    gold = {COLOURS.get(("start", s_)) for t_, s_ in rows if s_.startswith(WIFI_LINE)}
    check("... in gold (no problem: only the Wi-Fi)", gold == {"gold"}, gold)
    check("no time-server line when it synced", not any("NTP" in s for t, s in rows), [s for t, s in rows if "NTP" in s][:2])

    hours = NTP_H + 2
    days = int(hours // 24)
    print("2. online, the last time sync %.0f hours ago (the line comes after %.0f)" % (hours, NTP_H))
    rows = run("ntp_silent", ["--ntp-silent-hours", str(int(hours))], STARTUP_S + 420)
    ntp = [s for t, s in rows if NTP_LINE in s]
    check("'%s...' shown (every %dth round)" % (NTP_LINE, EVERY_N), bool(ntp), [s for t, s in rows][:12])
    later = [s for t, s in rows if NTP_LINE in s and (t - rows[0][0]).total_seconds() > STARTUP_S + 20]
    want = say("noNtp", days)
    check("... after the startup minutes alone on its slide, the default window's wording: '%s' (fits, no scrolling)" % want,
          bool(later) and all(s == want for s in later), later[:4])
    check("... in red (a problem)", COLOURS.get(("ntp_silent", want)) == "red", COLOURS.get(("ntp_silent", want)))

    print("2b. 40 days without a time server: the count goes on")
    rows = run("ntp_long", ["--ntp-silent-hours", str(40 * 24 + 2)], STARTUP_S + 420)
    later = [s for t, s in rows if NTP_LINE in s and (t - rows[0][0]).total_seconds() > STARTUP_S + 20]
    check("'%s'" % say("noNtp", 40), bool(later) and all(s == say("noNtp", 40) for s in later), later[:3])

    print("3. the last sync 2 hours short of it: nothing yet")
    rows = run("ntp_recent", ["--ntp-silent-hours", str(int(NTP_H - 2))], long_s)
    check("no '%s' line" % NTP_LINE, not any(NTP_LINE in s for t, s in rows), [s for t, s in rows if "NTP" in s][:2])

    print("4. a text too long in the clock's own settings file (the user, 2026-11-15)")
    TOO_LONG = say("errorSeeSettings")
    long_sep = {"separator": "שלום" * 5}   # longer than its limit: kept as far as it fits, noted
    rows = run("texts_long", [], long_s, long_sep)
    said = [s for t, s in rows if TOO_LONG in s]
    later = [s for t, s in rows if TOO_LONG in s and (t - rows[0][0]).total_seconds() > STARTUP_S + 20]
    check("'%s' on the status slide" % TOO_LONG, bool(said), [s for t, s in rows][:10])
    check("... after the startup minutes too (every %dth round), alone, in red" % EVERY_N,
          bool(later) and all(COLOURS.get(("texts_long", s)) == "red" for s in later), [(s, COLOURS.get(("texts_long", s))) for s in later[:3]])
    notes_file = WORK / "texts_long" / "fs" / "text_notes.json"
    kept = json.loads(notes_file.read_text(encoding="utf-8")) if notes_file.exists() else {}
    from datetime import date
    today = (date(*map(int, MOMENT[:10].split("-"))) - date(1970, 1, 1)).days
    check("... the day it was first shown kept (text_notes.json)", kept.get("day") == today, kept.get("day"))

    notes_days = int(T["TEXT_NOTES_SCREEN_DAYS"])
    print("4b. the same, first shown %d days ago: only in Settings now" % notes_days)
    kept["day"] = today - notes_days
    rows = run("texts_long_old", [], long_s, long_sep, {"text_notes.json": json.dumps(kept, ensure_ascii=False)})
    check("not on the screen", not any(TOO_LONG in s for t, s in rows), [s for t, s in rows if TOO_LONG in s][:2])
    kept2 = json.loads((WORK / "texts_long_old" / "fs" / "text_notes.json").read_text(encoding="utf-8"))
    check("... still noted (Settings lists it)", kept2.get("notes") and kept2.get("day") == today - notes_days, kept2)

    print("status slide test: " + ("ALL CORRECT" if not fails else "%d FAILED" % len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
