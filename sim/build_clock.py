"""
Builds the FULL simulator, jcclock.exe: every src/*.cpp of the clock unchanged
(main.cpp's setup()/loop() included, NTPManager.cpp asking real time servers),
on PC stand-ins for the hardware only (sim/clock_stubs, then the screen
simulator's sim/stubs). Same compiler as sim/build.py (Zig).

    python sim/build_clock.py

-> C:/pio3/jc_clean/sim/build/jcclock.exe, and a copy as
   C:/pio3/jc_clean/sim/versions/<version>/jcclock.exe - the build an update to
   that version runs (sim/sim_clock_main.cpp). The version is src/Version.cpp's,
   i.e. the last firmware build's: build the firmware first (pio run), so the
   simulator and the firmware.bin of an update package are the same version.
"""
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build import compiler, link, ARDUINOJSON, OUT, ROOT, SIM  # noqa: E402

VERSIONS = OUT.parent / "versions"
NOT_ON_PC = {"UpdateSignature"}   # the clock's files a stand-in replaces on the PC (sim_signature.cpp)
SIM_SOURCES = ["sim_clock_main", "sim_platform", "sim_web", "sim_wifi", "sim_ota", "sim_heap", "sim_ntp", "sim_net", "sim_win", "sim_coverage"]


FIRMWARE_PATHS = ["src", "include", "lib", "scripts", "pioarduino.ini"]   # as scripts/gen_version.py


TREE = ROOT   # the clock's code (src/, include/) - --tree: another checkout, e.g. an older version


SCRAMBLED = "--scrambled" in sys.argv   # sim/scrambled_build.py: the tree is a changed copy of this project's code


def git(*args):
    r = subprocess.run(["git", *args], cwd=ROOT if SCRAMBLED else TREE, capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def version():
    """The version as scripts/gen_version.py makes it - from git, NOW (src/Version.cpp
    is only refreshed by a firmware build: reading it labelled uncommitted code
    with the last release's name and overwrote that release's build, 2026-11-06)."""
    desc = git("describe", "--tags", "--long", "--match", "v[0-9]*")
    if not desc:
        return "0.0.0-unknown"
    tag, commits, sha = desc.rsplit("-", 2)
    dirty = bool(git("status", "--porcelain", "--untracked-files=no", "--", *FIRMWARE_PATHS))
    v = tag[1:]
    if commits != "0" or dirty:
        v += "-%s-%s" % (commits, sha)
    return v + ("-dirty" if dirty else "")


def write_version_cpp(v):
    """src/Version.cpp as gen_version.py writes it (compiled into the simulator too)."""
    text = ('// Written by scripts/gen_version.py - not in git, don\'t edit.\n#include "Version.h"\n'
            'const char FIRMWARE_VERSION[] = "%s";\n'
            'extern "C" __attribute__((used)) const char kJcVersionMarker[] = "JCVER=%s;";\n' % (v, v))
    p = TREE / "src" / "Version.cpp"
    if not p.exists() or p.read_text(encoding="utf-8") != text:
        p.write_text(text, encoding="utf-8", newline="\n")


def main():
    if not ARDUINOJSON.exists():
        sys.exit("ArduinoJson not found - run `pio run` once so PlatformIO downloads the libraries.")
    global TREE
    if "--tree" in sys.argv:   # e.g. git worktree add C:/pio3/jc_clean/wt v2.1.0 -> today's stand-ins, that version's clock code
        TREE = Path(sys.argv[sys.argv.index("--tree") + 1]).resolve()
    if TREE == ROOT and subprocess.call([sys.executable, str(ROOT / "scripts" / "check_rules.py")]):
        sys.exit("build stopped: scripts/check_rules.py")   # as the firmware build (an older --tree had its own rules)
    OUT.mkdir(parents=True, exist_ok=True)
    ver = version()
    write_version_cpp(ver)
    clock = sorted(p for p in (TREE / "src").glob("*.cpp") if p.stem not in NOT_ON_PC)
    sources = [str(SIM / (s + ".cpp")) for s in SIM_SOURCES] + [str(p) for p in clock]
    if (TREE / "src" / "UpdateSignature.cpp").exists():   # signed updates (2026-11-14): Windows' crypto stands in for mbedtls
        sources.append(str(SIM / "sim_signature.cpp"))
    flags = ["c++", "-std=c++17", "-O1", "-g0", "-target", "x86_64-windows-gnu",
             "-include", str(SIM / "stubs" / "sim_force.h"), "-include", str(SIM / "clock_stubs" / "clock_force.h"),
             "-I", str(SIM / "clock_stubs"), "-I", str(SIM / "stubs"), "-I", str(SIM), "-I", str(TREE / "include"),
             "-I", str(ARDUINOJSON),
             "-DARDUINO=10819", "-DARDUINOJSON_ENABLE_PROGMEM=0", "-DJC_FULL_SIM=1",
             "-Wno-deprecated-declarations", "-Wno-unused-command-line-argument", "-Wno-invalid-offsetof"]
    exe = OUT / ("jcclock.exe" if TREE == ROOT or SCRAMBLED else "jcclock_tree.exe")   # scrambled: where the tests take it
    # --clean-ap: built as the clean copy is - no fixed password for the clock's own
    # Wi-Fi, each clock makes its own (Config.h AP_PASSWORD_DEFAULT ""). A test
    # build only: not kept as a version.
    clean_ap = "--clean-ap" in sys.argv
    if clean_ap:
        flags.append('-DAP_PASSWORD_DEFAULT=""')
        exe = OUT / "jcclock_cleanap.exe"
    print("building", exe, "...")
    if not link(compiler(flags) + sources + ["-lws2_32", "-lbcrypt"], exe):
        sys.exit("BUILD FAILED")
    if clean_ap or "--coverage" in sys.argv:   # a test / measuring build: never kept as a version
        print("OK:", exe)
        return
    dest = VERSIONS / ver
    # A release's build is kept as it was (the family's clock runs it - complaints
    # are looked at on exactly that build): never replaced, unless --force. Before
    # 2026-11-07 a withdrawn "2.1.1" kept running under the name of the new one.
    commit = git("rev-parse", "HEAD")
    source = dest / "SOURCE.txt"
    if "-" not in ver and source.exists() and "--force" not in sys.argv:
        print("OK:", exe, "(version %s; the kept release build %s stays as it is - %s)"
              % (ver, dest / "jcclock.exe", source.read_text(encoding="utf-8").split("\n")[0]))
        return
    dest.mkdir(parents=True, exist_ok=True)
    shutil.copy2(exe, dest / "jcclock.exe")
    source.write_text("commit %s\nbuilt %s\n" % (commit, time.strftime("%Y-%m-%d %H:%M")), encoding="utf-8")
    print("OK:", exe, "(version %s, also %s)" % (ver, dest / "jcclock.exe"))


if __name__ == "__main__":
    main()
