"""
Sets up a JewishClock build kit (make_source_bundle.py --kit) without any
download - the kit's copy of this file, run from the kit folder:

    python kit_setup.py

Makes the two Python environments PlatformIO needs, from python-wheels/:
  venv/  PlatformIO itself (pioarduino), as the firmware was built with it
         (venv-requirements.txt)
  penv/  the platform's own environment - it fills that from the internet on
         every build, unless PLATFORMIO_OFFLINE=1 (BUILD-KIT.txt); this puts
         the same packages there first (penv-requirements.txt), and esptool
         from the kit's packages/tool-esptoolpy, where the platform looks.
Windows, Python 3.10 to 3.13 (the kit's compiler is the Windows one).
"""
import subprocess
import sys
import venv
from pathlib import Path

KIT = Path(__file__).resolve().parent
WHEELS = KIT / "python-wheels"


def environment(d, *pip_args):
    py = d / "Scripts" / "python.exe"
    if not py.exists():
        venv.create(d, with_pip=True)
    subprocess.run([str(py), "-m", "pip", "install", "-q", "--no-index", "--find-links", str(WHEELS)] + list(pip_args),
                   check=True)
    return py


def main():
    # Complete lists of exact versions (pip freeze of the working ones), hence --no-deps.
    environment(KIT / "venv", "--no-deps", "-r", str(KIT / "venv-requirements.txt"))
    environment(KIT / "penv", "--no-deps", "-r", str(KIT / "penv-requirements.txt"))
    environment(KIT / "penv", "--no-build-isolation", "--no-deps", "-e", str(KIT / "packages" / "tool-esptoolpy"))
    print("ready: %s" % (KIT / "venv" / "Scripts" / "pio.exe"))


if __name__ == "__main__":
    sys.exit(main())
