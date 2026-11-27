"""The web files as the clock gets them (2026-11-04).

Data/ keeps every file plain, for editing. The clock gets the files only a
browser reads gzipped instead (index.html 218 KB -> ~64 KB): the page loaded
in ~1 s, almost all of it index.html's download. The web server library
sends "<path>.gz" when "<path>" is missing, with Content-Encoding: gzip and
an ETag from the gzip's CRC - the browser keeps the page and asks again with
it, an unchanged page comes back as 304 (no body).

Only files nothing on the clock reads itself: tz.csv, the pictures, the
Excel tools stay plain. gzip with mtime 0, so the same file gives the same
bytes (and ETag) every time - and their line ends as LF, as git keeps them:
a checkout with core.autocrlf (this PC's git) has them CRLF, so a release
built from a worktree differed from the project folder's in every page file
(2026-11-06, the web check of 2.1.0). Browsers don't care either way.

Used by scripts/stage_fs.py (uploadfs) and utils/make_update_package.py.
"""
import gzip
import os
import shutil
import stat
from pathlib import Path

GZIP = {"index.html", "update.html", "sortable.min.js", "licenses.txt",   # paths under Data/
        "start.html", "manual.html", "whats_new.html"}


def clock_files(data_dir):
    """(path under Data/ as posix, bytes, source file) for every file, as the clock gets it."""
    data_dir = Path(data_dir)
    for f in sorted(data_dir.rglob("*")):
        if not f.is_file():
            continue
        rel = f.relative_to(data_dir).as_posix()
        if rel in GZIP:
            plain = f.read_bytes().replace(b"\r\n", b"\n")
            yield rel + ".gz", gzip.compress(plain, compresslevel=9, mtime=0), f
        else:
            yield rel, f.read_bytes(), f


def remove_tree(path):
    """Deletes a folder, read-only ones too: folders copied out of OneDrive keep its
    read-only mark, which shutil.rmtree can't delete on Windows (the full test run
    stopped on Data/custom's copy, 2026-11-08)."""
    def clear_and_retry(func, p, _exc):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    if Path(path).exists():
        shutil.rmtree(path, onerror=clear_and_retry)


def stage(data_dir, out_dir):
    """out_dir := Data/ as the clock gets it (file times kept: the page's
    build time on the clock is index.html's)."""
    out_dir = Path(out_dir)
    remove_tree(out_dir)
    for rel, data, src in clock_files(data_dir):
        dst = out_dir / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(data)
        st = src.stat()
        os.utime(dst, (st.st_atime, st.st_mtime))
