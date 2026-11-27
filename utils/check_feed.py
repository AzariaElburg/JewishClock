"""
The update feed, checked from outside (2026-11-15, the user): what every clock asks daily must be
there and right. Run daily by GitHub (.github/workflows/update-feed-monitor.yml in the private repo:
a failed run is emailed to its owner), and by hand:

    C:/pio3/jc_clean/venv/Scripts/python utils/check_feed.py [--repo OWNER/NAME] [--api URL]

Checks, as a clock would see it (OnlineUpdate.cpp), and a little more:
  1. the repository is there and public (a clock can't sign in);
  2. its latest release: none yet = fine (said); else a tag vA.B.C, not a draft or pre-release,
     exactly one .jcupdate package, completely uploaded, with a size;
  3. the package downloads at exactly that size, is a clock update (JCUPD1 .. JCEND!), says the
     tag's version, and its signature checks with the public key the firmware carries
     (include/UpdateKey.h).
Exit 0 = all right; 1 = a problem (each said). Read only: it changes nothing anywhere.
"""
import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "utils"))
import update_key  # noqa: E402  (the public key and the signature check)

problems = []


def say(ok, what):
    print(("ok    " if ok else "WRONG ") + what, flush=True)
    if not ok:
        problems.append(what)
    return ok


def get(url, accept="application/vnd.github+json"):
    """(status, bytes) - GitHub's redirects to its file store followed, the sign-in not sent there."""
    h = {"User-Agent": "JewishClock-feed-check", "Accept": accept}
    tok = os.environ.get("GITHUB_TOKEN")
    if tok and "api.github.com" in url:
        h["Authorization"] = "Bearer " + tok   # Actions: more requests allowed; public data either way
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=h), timeout=120) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:   # no connection
        return 0, str(e).encode()


def feed_repo():
    """The feed the firmware asks: OWNER/NAME, Config.h UPDATE_FEED_REPO (named only there)."""
    m = re.search(r'#define UPDATE_FEED_REPO\s+"([^"]+)"', (ROOT / "include" / "Config.h").read_text(encoding="utf-8"))
    return m.group(1) if m else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--repo", default=feed_repo(), help="OWNER/NAME of the update feed's repository")
    ap.add_argument("--api", default="https://api.github.com", help="GitHub's API (another for tests)")
    a = ap.parse_args()
    if not a.repo:
        sys.exit("no repository - give --repo OWNER/NAME")
    base = a.api.rstrip("/") + "/repos/" + a.repo
    print("update feed: " + a.repo)

    st, body = get(base)
    repo = json.loads(body) if st == 200 else {}
    if not say(st == 200, "the repository is there (answer %s)" % st):
        return finish()
    say(repo.get("private") is False, "it is public (clocks can't sign in)")

    st, body = get(base + "/releases/latest")
    if st == 404:
        print("      no release there yet - clocks say 'No versions on the update feed yet'")
        return finish()
    if not say(st == 200, "its latest release answers (answer %s)" % st):
        return finish()
    rel = json.loads(body)
    tag = rel.get("tag_name", "")
    say(re.fullmatch(r"v\d+\.\d+\.\d+", tag) is not None, "latest release %s: a version tag vA.B.C" % (tag or "(none)"))
    say(not rel.get("draft") and not rel.get("prerelease"), "... published, not a draft or pre-release")
    pkgs = [x for x in rel.get("assets", []) if x.get("name", "").endswith(".jcupdate")]
    if not say(len(pkgs) == 1, "... exactly one update package (.jcupdate) - %d" % len(pkgs)):
        return finish()
    p = pkgs[0]
    say(p.get("state") == "uploaded", "... %s completely uploaded (state %s)" % (p["name"], p.get("state")))
    size = p.get("size", 0)
    if not say(size > 0, "... its size known (%s bytes)" % size):
        return finish()

    st, data = get(p["browser_download_url"], "application/octet-stream")
    if not say(st == 200, "the package downloads (answer %s)" % st):
        return finish()
    say(len(data) == size, "... exactly its size (%d of %d bytes)" % (len(data), size))
    if not say(data.startswith(b"JCUPD1") and b"JCEND!" in data, "... a clock update (JCUPD1 .. JCEND!)"):
        return finish()
    n = data[6]
    version = data[7:7 + n].decode("ascii", "replace")
    say("v" + version == tag, "... says version %s, as its tag" % version)
    cut = data.rfind(b"JCSIG1")
    if not say(cut > 0, "... signed (JCSIG1 after the package)"):
        return finish()
    slen = int.from_bytes(data[cut + 6:cut + 8], "little")
    sig = data[cut + 8:cut + 8 + slen]
    pub = update_key.header_public_der()
    say(update_key.verify(data[:cut], sig, pub), "... its signature checks with the clocks' key (id %s)" % update_key.key_id(pub))
    return finish()


def finish():
    print("ALL RIGHT" if not problems else "%d PROBLEM%s - clocks may get no update" % (len(problems), "" if len(problems) == 1 else "S"))
    if problems and os.environ.get("GITHUB_ACTIONS"):   # an error note: GitHub's failure email shows these
        for what in problems[:5]:
            print("::error title=Update feed problem::" + what)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
