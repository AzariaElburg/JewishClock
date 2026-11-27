"""
The family simulator's data from an Excel export of theirs (2026-11-09):

    C:/pio3/jc_clean/venv/Scripts/python sim/seed_from_export.py <JewishClock-backup-....xlsx>

The family's data reaches us only as an Excel export (their clock is reachable
only over its own Wi-Fi). This puts it into the family clock (sim/start_clock.py
family, the version in sim/family_version.txt) the way they would: its own page,
Settings > Import > "Import both" (pictures, events, settings; the clock
restarts). Then it reads back what the clock saved (/api/backup, pictures,
replaced icons) as the new seed (C:/pio3/jc_clean/sim/family/seed/; the previous one
is kept as seed.<date>), restarts the family clock from it (--fresh) and checks
it against the file: every event, every picture, every setting.

Set family_version.txt first if they installed a new version.
"""
import datetime
import json
import shutil
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "sim"))
import start_clock as sc   # noqa: E402

FAMILY = sc.SIM / "family"
HOME = "http://127.0.0.1:%d" % sc.CLOCKS["family"]["home"]
# Not compared with the file: the clock keeps its own value (setup state,
# the debug-time offset is reset to 0 below, as for every seed).
NOT_COMPARED = {"setupComplete", "debugTimeOffset"}


def get(path, timeout=60):
    return urllib.request.urlopen(HOME + path, timeout=timeout).read()


def import_both(xlsx):
    from playwright.sync_api import sync_playwright
    with sync_playwright() as pw:
        b = pw.chromium.launch(channel="msedge")
        pg = b.new_page(viewport={"width": 1280, "height": 900})
        pg.set_default_timeout(60000)
        pg.goto(HOME + "/", wait_until="load")
        pg.wait_for_function("typeof events !== 'undefined' && el('cfg-name').value !== ''")
        pg.click("button.rail-item[data-view=settings]")
        pg.set_input_files("#data-upload-input", str(xlsx))
        pg.wait_for_selector("#data-result button:has-text('Import both')", timeout=30000)
        print("page:", pg.inner_text("#data-result").replace("\n", " ")[:300])
        pg.click("#data-result button:has-text('Import both')")
        t0 = time.time()
        while time.time() - t0 < 180:   # the page reloads itself once the clock is back
            try:
                if pg.evaluate("document.readyState === 'complete' && typeof events !== 'undefined' && events.length > 0"
                               " && !/Importing|Settings received|restarting/.test(el('data-result').textContent)") and time.time() - t0 > 8:
                    break
                said = pg.inner_text("#data-result")
                if "not imported" in said or "not saved" in said or "failed" in said.lower():
                    sys.exit("import refused: " + said.replace("\n", " "))
            except Exception:
                pass   # in the middle of the reload
            time.sleep(2)
        else:
            sys.exit("the page did not come back within 180 s")
        b.close()
    for _ in range(30):   # and the clock answers
        try:
            json.loads(get("/api/status", 5))
            return
        except Exception:
            time.sleep(2)
    sys.exit("the clock did not answer after the import")


def read_back(dest):
    backup = json.loads(get("/api/backup").decode("utf-8"))
    cfg, evts = backup.get("config"), backup.get("events")
    if not isinstance(cfg, dict) or not isinstance(evts, list) or not cfg.get("wifiSSID"):
        sys.exit("backup incomplete - seed left as it was")
    cfg["debugTimeOffset"] = 0
    dest.mkdir(parents=True)
    (dest / "config.json").write_text(json.dumps(cfg, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    (dest / "events.json").write_text(json.dumps(evts, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    images = json.loads(get("/api/images/list").decode("utf-8"))
    if images:
        (dest / "images").mkdir()
    for im in images:
        (dest / "images" / (im["id"] + ".bmp")).write_bytes(get("/api/images/get?id=" + im["id"]))
    custom = [p["name"] for p in json.loads(get("/api/builtin/list").decode("utf-8"))["pictures"] if p.get("custom")]
    if custom:
        (dest / "custom").mkdir()
    for name in custom:
        (dest / "custom" / (name + ".bmp")).write_bytes(get("/api/builtin/get?which=custom&name=" + name))
    return len(evts), [im["id"] for im in images], custom


def sheet(wb, name):
    rows = [r for r in wb[name].iter_rows(values_only=True) if any(c is not None for c in r)] if name in wb.sheetnames else []
    return rows[0] if rows else (), rows[1:]


def same(file_value, clock_value):
    if file_value is None:
        return clock_value in (None, "", [], {})
    if isinstance(clock_value, (list, dict)):
        try:
            return json.loads(str(file_value)) == clock_value
        except ValueError:
            return False
    if isinstance(clock_value, bool):
        return str(file_value).lower() == str(clock_value).lower()
    if isinstance(clock_value, (int, float)):
        try:
            return abs(float(file_value) - float(clock_value)) < 1e-6
        except ValueError:
            return False
    return str(file_value) == str(clock_value)


def check(xlsx):
    """The clock started from the new seed against the file."""
    import openpyxl
    wb = openpyxl.load_workbook(xlsx)
    problems = []
    head, rows = sheet(wb, "Events")
    file_names = sorted(str(r[0]).strip() for r in rows)
    clock_events = json.loads(get("/api/events/list").decode("utf-8"))
    clock_events = clock_events.get("events", clock_events) if isinstance(clock_events, dict) else clock_events
    clock_names = sorted(e["name"].strip() for e in clock_events)
    if file_names != clock_names:
        problems.append("events differ: only in file %s, only on clock %s" % (
            sorted(set(file_names) - set(clock_names)), sorted(set(clock_names) - set(file_names))))
    pic_col = head.index("Picture") if "Picture" in head else None
    wanted = sorted({str(r[pic_col]) for r in rows if pic_col is not None and r[pic_col]})
    have = sorted(im["id"] for im in json.loads(get("/api/images/list").decode("utf-8")))
    if wanted != have:
        problems.append("pictures differ: file %s, clock %s" % (wanted, have))
    cfg = json.loads(get("/api/backup").decode("utf-8"))["config"]
    _, settings = sheet(wb, "Settings")
    compared = 0
    for key, value, *_ in settings:
        if key in NOT_COMPARED:
            continue
        compared += 1
        if key not in cfg:
            problems.append("setting %s: in the file, not on the clock" % key)
        elif not same(value, cfg[key]):
            problems.append("setting %s: file %r, clock %r" % (key, value, cfg[key]))
    print("checked: %d events, %d pictures, %d settings" % (len(clock_names), len(have), compared))
    return problems


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    if len(sys.argv) != 2 or not Path(sys.argv[1]).is_file():
        sys.exit(__doc__)
    xlsx = Path(sys.argv[1]).resolve()
    print("family version:", sc.family_version())
    sc.start("family", fresh=True, build=False)
    import_both(xlsx)
    new = FAMILY / "seed.new"
    shutil.rmtree(new, ignore_errors=True)
    n, pics, custom = read_back(new)
    (new / "FROM.txt").write_text("From the Excel export %s, imported %s on %s (sim/seed_from_export.py)\n" % (
        xlsx.name, datetime.datetime.now().strftime("%Y-%m-%d %H:%M"), sc.family_version()), encoding="utf-8")
    print("read back: %d events, pictures %s, replaced icons %s" % (n, pics, custom))
    sc.stop("family")
    old = FAMILY / ("seed." + datetime.datetime.now().strftime("%Y%m%d_%H%M%S"))
    (FAMILY / "seed").rename(old)
    new.rename(FAMILY / "seed")
    print("previous seed kept as", old)
    sc.start("family", fresh=True, build=False)
    problems = check(xlsx)
    for p in problems:
        print("MISMATCH:", p)
    print("OK - family seed matches the export" if not problems else "%d mismatch(es)" % len(problems))
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
