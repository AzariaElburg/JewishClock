"""The safe uploadfs sequence (CLAUDE.md "Build & Flash"), steps 1-2c and 5:

    python utils/sync_from_device.py pre     # before uploadfs
    pio run -c pioarduino.ini -t uploadfs
    python utils/sync_from_device.py post    # after it

pre:  notes "Data flashing initiated" in the clock's log, then copies the
      clock's live settings, events, log, event pictures and replaced
      pictures into Data/, so the upload carries them instead of wiping them.
      Aborts before touching Data/ if the backup looks wrong. The previous
      Data/config.json / events.json are kept in local_dirs.LOCAL/uploadfs_backups.
post: waits for the clock to come back and notes "Data flash complete".

--clock ADDRESS: the clock's address if jewishclock.local doesn't answer.
"""
import argparse
import datetime
import json
import shutil
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

from local_dirs import LOCAL

PROJ = Path(__file__).resolve().parent.parent
DEV = ""   # set in main(): the first address that answers


def get(path, timeout=30):
    with urllib.request.urlopen(DEV + path, timeout=timeout) as r:
        return r.read()


def note(msg):
    data = urllib.parse.urlencode({"msg": msg}).encode()
    with urllib.request.urlopen(urllib.request.Request(DEV + "/api/debug/diagnostic-log/note", data=data, method="POST"), timeout=15) as r:
        return r.status


def find_clock(addresses, max_s):
    """The first address whose /api/status answers, retried for max_s seconds."""
    global DEV
    end = time.time() + max_s
    while time.time() < end:
        for a in addresses:
            DEV = "http://" + a
            try:
                get("/api/status", timeout=4)
                print("clock at", a)
                return True
            except Exception:
                pass
        time.sleep(3)
    return False


def emptied(folder):
    """The folder, existing and empty. Emptied rather than deleted: OneDrive marks
    pinned folders read-only, so removing the folder itself fails (2026-10-31)."""
    folder.mkdir(exist_ok=True)
    for p in folder.iterdir():
        shutil.rmtree(p) if p.is_dir() else p.unlink()
    return folder


def pre():
    print("note:", note("Data flashing initiated"))
    backup = json.loads(get("/api/backup", 60).decode("utf-8"))
    cfg, evts = backup.get("config"), backup.get("events")
    if not isinstance(cfg, dict) or not isinstance(evts, list) or not cfg.get("wifiSSID"):
        sys.exit("backup incomplete - aborting before touching Data/")
    diag = get("/api/debug/diagnostic-log", 60)
    if b"Data flashing initiated" not in diag:
        sys.exit("diagnostic log missing the note - aborting")

    data = PROJ / "Data"
    bdir = LOCAL / "uploadfs_backups" / datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    bdir.mkdir(parents=True)
    for f in ("config.json", "events.json"):
        shutil.copy(data / f, bdir)

    removed_offset = cfg.get("debugTimeOffset")
    cfg["debugTimeOffset"] = 0  # a test's shifted time must never ship
    # Compact, one line, like the committed files (CLAUDE.md step 2b).
    (data / "config.json").write_text(json.dumps(cfg, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    (data / "events.json").write_text(json.dumps(evts, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
    # The log lives in /log since 2026-10-25 (Config.h FS_LOG_DIR).
    (data / "diagnostic_log.txt").unlink(missing_ok=True)
    (data / "log").mkdir(exist_ok=True)
    (data / "log" / "diagnostic_log.txt").write_bytes(diag)
    # and the previous file (2026-11-07; firmware before that answers with the current one again)
    old = get("/api/debug/diagnostic-log?part=old", 60)
    if old and old != diag:
        (data / "log" / "diagnostic_log.old.txt").write_bytes(old)
    else:
        (data / "log" / "diagnostic_log.old.txt").unlink(missing_ok=True)

    # Event pictures: Data/images (the picture library, since 2026-10-21; the move of the
    # older per-event photos was taken out of the firmware 2026-11-17).
    emptied(data / "images")
    images = json.loads(get("/api/images/list", 60).decode("utf-8"))
    for im in images:
        (data / "images" / (im["id"] + ".bmp")).write_bytes(get("/api/images/get?id=" + im["id"], 30))
    print("pictures copied:", len(images))

    # The clock's own pictures the family replaced: Data/custom.
    emptied(data / "custom")
    try:
        builtin = json.loads(get("/api/builtin/list", 60).decode("utf-8"))
        custom = [p["name"] for p in builtin["pictures"] if p.get("custom")]
        if custom:
            for name in custom:
                (data / "custom" / (name + ".bmp")).write_bytes(get("/api/builtin/get?which=custom&name=" + name, 60))
        print("replaced built-in pictures copied:", len(custom))
    except (urllib.error.HTTPError, ValueError, KeyError):   # older firmware (unknown path -> redirected to the page)
        print("replaced built-in pictures: none (older firmware)")

    nameless = [e.get("id") for e in evts if not str(e.get("name", "")).strip()]
    print("synced: ssid=%s events=%d nameless=%s debugTimeOffset was %s -> 0, diag %d bytes, old copies in %s"
          % (cfg.get("wifiSSID"), len(evts), nameless or "none", removed_offset, len(diag), bdir))


def post():
    print("note:", note("Data flash complete"))
    st = json.loads(get("/api/status").decode("utf-8"))
    print("up: firmware=%s ssid=%s synced=%s todayHDay=%s todayHMonth=%s"
          % (st.get("firmware"), st.get("ssid"), st.get("synced"), st.get("todayHDay"), st.get("todayHMonth")))


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["pre", "post"])
    ap.add_argument("--clock", help="the clock's address, tried after jewishclock.local")
    args = ap.parse_args()
    addresses = ["jewishclock.local"] + ([args.clock] if args.clock else [])
    if not find_clock(addresses, 90 if args.mode == "pre" else 240):
        sys.exit("clock not reachable at " + ", ".join(addresses))
    pre() if args.mode == "pre" else post()


if __name__ == "__main__":
    main()
