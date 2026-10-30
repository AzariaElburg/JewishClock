"""Where generated files go: outside the project folder (2026-10-30).

The project folder is backed up by OneDrive; builds rewrite thousands of files
(the simulator's test files alone are over 1 GB), which OneDrive would upload
again and again and sometimes lock mid-build. Everything here can be rebuilt.
pioarduino.ini names the same folder for the firmware build (build_dir).
"""
from pathlib import Path

LOCAL = Path("C:/pio3/jc_clean")
LIBDEPS = LOCAL / "pio" / "libdeps"          # firmware libraries (pioarduino.ini libdeps_dir)
FIRMWARE_BUILD = LOCAL / "pio" / "build"     # pioarduino.ini build_dir
SIM_BUILD = LOCAL / "sim" / "build"          # jcsim.exe
SIM_EXE = SIM_BUILD / "jcsim.exe"
SIM_WORK = LOCAL / "sim" / "work"            # simulator file system, downloads, results
DIST = LOCAL / "dist"                        # update packages (make_update_package.py)
