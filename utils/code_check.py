"""
Static code check of the clock's own code (src/, include/) with clang-tidy -
the LLVM project's checker, incl. the Clang Static Analyzer (follows paths through
the code: null pointers, buffers overrun, memory used after free, ...).

    C:/pio3/jc_clean/venv/Scripts/python utils/code_check.py [FILE.cpp ...] [--jobs N]

Started 2026-11-13 after an overflow found by reading (dafYomi[48] copied with a
size of 64). clang-tidy is the one VS Code's C/C++ extension ships (no download).
The code is checked the way the simulator compiles it (sim/build_clock.py: the
same src/, PC stand-ins for the hardware headers, Zig's clang headers) - clang
can't parse the ESP32 toolchain's own flags.

Out: C:/pio3/jc_clean/code_check/<date>/ - one .txt per file and summary.txt
(findings by check, ours only: src/ and include/).
"""
import argparse
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SIM = ROOT / "sim"
sys.path.insert(0, str(SIM))
from build import find_zig, ARDUINOJSON  # noqa: E402
from build_clock import NOT_ON_PC  # noqa: E402

# What is looked for. The analyzer and bugprone are the point; cert / performance /
# concurrency add known-dangerous patterns. Left out: pure style, and checks whose
# every finding here is noise (named in the comment where they are).
CHECKS = ",".join([
    "-*",
    "clang-analyzer-*",
    "bugprone-*",
    "-bugprone-easily-swappable-parameters",   # any two ints side by side
    "cert-*",
    "-cert-err58-cpp",                          # static objects may throw - not on this target
    "concurrency-*",
    "performance-*",
    "-performance-enum-size",                   # 4-byte enums: a few bytes, nothing else
    "misc-redundant-expression",
    "misc-misplaced-const",
])


def clang_tidy():
    ext = Path.home() / ".vscode" / "extensions"
    found = sorted(ext.glob("ms-vscode.cpptools-*/LLVM/bin/clang-tidy.exe"))
    if not found:
        sys.exit("clang-tidy not found (VS Code's C/C++ extension ships it)")
    return str(found[-1])


def compile_args():
    zig = Path(find_zig()).parent / "lib"
    return ["-std=c++17", "--target=x86_64-w64-windows-gnu", "-nostdinc", "-nostdinc++",
            "-isystem", str(zig / "libcxx" / "include"), "-isystem", str(zig / "libcxxabi" / "include"),
            "-isystem", str(zig / "include"),
            "-isystem", str(zig / "libc" / "include" / "x86_64-windows-gnu"),
            "-isystem", str(zig / "libc" / "include" / "any-windows-any"),
            "-D_LIBCPP_HAS_NO_VENDOR_AVAILABILITY_ANNOTATIONS", "-D_LIBCPP_ABI_VERSION=1", "-D_LIBCPP_ABI_NAMESPACE=__1",
            "-D_LIBCPP_DISABLE_VISIBILITY_ANNOTATIONS", "-D_LIBCPP_HAS_NO_VENDOR_AVAILABILITY_ANNOTATIONS",
            "-include", str(SIM / "stubs" / "sim_force.h"), "-include", str(SIM / "clock_stubs" / "clock_force.h"),
            "-I", str(SIM / "clock_stubs"), "-I", str(SIM / "stubs"), "-I", str(SIM), "-I", str(ROOT / "include"),
            "-isystem", str(ARDUINOJSON),
            "-DARDUINO=10819", "-DARDUINOJSON_ENABLE_PROGMEM=0", "-DJC_FULL_SIM=1",
            "-Wno-deprecated-declarations", "-Wno-invalid-offsetof", "-Wno-unknown-warning-option"]


def check_one(tidy, src, out_dir):
    cmd = [tidy, "--quiet", "--checks=" + CHECKS,
           "--header-filter=" + re.escape(str(ROOT / "include")).replace("\\\\", "[\\\\/]"),
           str(src), "--"] + compile_args()
    t0 = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    text = r.stdout + r.stderr
    (out_dir / (src.stem + ".txt")).write_text(text, encoding="utf-8")
    return src, text, time.time() - t0


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="*")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    a = ap.parse_args()
    files = [Path(f).resolve() for f in a.files] or sorted(p for p in (ROOT / "src").glob("*.cpp") if p.stem not in NOT_ON_PC)
    out_dir = Path("C:/pio3/jc_clean/code_check") / time.strftime("%Y%m%d_%H%M")
    out_dir.mkdir(parents=True, exist_ok=True)
    tidy = clang_tidy()
    found = {}   # "file:line: [check] message" -> check
    errors = []
    with ThreadPoolExecutor(a.jobs) as pool:
        for src, text, sec in pool.map(lambda s: check_one(tidy, s, out_dir), files):
            n = 0
            for line in text.splitlines():
                m = re.match(r"(.+?):(\d+):\d+: (warning|error): (.*?) \[([\w\-.,]+)\]$", line)
                if not m:
                    continue
                path = Path(m.group(1))
                try:
                    rel = path.resolve().relative_to(ROOT).as_posix()
                except ValueError:
                    continue
                if not (rel.startswith("src/") or rel.startswith("include/")):
                    continue
                key = "%s:%s: [%s] %s" % (rel, m.group(2), m.group(5), m.group(4))
                if m.group(3) == "error" and m.group(5).startswith("clang-diagnostic"):
                    errors.append(key)
                    continue
                if key not in found:
                    found[key] = m.group(5)
                    n += 1
            print("%-28s %3d new  %5.1f s" % (src.name, n, sec))
    by_check = {}
    for k, c in found.items():
        by_check.setdefault(c, []).append(k)
    lines = ["%d findings in our code, %d compile errors (the check couldn't read something)" % (len(found), len(errors)), ""]
    for c in sorted(by_check, key=lambda c: -len(by_check[c])):
        lines.append("== %s (%d)" % (c, len(by_check[c])))
        lines += ["   " + k for k in sorted(by_check[c])]
    if errors:
        lines += ["", "== compile errors"] + ["   " + e for e in sorted(set(errors))]
    (out_dir / "summary.txt").write_text("\n".join(lines), encoding="utf-8")
    print("\n" + lines[0] + "\n" + str(out_dir / "summary.txt"))


if __name__ == "__main__":
    main()
