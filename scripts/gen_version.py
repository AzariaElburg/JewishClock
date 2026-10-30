# PlatformIO pre-build script: the firmware version, from git, into
# src/Version.cpp (declared in include/Version.h).
#
# The version (2026-10-26) - git describe, the usual way:
#   a release is a git tag "v<a.b.c>"            e.g. git tag v2.0.1
#   a build of exactly that tag, firmware sources committed  -> "2.0.1"
#   anything else -> "2.0.1-3-g1a2b3c4" (3 commits after it, at commit
#   1a2b3c4), plus "-dirty" if the firmware sources (src include lib scripts
#   pioarduino.ini) have uncommitted changes. Data/config.json and events.json
#   don't count: they hold the clock's synced values and are always modified.
# No git (or no tag): "0.0.0-unknown".
#
# A generated source file, not a compiler define: a define that changes made
# PlatformIO recompile everything - libraries and the whole Arduino core
# included, 5-9 minutes per build. PlatformIO's own advice:
# https://community.platformio.org/t/extra-scripts-and-imprintig-fw-ver-in-code-rebuild-all-files-always/40245
# The file is rewritten only when the version changes, so a build without
# changes compiles nothing.

Import("env")
import os
import subprocess

project_dir = env.get("PROJECT_DIR")
out_path = os.path.join(project_dir, "src", "Version.cpp")
FIRMWARE_PATHS = ["src", "include", "lib", "scripts", "pioarduino.ini"]


def git(*args):
    r = subprocess.run(["git", *args], cwd=project_dir, capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def firmware_version():
    desc = git("describe", "--tags", "--long", "--match", "v[0-9]*")   # v2.0.1-3-g1a2b3c4
    if not desc:
        return "0.0.0-unknown"
    tag, commits, sha = desc.rsplit("-", 2)
    dirty = bool(git("status", "--porcelain", "--untracked-files=no", "--", *FIRMWARE_PATHS))
    version = tag[1:]
    if commits != "0" or dirty:
        version += "-%s-%s" % (commits, sha)
    if dirty:
        version += "-dirty"
    return version


version = firmware_version()
print("Firmware version: %s" % version)

text = (
    "// Written by scripts/gen_version.py - not in git, don't edit.\n"
    '#include "Version.h"\n'
    'const char FIRMWARE_VERSION[] = "%s";\n'
    'extern "C" __attribute__((used)) const char kJcVersionMarker[] = "JCVER=%s;";\n'
    % (version, version)
)
old = open(out_path).read() if os.path.exists(out_path) else None
if old != text:
    with open(out_path, "w", newline="\n") as f:
        f.write(text)
