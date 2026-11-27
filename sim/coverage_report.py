"""
Code coverage report (2026-11-16): which of the clock's code the tests run - the C++ (the
simulators' coverage builds) and the page's JavaScript - and which tests add nothing of their own.

    1. python sim/build.py --coverage; python sim/build_clock.py --coverage   (llvm-mingw, see build.py)
    2. python sim/run_all_tests.py --from 2.1.2 --skip build-firmware package build-jcsim build-clock --coverage DIR
    3. python sim/coverage_report.py DIR
    4. python sim/build.py; python sim/build_clock.py        (the normal builds back)

DIR/<test>/*.profraw (one folder per display test, resilience drill, web-ui section) and DIR/js/
(web_ui.py --js-coverage). Writes DIR/report.txt (the summary) and next to it:
  cpp_never_run.txt  - the clock's functions and code blocks no test ran, file:line and the code
  js_never_run.txt   - the same for the page's scripts
  per_test.txt       - each test: lines it runs, and lines no other test runs ("own")
  by_tests.txt       - each function: how many tests run it
Counts say what ran, not what was checked: a test with no "own" lines may still check something
the others don't.
"""
import collections
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LLVM = Path(os.environ.get("LLVM_MINGW", "C:/pio3/llvm-mingw/llvm-mingw-20261104-ucrt-x86_64")) / "bin"
BUILD = Path("C:/pio3/jc_clean/sim/build")
OBJECTS = [BUILD / "jcclock.exe", BUILD / "jcsim.exe",
           # tests with programs of their own, built with counting in a measuring run (2026-11-18 -
           # before, their lines counted as never run although these tests check them)
           BUILD / "test_brightness_cov.exe", BUILD / "test_own_calculations_vs_hebcal_cov.exe"]
# Only the clock's own code: src/ and include/ (not the simulator's stand-ins, libraries, the compiler's)
IGNORE = r"([/\\]sim[/\\]|ArduinoJson|libdeps|llvm-mingw|[/\\]lib[/\\]|Version\.cpp)"


def run(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, encoding="utf-8", errors="replace", **kw)
    if r.returncode != 0:
        raise RuntimeError("%s: %s" % (cmd[0], r.stderr[:500]))
    return r.stdout


SKIPPED = set()   # count files a stopped simulator left half-written (merged without them, said in the report)


def profdata(raws, out):
    # The list in a file (-f): 734 count files made a command line too long for Windows (2026-11-18)
    lst = Path(str(out) + ".inputs")
    lst.write_text("".join(str(x) + chr(10) for x in raws), encoding="utf-8")
    r = subprocess.run([str(LLVM / "llvm-profdata.exe"), "merge", "-sparse", "-failure-mode=warn", "-f", str(lst), "-o", str(out)],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    for m in re.finditer(r"warning: (\S+\.profraw): ", r.stderr):
        SKIPPED.add(m.group(1))
    if r.returncode != 0:
        raise RuntimeError("llvm-profdata: " + r.stderr[:500])
    return out


def lcov(prof):
    """{file: {"lines": {line: count}, "fns": {name: (line, count)}}} from llvm-cov's lcov export."""
    objs = [OBJECTS[0]] + sum([["-object", o] for o in OBJECTS[1:] if o.exists()], [])
    text = run([LLVM / "llvm-cov.exe", "export", "-format=lcov", "-instr-profile=" + str(prof),
                "-ignore-filename-regex=" + IGNORE] + objs)
    files, cur = {}, None
    fn_line = {}
    for line in text.splitlines():
        if line.startswith("SF:"):
            cur = files.setdefault(str(Path(line[3:]).resolve()), {"lines": {}, "fns": {}})
            fn_line = {}
        elif line.startswith("DA:"):
            n, c = line[3:].split(",")[:2]
            cur["lines"][int(n)] = cur["lines"].get(int(n), 0) + int(c)
        elif line.startswith("FN:"):
            n, name = line[3:].split(",", 1)
            fn_line[name] = int(n)
        elif line.startswith("FNDA:"):
            c, name = line[5:].split(",", 1)
            old = cur["fns"].get(name, (fn_line.get(name, 0), 0))
            cur["fns"][name] = (old[0], old[1] + int(c))
    return files


def demangle(names):
    names = list(names)
    if not names:
        return {}
    out = subprocess.run([str(LLVM / "llvm-cxxfilt.exe")], input="\n".join(names), capture_output=True, text=True).stdout.splitlines()
    return dict(zip(names, out)) if len(out) == len(names) else {n: n for n in names}


def rel(f):
    try:
        return str(Path(f).relative_to(ROOT)).replace("\\", "/")
    except ValueError:
        return f


def blocks(lines_never):
    """Consecutive line numbers -> (first, last) runs."""
    out, start, prev = [], None, None
    for n in sorted(lines_never):
        if start is None:
            start = prev = n
        elif n == prev + 1:
            prev = n
        else:
            out.append((start, prev))
            start = prev = n
    if start is not None:
        out.append((start, prev))
    return out


def source_lines(f, cache={}):
    if f not in cache:
        try:
            cache[f] = Path(f).read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            cache[f] = []
    return cache[f]


# ── the page's JavaScript (web_ui.py --js-coverage) ──────────────────────────
def js_counts(jsdir):
    """{(url, sha): [count per character]} summed over every section, and per section the set of
    (sha, line) it ran."""
    total, per_section = {}, {}
    for f in sorted(jsdir.glob("*.json")):
        section = "web-ui." + f.stem
        ran = per_section.setdefault(section, set())
        for row in json.loads(f.read_text(encoding="utf-8")):
            src = (jsdir / "src" / (row["sha"] + ".js"))
            if not src.exists():
                continue
            text = src.read_text(encoding="utf-8")
            if row["url"].endswith(".min.js"):
                continue   # a library (sortable.min.js), not the page's own code
            # one script = one source, whichever lane's clock (port) served it - counted once, summed
            path = re.sub(r"^https?://[^/]+", "", row["url"]) or "/"
            key = (path, row["sha"])
            arr = total.setdefault(key, [0] * len(text))
            mine = [0] * len(text)
            for fn in row["functions"]:
                for r in fn["ranges"]:   # outer first, inner after: the inner count wins (V8's block coverage)
                    for i in range(r["startOffset"], min(r["endOffset"], len(text))):
                        mine[i] = r["count"]
            for i, c in enumerate(mine):
                arr[i] += c
            starts = [0] + [m.end() for m in re.finditer("\n", text)]
            for ln, st in enumerate(starts):
                en = starts[ln + 1] if ln + 1 < len(starts) else len(text)
                if any(mine[i] for i in range(st, en) if not text[i].isspace()):
                    ran.add((row["sha"], ln + 1))
    return total, per_section


def main():
    d = Path(sys.argv[1])
    tests = sorted(p for p in d.iterdir() if p.is_dir() and p.name not in ("js",) and list(p.glob("*.profraw")))
    print("%d tests with counts" % len(tests))
    pd = d / "profdata"
    pd.mkdir(exist_ok=True)
    per = {}
    for t in tests:
        try:
            per[t.name] = lcov(profdata(list(t.glob("*.profraw")), pd / (t.name + ".profdata")))
        except RuntimeError as e:
            print("  %s: %s" % (t.name, e))
    allraw = [str(p) for t in tests for p in t.glob("*.profraw")]
    total = lcov(profdata(allraw, pd / "all.profdata"))

    # Never run (C++)
    lines_all = sum(len(v["lines"]) for v in total.values())
    lines_hit = sum(1 for v in total.values() for c in v["lines"].values() if c)
    fns_all = sum(len(v["fns"]) for v in total.values())
    never_fns = [(f, ln, name) for f, v in total.items() for name, (ln, c) in v["fns"].items() if c == 0]
    dm = demangle({n for _, _, n in never_fns} | {n for v in total.values() for n in v["fns"]})
    out = []
    n_blocks = 0
    for f in sorted(total, key=rel):
        never = [n for n, c in total[f]["lines"].items() if c == 0]
        if not never:
            continue
        src = source_lines(f)
        bl = blocks(never)
        n_blocks += len(bl)
        out.append("== %s: %d of %d lines never run, %d blocks" % (rel(f), len(never), len(total[f]["lines"]), len(bl)))
        for a, b in bl:
            first = src[a - 1].strip() if 0 < a <= len(src) else ""
            out.append("  %d-%d  %s" % (a, b, first[:110]) if b > a else "  %d  %s" % (a, first[:110]))
    (d / "cpp_never_run.txt").write_text("\n".join(out) + "\n", encoding="utf-8")

    # Per test: lines it runs, and its own (no other test runs them)
    who = collections.defaultdict(set)
    ran = {}
    for t, files in per.items():
        s = {(f, n) for f, v in files.items() for n, c in v["lines"].items() if c}
        ran[t] = s
        for k in s:
            who[k].add(t)
    # Function -> number of tests
    fn_tests = collections.Counter()
    for t, files in per.items():
        for f, v in files.items():
            for name, (ln, c) in v["fns"].items():
                if c:
                    fn_tests[(rel(f), ln, dm.get(name, name))] += 1

    # The page's JavaScript
    js_lines = js_hit = 0
    js_out = []
    jsdir = d / "js"
    js_per = {}
    if jsdir.exists():
        jtotal, js_per = js_counts(jsdir)
        for (url, sha), arr in sorted(jtotal.items()):
            text = (jsdir / "src" / (sha + ".js")).read_text(encoding="utf-8")
            starts = [0] + [m.end() for m in re.finditer("\n", text)]
            never = []
            for ln, st in enumerate(starts):
                en = starts[ln + 1] if ln + 1 < len(starts) else len(text)
                code = [i for i in range(st, en) if not text[i].isspace()]
                if not code:
                    continue
                js_lines += 1
                if any(arr[i] for i in code):
                    js_hit += 1
                else:
                    never.append(ln + 1)
            src = text.splitlines()
            bl = blocks(never)
            js_out.append("== %s (script %s): %d lines never run, %d blocks" % (url, sha[:8], len(never), len(bl)))
            for a, b in bl:
                first = src[a - 1].strip() if 0 < a <= len(src) else ""
                js_out.append("  %d-%d  %s" % (a, b, first[:110]) if b > a else "  %d  %s" % (a, first[:110]))
        (d / "js_never_run.txt").write_text("\n".join(js_out) + "\n", encoding="utf-8")
        jwho = collections.defaultdict(set)
        for t, s in js_per.items():
            for k in s:
                jwho[k].add(t)

    rows = []
    for t in sorted(set(ran) | set(js_per)):
        own = sum(1 for k in ran.get(t, ()) if len(who[k]) == 1)
        jown = sum(1 for k in js_per.get(t, ()) if len(jwho[k]) == 1) if js_per else 0
        rows.append((own + jown, t, len(ran.get(t, ())), own, len(js_per.get(t, ())), jown))
    rows.sort()
    pt = ["%-45s %8s %6s %8s %6s" % ("test", "C++ ran", "own", "JS ran", "own")]
    pt += ["%-45s %8d %6d %8d %6d" % (t, a, b, c, e) for _, t, a, b, c, e in rows]
    (d / "per_test.txt").write_text("\n".join(pt) + "\n", encoding="utf-8")
    bt = ["%3d tests  %s:%d  %s" % (n, f, ln, name[:100]) for (f, ln, name), n in fn_tests.most_common()]
    (d / "by_tests.txt").write_text("\n".join(bt) + "\n", encoding="utf-8")

    hist = collections.Counter()
    for k, s in who.items():
        n = len(s)
        hist["1" if n == 1 else "2-5" if n <= 5 else "6-20" if n <= 20 else "21-50" if n <= 50 else "51+"] += 1
    no_own = [t for o, t, *_ in rows if o == 0]
    rep = [
        "Code coverage, %s - %d tests" % (d.name, len(set(ran) | set(js_per))),
        "",
        "Clock's C++ (src/, include/), all tests together:",
        "  lines run:     %d of %d (%.1f%%), never run %d, in %d blocks" % (lines_hit, lines_all, 100.0 * lines_hit / max(lines_all, 1), lines_all - lines_hit, n_blocks),
        "  functions run: %d of %d, never run %d" % (fns_all - len(never_fns), fns_all, len(never_fns)),
        "Page JavaScript: lines run %d of %d (%.1f%%), never run %d" % (js_hit, js_lines, 100.0 * js_hit / max(js_lines, 1), js_lines - js_hit) if js_lines else "Page JavaScript: no counts",
        "",
        "C++ lines by how many tests run them: " + ", ".join("%s: %d" % (k, hist[k]) for k in ("1", "2-5", "6-20", "21-50", "51+")),
        "Tests that run no line no other test runs (C++ and JS): %d - %s" % (len(no_own), ", ".join(no_own)),
        "",
        "Count files left half-written by a stopped simulator, merged without: %d of %d" % (len(SKIPPED), len(allraw)),
        "Details: cpp_never_run.txt, js_never_run.txt, per_test.txt, by_tests.txt",
    ]
    (d / "report.txt").write_text("\n".join(rep) + "\n", encoding="utf-8")
    print("\n".join(rep))


if __name__ == "__main__":
    main()
