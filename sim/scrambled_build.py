"""
The compliance build (the user, 2026-11-20): every message-like text in the clock's code changed,
then the whole test suite run on it. A test that asks the clock for its words (/api/texts, the list
in include/Texts.h - scrambled the same way here) still passes; a test that keeps its own copy of a
text - listed or not - fails, and so shows where it is. Nothing depends on a search finding them.

    C:/pio3/jc_clean/venv/Scripts/python sim/scrambled_build.py            # builds jcclock.exe + jcsim.exe from it
    C:/pio3/jc_clean/venv/Scripts/python sim/run_all_tests.py --from 2.1.2 --skip build-jcsim build-clock dates simulator-validity
    C:/pio3/jc_clean/venv/Scripts/python sim/scrambled_build.py --show     # what it would change (no build)

What is changed: every C/C++ string literal in src/ and include/ that reads like a message - ASCII
letters and a space - by shifting each letter one on (a->b, z->a, keeping case), the same length
so screen texts still fit their window. Kept as they are: printf specifiers (%d, %+lld, %s ...),
the {} the clock fills in, escapes (\\n, \\"), web addresses, file paths, Hebrew and every other
non-ASCII letter, #include lines - and the literals in sim/scramble_keep.txt: texts the clock
compares with data from outside (a Hebcal title, a header), each with its reason.
The copy lives in C:/pio3/jc_clean/scrambled/ (never the project folder); the sim/ stand-ins are not
changed (their "[sim]" lines are the simulator's, not the clock's).
"""
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TREE = Path("C:/pio3/jc_clean/scrambled/tree")
KEEP_FILE = ROOT / "sim" / "scramble_keep.txt"
PY = sys.executable

LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
SPEC = re.compile(r"%[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z|j|t|L)?[diouxXeEfgGcspn%]|\{\}|\\x[0-9a-fA-F]{2}|\\.")


def keep_list():
    keep = set()
    if KEEP_FILE.exists():
        for line in KEEP_FILE.read_text(encoding="utf-8").splitlines():
            line = line.split("  #", 1)[0].rstrip()
            if line and not line.startswith("#"):
                keep.add(line)
    return keep


def message_like(s):
    if " " not in s or not re.search(r"[A-Za-z]{2}", s):
        return False
    if "://" in s or s.startswith("/") or re.search(r"\.(json|txt|bmp|html|csv|bin|jcupdate|js|css)\b", s):
        return False
    if re.match(r'\s*[\[{]\s*\\?"', s):   # JSON the clock sends ({"networks":[], "status":"scanning"}) - data, not words
        return False
    return True


# HTML markup is the page's machinery, not its words (2026-11-20: a scrambled <meta http-equiv='refresh'>
# left the clock's "initializing" page standing for good): a tag, a tag the literal leaves open
# ("<a href='http://" + host ...), and the end of one opened in the literal before ("/'>").
TAG = re.compile(r"<[A-Za-z/!][^>]*>?")
TAG_END = re.compile(r"^[^<>]*['\"/]>")


def shift(s):
    """Each ASCII letter one on, outside specifiers/escapes/{} and HTML markup."""
    keep = [m.span() for m in SPEC.finditer(s)] + [m.span() for m in TAG.finditer(s)]
    m = TAG_END.match(s)
    if m:
        keep.append(m.span())
    out, i = [], 0
    for a, b in sorted(keep):
        if b <= i:
            continue
        a = max(a, i)
        out.append(_shift_plain(s[i:a]))
        out.append(s[a:b])
        i = b
    out.append(_shift_plain(s[i:]))
    return "".join(out)


def _shift_plain(t):
    r = []
    for ch in t:
        if "a" <= ch <= "z":
            r.append(chr((ord(ch) - 97 + 1) % 26 + 97))
        elif "A" <= ch <= "Z":
            r.append(chr((ord(ch) - 65 + 1) % 26 + 65))
        else:
            r.append(ch)
    return "".join(r)


RAW_START = re.compile(r'R"([^(\s]*)\(')


def scramble_file(text, keep, changed):
    lines = []
    raw_end = None   # inside a raw string R"tag( ... )tag" - left whole (a web page built into the firmware)
    for line in text.split("\n"):
        if raw_end:
            lines.append(line)
            if raw_end in line:
                raw_end = None
            continue
        m = RAW_START.search(line)
        if m and (')' + m.group(1) + '"') not in line[m.end():]:
            raw_end = ')' + m.group(1) + '"'
            lines.append(line)
            continue
        if line.lstrip().startswith("#include") or m:
            lines.append(line)
            continue

        def repl(m):
            s = m.group(1)
            if s in keep or not message_like(s):
                return m.group(0)
            changed.append(s)
            return '"' + shift(s) + '"'
        lines.append(LITERAL.sub(repl, line))
    return "\n".join(lines)


def main():
    show = "--show" in sys.argv
    keep = keep_list()
    if TREE.exists():
        shutil.rmtree(TREE)
    changed = []
    for sub in ("src", "include"):
        for p in (ROOT / sub).rglob("*"):
            if p.is_dir():
                continue
            dest = TREE / sub / p.relative_to(ROOT / sub)
            dest.parent.mkdir(parents=True, exist_ok=True)
            if p.suffix in (".cpp", ".h"):
                dest.write_text(scramble_file(p.read_text(encoding="utf-8"), keep, changed), encoding="utf-8", newline="")
            else:
                shutil.copy2(p, dest)
    print("%d message-like texts scrambled (%d kept by sim/scramble_keep.txt)" % (len(changed), len(keep)))
    if show:
        for s in changed:
            print("  %r -> %r" % (s, shift(s)))
        return 0
    # jcclock from the scrambled tree, put where the tests take it; jcsim the same
    r = subprocess.run([PY, str(ROOT / "sim" / "build_clock.py"), "--tree", str(TREE), "--scrambled"])
    if r.returncode:
        return r.returncode
    r = subprocess.run([PY, str(ROOT / "sim" / "build.py"), "--tree", str(TREE)])
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
