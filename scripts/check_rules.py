# Rules the compiler can't enforce, checked before every build (pioarduino.ini
# extra_scripts, and sim/build.py / sim/build_clock.py). A hit stops the build.
#
# 1. No millis() / micros() outside include/Uptime.h (2026-11-14): they are
#    32-bit and start over every 49.7 days / 71 minutes; the clock runs for
#    years. Uptime::ms() / us() and Timer are 64-bit (292,000 years).
# 1b. No restart except through include/Restart.h (2026-11-21): it writes the log's waiting
#    lines first - a failed trial's rollback lost its own line once.
# 1c. No C-library character classes (2026-11-21): StringTools::ascii*() instead.
# 2. Tests never keep their own copy of the clock's words (the user, 2026-11-20):
#    a literal in a test script that matches a text of include/Texts.h - the whole
#    text with anything where the clock fills in ({}, or a LOG text's %s/%d...), or a fixed piece of 12+
#    characters with a space (or one of its sentences / clauses) - stops the build: the test asks the clock instead
#    (sim/clock_facts.py screen_text(), /api/texts). A test's own descriptions
#    (docstrings, the label of a check / log line) don't count, nor a line whose comment says
#    "copy-ok: <why>" (a tool reading an older version, which has no list to ask). Copies of texts
#    NOT on the list are found by the compliance run (sim/scrambled_build.py).
#    The same for the web page's own messages (Data/index.html PAGE_TEXTS - the tests read
#    them from the page: sim/web_ui.py PW()).
#
# Runs as a PlatformIO pre-script (Import("env") present) or on its own:
#   python scripts/check_rules.py      -> exit 1 and the hits on a violation.
import os
import re
import sys

RULES = [
    (re.compile(r"\b(millis|micros)\s*\(\s*\)"),
     "use Uptime::ms() / Uptime::us() or a Timer (include/Uptime.h) - millis()/micros() are 32-bit and wrap",
     {"include/Uptime.h"}),
    (re.compile(r"\bESP\.restart\s*\(|\besp_restart\s*\(|\besp_ota_mark_app_invalid_rollback_and_reboot\s*\("),
     "restart through Restart::now() / Restart::rollback() (include/Restart.h): the waiting log lines are written first",
     {"include/Restart.h"}),
    (re.compile(r"(?<![\w:.])(isdigit|isalpha|isalnum|isspace|isupper|islower|isxdigit|ispunct|toupper|tolower)\s*\("),
     "use StringTools::ascii*() - the C library's character classes are undefined for a Hebrew letter's (negative) bytes",
     set()),
]


def strip_comments(line, in_block):
    out, i = "", 0
    while i < len(line):
        if in_block:
            j = line.find("*/", i)
            if j < 0:
                return out, True
            i, in_block = j + 2, False
        elif line.startswith("//", i):
            break
        elif line.startswith("/*", i):
            in_block, i = True, i + 2
        elif line[i] == '"':   # skip string literals
            j = i + 1
            while j < len(line) and line[j] != '"':
                j += 2 if line[j] == "\\" else 1
            out += '""'
            i = j + 1
        else:
            out += line[i]
            i += 1
    return out, in_block


def check(root):
    hits = []
    for folder in ("src", "include"):
        for dirpath, _, files in os.walk(os.path.join(root, folder)):
            for name in files:
                if not name.endswith((".cpp", ".h", ".c", ".hpp")):
                    continue
                path = os.path.join(dirpath, name)
                rel = os.path.relpath(path, root).replace("\\", "/")
                in_block = False
                with open(path, encoding="utf-8", errors="replace") as f:
                    for n, line in enumerate(f, 1):
                        code, in_block = strip_comments(line, in_block)
                        for rx, why, allowed in RULES:
                            if rel not in allowed and rx.search(code):
                                hits.append("%s:%d: %s\n    %s" % (rel, n, why, line.strip()))
    return hits


TEST_SCRIPTS = ("sim", "utils/ap_tests.py")
TEXT_ENTRY = re.compile(r'X\((\w+),\s*"((?:[^"\\]|\\.)*)"\)')
PY_LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"|' + r"'((?:[^'\\\n]|\\.)*)'")
# a literal right after one of these is a test's own description of a check, not a comparison
LABEL_BEFORE = re.compile(r"(?:\bcheck|\.expect|\.expect_absent|\.stage|\blog|\bprint)\(\s*$")


# where the clock fills something in: {} (Texts::fill) or a printf specifier (a LOG text) - as
# sim/clock_facts.py _FILLED; a piece between two of them is a fixed part a test may not copy either
FILLED = re.compile(r"\{\}|%[-+ #0]*\d*(?:\.\d+)?(?:hh|h|ll|l|z|j|t|L)?[diouxXeEfgGcsp]")
# a fixed piece's own sentences and clauses: a test copying just "Requesting in background" of
# "Fetch conditions met. Requesting in background: %s" is a copy too (2026-11-22, test_midnight)
CLAUSE = re.compile(r"[.:;!?]\s+|\s+-\s+|\s*[()]\s*")
PAGE_BLOCK = re.compile(r"const PAGE_TEXTS = \{(.*?)\n\};", re.S)
PAGE_ENTRY = re.compile(r"""^\s*(\w+):\s*(?:'((?:[^'\\]|\\.)*)'|"((?:[^"\\]|\\.)*)"),?\s*$""", re.M)


def text_rules(root):
    """[(regex, (whose, how to ask))] for the texts of include/Texts.h and the page's PAGE_TEXTS (rule 2)."""
    rules = []
    entries = []
    path = os.path.join(root, "include", "Texts.h")
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            entries += [("the clock's", "W().has(%r, ...)" % n, t) for n, t in TEXT_ENTRY.findall(f.read())]
    path = os.path.join(root, "Data", "index.html")
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            m = PAGE_BLOCK.search(f.read())
        if m:
            entries += [("the page's", "PW().has(%r, ...)" % n, a or b) for n, a, b in PAGE_ENTRY.findall(m.group(1))]
    for whose, ask, text in entries:
        name = (whose, ask)
        pieces = FILLED.split(text.replace("%%", "%"))
        if len("".join(pieces).strip()) >= 12:   # short ones ("answer {}") would match any sentence
            rules.append((re.compile(".+".join(re.escape(p) for p in pieces)), name))
        for piece in pieces:
            for part in {piece.strip()} | {c.strip() for c in CLAUSE.split(piece)}:
                if len(part) >= 12 and " " in part:
                    rules.append((re.compile(re.escape(part)), name))
    return rules


def check_test_copies(root):
    rules = text_rules(root)
    files = []
    for entry in TEST_SCRIPTS:
        p = os.path.join(root, entry)
        if os.path.isdir(p):
            files += [os.path.join(p, f) for f in sorted(os.listdir(p)) if f.endswith(".py")]
        elif os.path.exists(p):
            files.append(p)
    hits = []
    for path in files:
        rel = os.path.relpath(path, root).replace("\\", "/")
        in_doc = False
        with open(path, encoding="utf-8", errors="replace") as f:
            for n, line in enumerate(f, 1):
                if line.count('"""') % 2 == 1:   # a docstring starts or ends here: a description
                    in_doc = not in_doc
                    continue
                if line.count('"""'):   # a one-line docstring
                    continue
                if in_doc or line.lstrip().startswith("#"):
                    continue
                spans = [m.span() for m in PY_LITERAL.finditer(line)]
                hashes = [i for i, ch in enumerate(line) if ch == "#" and not any(a <= i < b for a, b in spans)]
                code_end = hashes[0] if hashes else len(line)   # a comment after the code: a description too
                if "copy-ok:" in line[code_end:]:   # a deliberate copy, its reason in the comment (a tool for an older clock)
                    continue
                for m in PY_LITERAL.finditer(line):
                    if m.start() >= code_end or LABEL_BEFORE.search(line[:m.start()]):
                        continue
                    s = m.group(1) if m.group(1) is not None else m.group(2)
                    for rx, (whose, ask) in rules:
                        if rx.search(s):
                            hits.append("%s:%d: %s words copied (%r) - ask: %s\n    %s"
                                        % (rel, n, whose, s[:50], ask, line.strip()))
                            break
    return hits


def main(root):
    hits = check(root) + check_test_copies(root)
    for h in hits:
        print("check_rules: " + h)
    return 1 if hits else 0


try:
    Import("env")  # noqa: F821 - PlatformIO's SCons
    _root = env.subst("$PROJECT_DIR")  # noqa: F821
    if main(_root):
        sys.exit("check_rules: build stopped (see above)")
except NameError:
    if __name__ == "__main__":
        sys.exit(main(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
