"""
Copy-paste finder for the clock's code (src/, include/, Data/*.html scripts):
runs of at least N lines that appear twice or more, after normalising what
copies usually differ in (spaces, names of variables are KEPT - a renamed copy
is still worth a look, so identifiers are replaced by "x" with --loose).

    C:/pio3/jc_clean/venv/Scripts/python utils/find_copies.py [--lines 6] [--loose]

Started 2026-11-14 (the user: "reuse rather than copies"). Same idea as PMD's
CPD; written here because nothing else may be installed. Blank lines,
comments and lone braces don't count.
"""
import argparse
import re
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def lines_of(path):
    out = []
    text = path.read_text(encoding="utf-8", errors="replace")
    if path.suffix == ".html":
        # only the scripts
        for m in re.finditer(r"<script(?![^>]*src)[^>]*>(.*?)</script>", text, re.S):
            start = text[:m.start(1)].count("\n") + 1
            for i, ln in enumerate(m.group(1).split("\n")):
                out.append((start + i, ln))
    else:
        out = list(enumerate(text.split("\n"), 1))
    return out


def norm(line, loose):
    s = re.sub(r"//.*$", "", line).strip()
    if not s or s in ("{", "}", "};", "});", ")", "]", "});"):
        return None
    s = re.sub(r"\s+", " ", s)
    if loose:
        s = re.sub(r"\b[A-Za-z_]\w*\b", "x", s)
        s = re.sub(r"\b\d+\b", "0", s)
        s = re.sub(r'"[^"]*"', '""', s)
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lines", type=int, default=6)
    ap.add_argument("--loose", action="store_true")
    a = ap.parse_args()
    files = sorted((ROOT / "src").glob("*.cpp")) + sorted((ROOT / "include").glob("*.h")) + sorted((ROOT / "Data").glob("*.html"))
    seqs = []   # (file, [(lineno, normalised)])
    for f in files:
        if f.name in ("Font.h", "Version.cpp"):
            continue
        seq = [(n, norm(l, a.loose)) for n, l in lines_of(f)]
        seqs.append((f, [(n, s) for n, s in seq if s]))
    where = defaultdict(list)
    N = a.lines
    for f, seq in seqs:
        for i in range(len(seq) - N + 1):
            key = "\n".join(s for _, s in seq[i:i + N])
            where[key].append((f.relative_to(ROOT).as_posix(), seq[i][0], seq[i + N - 1][0]))
    # merge overlapping windows into regions
    seen = set()
    groups = []
    for key, locs in where.items():
        if len(locs) < 2:
            continue
        sig = tuple(sorted((p, s // 10) for p, s, _ in locs))
        if sig in seen:
            continue
        seen.add(sig)
        groups.append(locs)
    # report each pair of places once, with the longest run
    pairs = defaultdict(int)
    for locs in groups:
        for i in range(len(locs)):
            for j in range(i + 1, len(locs)):
                a_, b_ = locs[i], locs[j]
                k = (a_[0], a_[1] // 25, b_[0], b_[1] // 25)
                pairs[k] = max(pairs[k], 1)
    shown = set()
    for locs in sorted(groups, key=lambda l: (l[0][0], l[0][1])):
        k = tuple((p, s // 25) for p, s, _ in locs)
        if k in shown:
            continue
        shown.add(k)
        print(" <-> ".join("%s:%d" % (p, s) for p, s, _ in locs))


if __name__ == "__main__":
    main()
