"""
Screen edge cases on simulated time (2026-11-18, from the coverage list - code no test reached):
  - the demo's end, alternating and single-scroll: the demo's slides, then the normal ones again;
  - every content row off: the clock's name, not an empty screen;
  - event names with brackets (mirrored in a Hebrew line), a currency sign, a 3-byte character
    the font lacks, and an emoji that isn't one of the clock's pictures: shown, the clock running.

    C:/pio3/jc_clean/venv/Scripts/python sim/test_screen_edges.py      (after sim/build.py)
"""
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORK = Path("C:/pio3/jc_clean/sim/work/test_screen_edges")
EXE = Path("C:/pio3/jc_clean/sim/build/jcsim.exe")
MOMENT = "2026-11-17 12:00"   # a plain Tuesday, Hebrew date ז׳ כסלו
fails = []


def check(what, ok, detail=""):
    print("  %s  %s%s" % ("ok  " if ok else "FAIL", what, "" if ok else "  -> %s" % (detail,)))
    if not ok:
        fails.append(what)


def run(name, extra, seconds, cfg_extra=None, events=None):
    """jcsim at MOMENT for `seconds`: the slides' texts in order, and what it printed."""
    fs = WORK / name / "fs"
    shutil.rmtree(WORK / name, ignore_errors=True)
    shutil.copytree(ROOT / "Data", fs, dirs_exist_ok=True)
    (fs / "jewish_calendar_data.json").unlink(missing_ok=True)
    cfg = json.loads((fs / "config.json").read_text(encoding="utf-8"))
    cfg.update({"language": "he", "rotationMode": 0})
    cfg.update(cfg_extra or {})
    (fs / "config.json").write_text(json.dumps(cfg, ensure_ascii=False), encoding="utf-8")
    if events is not None:
        (fs / "events.json").write_text(json.dumps(events, ensure_ascii=False), encoding="utf-8")
    out = WORK / name / "out"
    r = subprocess.run([str(EXE), "--fs", str(fs), "--time", MOMENT, "--seconds", str(seconds), "--out", str(out)] + extra,
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    texts = []
    for line in (out / "slides.txt").read_text(encoding="utf-8").splitlines():
        parts = line.split("\t")
        if len(parts) >= 4 and not parts[1].startswith("1970"):
            texts.append(parts[-1])
    return texts, r.stdout + r.stderr


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    if not EXE.exists():
        sys.exit("build the screen simulator first: python sim/build.py")
    today = "ז׳ כסלו"

    # ── the demo's end, both modes ──
    normal, _ = run("normal", [], 120)
    for mode, label in ((0, "alternating"), (1, "single scroll")):
        texts, _ = run("demo_%d" % mode, ["--demo-at", "5"], 400, {"rotationMode": mode})
        base, _ = run("plain_%d" % mode, [], 400, {"rotationMode": mode})
        demo_only = [t for t in texts if t not in base]
        first_demo = next((i for i, t in enumerate(texts) if t in demo_only), None)
        after = texts[first_demo:] if first_demo is not None else []
        back = next((i for i, t in enumerate(after) if t not in demo_only and today in t), None)
        check("demo, %s: its own slides shown" % label, bool(demo_only), texts[:6])
        check("demo, %s: it ends, the normal slides come back (today's date)" % label,
              back is not None and not any(t in demo_only for t in after[back:]), after[:12])

    # ── every content row off ──
    off = {k: 0 for k in json.loads((ROOT / "Data" / "config.json").read_text(encoding="utf-8"))["contentFrequency"]}
    name = json.loads((ROOT / "Data" / "config.json").read_text(encoding="utf-8"))["projectName"]
    # instant boot: content only - a long simulated boot (random, by the seed) shows "Waiting for the time..."
    # first, rightly; that is the start, not "every row off" (2026-11-21, a night run drew one)
    texts, _ = run("all_off", ["--instant-boot"], 60, {"contentFrequency": off})
    check("every content row off: the clock's name, nothing else", texts and all(name.split()[0] in t for t in texts), texts[:4])

    # ── unusual characters in event names (shown on their day) ──
    def ev(n, nm):
        return {"id": "edge%04d" % n, "name": nm, "hasG": True, "gD": 17, "gM": 11, "gY": 0, "afterSunset": False, "hasJ": False,
                "jD": 0, "jM": 0, "jYNum": 0, "icon": "", "warn": 0, "type": "other", "image": ""}
    names = ["UIT (סוגריים) [x]", "UIT ₪100", "UIT 日本", "UIT 🦄"]
    texts, said = run("names", [], 120, events=[ev(i, n) for i, n in enumerate(names)])
    shown = " | ".join(texts)
    for n in names:
        check("event name %r: on the screen" % n, n.split()[1][:2] in shown or n in shown, shown[:200])
    check("... the clock ran on (no crash, slides to the end)", len(texts) > 5 and "FATAL" not in said, len(texts))

    print("\n%d failed" % len(fails))
    print("screen edges test: ALL CORRECT" if not fails else "screen edges test: FAILED")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
