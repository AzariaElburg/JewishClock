"""
Writes the clock software version into the start guide and the manual
("For clock software version ..."), fixed in the files - each describes one
version and says so, however and whenever it is viewed or printed (the user,
2026-11-05).

    python utils/stamp_docs.py            # the version just built (src/Version.cpp)
    python utils/stamp_docs.py 2.1.0      # a release: stamp, commit, tag v2.1.0, build

Run it whenever either file changes. During development: build first, then
stamp with that build's version and commit Data/ (Data/ files don't change
the firmware's version, see scripts/gen_version.py). For a release the name
is known ahead, so stamp before the tag.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOCS = [ROOT / "Data" / "start.html", ROOT / "Data" / "manual.html"]
STAMP = re.compile(r'(class="fwver">)[^<]*(<)')


def built_version():
    src = (ROOT / "src" / "Version.cpp").read_text(encoding="utf-8")
    return re.search(r'FIRMWARE_VERSION\[\] = "([^"]+)"', src).group(1)


def main():
    version = sys.argv[1] if len(sys.argv) > 1 else built_version()
    if version.endswith("-dirty"):
        sys.exit("%s: built with uncommitted firmware changes - commit, build, then stamp." % version)
    for f in DOCS:
        text = f.read_text(encoding="utf-8")
        new, n = STAMP.subn(r"\g<1>%s\g<2>" % version, text)
        if not n:
            sys.exit("%s: no fwver mark" % f)
        if new != text:
            f.write_bytes(new.encode("utf-8"))
        print("%s: %d x %s" % (f.name, n, version))


if __name__ == "__main__":
    main()
