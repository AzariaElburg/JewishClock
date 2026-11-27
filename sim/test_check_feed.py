"""
utils/check_feed.py (the daily check of the update feed) against a stand-in for GitHub on this PC:
a good release passes; every way one can be wrong is found (2026-11-15).

    C:/pio3/jc_clean/venv/Scripts/python sim/test_check_feed.py [PACKAGE.jcupdate]

Without a package: the newest in C:/pio3/jc_clean/dist (signed by utils/make_update_package.py).
"""
import json
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPO = "uit/JewishClock-Updates"
fails = []


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    pkg = Path(sys.argv[1]) if len(sys.argv) > 1 else max(Path("C:/pio3/jc_clean/dist").glob("*.jcupdate"), key=lambda p: p.stat().st_mtime)
    # As a release: version "2.9.9" (a work build's "2.1.2-3-g..." is never a release tag), signed
    # again with the real key (utils/update_key.py - this PC has the private one)
    sys.path.insert(0, str(ROOT / "utils"))
    import update_key
    raw = pkg.read_bytes()
    body = raw[:raw.rfind(b"JCSIG1")]
    version = "2.9.9"
    body = b"JCUPD1" + bytes([len(version)]) + version.encode() + body[7 + body[6]:]
    sig = update_key.sign(body)
    good = body + b"JCSIG1" + len(sig).to_bytes(2, "little") + sig
    S = {}

    def reset(**k):
        S.clear()
        S.update({"repo": 200, "private": False, "latest": 200, "tag": "v" + version, "draft": False, "state": "uploaded",
                  "size": None, "file": good, "n_pkgs": 1})
        S.update(k)

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def send(self, code, body=b""):
            self.send_response(code)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            port = self.server.server_address[1]
            if self.path == "/repos/" + REPO:
                return self.send(S["repo"], json.dumps({"full_name": REPO, "private": S["private"]}).encode() if S["repo"] == 200 else b"")
            if self.path == "/repos/%s/releases/latest" % REPO:
                if S["latest"] != 200:
                    return self.send(S["latest"])
                size = len(S["file"]) if S["size"] is None else S["size"]
                assets = [{"name": "JewishClock-update-%s.jcupdate" % version, "state": S["state"], "size": size,
                           "browser_download_url": "http://127.0.0.1:%d/dl" % port}] * S["n_pkgs"]
                return self.send(200, json.dumps({"tag_name": S["tag"], "draft": S["draft"], "prerelease": False, "assets": assets}).encode())
            if self.path == "/dl":
                return self.send(200, S["file"])
            self.send(404)

    srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    api = "http://127.0.0.1:%d" % srv.server_address[1]

    def run(name, want_ok, want_text=None, **k):
        reset(**k)
        r = subprocess.run([sys.executable, str(ROOT / "utils" / "check_feed.py"), "--repo", REPO, "--api", api],
                           capture_output=True, text=True, encoding="utf-8")
        ok = (r.returncode == 0) == want_ok and (want_text is None or want_text in r.stdout)
        print("  %s  %s" % ("ok  " if ok else "FAIL", name))
        if not ok:
            fails.append(name)
            print("        " + r.stdout.replace("\n", "\n        "))

    bad = bytearray(good); bad[len(bad) // 2] ^= 0xFF
    print("check_feed.py against a stand-in feed (%s)" % pkg.name)
    run("a good release: all right", True, "ALL RIGHT")
    run("no release yet: all right, said", True, "no release there yet", latest=404)
    run("the repository gone", False, "the repository is there", repo=404)
    run("the repository private", False, "it is public", private=True)
    run("the package still being uploaded", False, "completely uploaded", state="starter")
    run("a draft", False, "not a draft", draft=True)
    run("no package in the release", False, "exactly one update package", n_pkgs=0)
    run("two packages in the release", False, "exactly one update package", n_pkgs=2)
    run("the size GitHub names is not the file's", False, "exactly its size", size=len(good) + 5)
    run("the tag not the package's version", False, "as its tag", tag="v99.0.0")
    run("the package damaged (signature fails)", False, "signature checks", file=bytes(bad))
    run("the package not signed", False, "signed (JCSIG1", file=good[:good.rfind(b"JCSIG1")])
    run("not a clock update at all", False, "a clock update", file=b"PK\x03\x04" + b"\0" * 5000)
    srv.shutdown()
    print("check_feed test: " + ("ALL CORRECT" if not fails else "%d FAILED" % len(fails)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
