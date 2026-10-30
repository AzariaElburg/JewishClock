#pragma once
// The firmware version - defined in src/Version.cpp, which
// scripts/gen_version.py writes before every build (not in git): from git,
// "2.0.1" for a build of a release tag (v2.0.1) with no uncommitted firmware
// changes, else e.g. "2.0.1-3-g1a2b3c4[-dirty]" (3 commits after it, at
// commit 1a2b3c4). A file, not a compiler define: a define that changes made
// PlatformIO recompile everything, libraries and the Arduino core included
// (5-9 min per build; 2026-10-26).
extern const char FIRMWARE_VERSION[];
// "JCVER=<version>;" - utils/make_update_package.py reads it out of firmware.bin.
extern "C" const char kJcVersionMarker[];
