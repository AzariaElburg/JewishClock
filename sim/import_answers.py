"""
A recording made by the page (Settings > Clock software > Web answers > Record - the
family's clock, where there is only a browser) into the folder sim/record_clock_web.py
makes: reference/clock-<date>/web/index.json + bodies/. sim/compare_web.py then checks
the simulator against it (a browser's recording: bodies unpacked, headers as a set).

    C:/pio3/jc_clean/venv/Scripts/python sim/import_answers.py RECORDING.json [--out DIR]
"""
import argparse
import base64
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from record_clock_web import safe_name  # noqa: E402 - the same file names as the PC's recording


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--out", help="default: reference/clock-<the recording's date>/web")
    a = ap.parse_args()
    rec = json.loads(Path(a.file).read_text(encoding="utf-8"))
    if rec.get("via") != "browser" or "requests" not in rec:
        sys.exit("not a recording from the clock's page (Settings > Clock software > Web answers)")
    out = Path(a.out) if a.out else ROOT / "reference" / ("clock-" + rec["recorded"][:10]) / "web"
    (out / "bodies").mkdir(parents=True, exist_ok=True)
    index = []
    for r in rec["requests"]:
        body = base64.b64decode(r.pop("body", "") or "")
        r["via"] = "browser"
        if "error" not in r and r["accept"] != "browser+If-None-Match":
            name = ("POST_%s.bin" % safe_name(r["path"])) if r["method"] == "POST" else ("GET_%s_browser.bin" % safe_name(r["path"]))
            (out / "bodies" / name).write_bytes(body)
        index.append(r)
    (out / "index.json").write_text(json.dumps(index, ensure_ascii=False, indent=1), encoding="utf-8")
    (out / "recording.json").write_text(json.dumps({k: v for k, v in rec.items() if k != "requests"}, ensure_ascii=False, indent=1),
                                        encoding="utf-8")
    errors = sum(1 for r in index if "error" in r)
    print("%d answers -> %s%s" % (len(index), out, " (%d failed in the browser)" % errors if errors else ""))


if __name__ == "__main__":
    main()
