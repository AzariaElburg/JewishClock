"""
The clock's own data paths - the files and folders an update never writes or
removes - read from the firmware's own list (include/Config.h,
isClockDataPath()), so the update package script and the firmware can't
disagree (2026-11-08: the script kept a copy).

    from clock_data_paths import is_clock_data_path
    is_clock_data_path("/config.json")  -> True
"""
import re
from pathlib import Path

CONFIG_H = Path(__file__).resolve().parent.parent / "include" / "Config.h"


def _lists():
    src = CONFIG_H.read_text(encoding="utf-8")
    defines = dict(re.findall(r'^#define\s+(\w+)\s+"([^"]*)"', src, re.M))
    body = src[src.index("inline bool isClockDataPath("):]
    body = body[:body.index("\n}\n")]

    def items(block):
        out = []
        for tok in re.findall(r'"[^"]*"|\b[A-Z_][A-Z0-9_]+\b', block):
            if tok.startswith('"'):
                out.append(tok[1:-1])
            elif tok in defines:
                out.append(defines[tok])
        return out

    files = items(body[body.index("files[] = {"):body.index("};")])
    dirs = items(re.search(r"for \(const char\* d : \{([^}]*)\}", body).group(1))
    if not files or not dirs:
        raise RuntimeError("isClockDataPath() in Config.h no longer reads as expected - update utils/clock_data_paths.py")
    return files, dirs


FILES, DIRS = _lists()


def is_clock_data_path(p):
    """As Config.h isClockDataPath(): one of its files (or that file + ".tmp"), or inside one of its folders."""
    p = "/" + p.lstrip("/")
    low = p.lower()
    for f in FILES:
        if low == f.lower() or low == (f + ".tmp").lower():
            return True
    return any(low == d.lower() or low.startswith(d.lower() + "/") for d in DIRS)


if __name__ == "__main__":
    print("files:", FILES)
    print("folders:", DIRS)
