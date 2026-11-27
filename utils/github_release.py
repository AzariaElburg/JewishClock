"""
A release onto GitHub (2026-11-09), after the tag, the package, the documents,
the source bundle and the clean snapshot are made (CLAUDE.md "Every release"):

    C:/pio3/jc_clean/venv/Scripts/python utils/github_release.py 2.1.2

1. This project (remote origin): main moves to tag v<version> (main = released
   versions only, ai-assisted = work in progress); main, ai-assisted and the tag
   are pushed.
2. The clean copy (the folder next to this one, ending in _clean): its main and
   its tag v<version> are pushed - it must already have that tag (snapshot.py --tag).
3. A release page on each, for the tag:
   - this project: what the family gets (C:/pio3/jc_clean/dist/<version>/: the update
     package and the PDFs);
   - the clean copy: the source bundle and build kit
     (C:/pio3/jc_clean/dist/sources/<version>/ - the zips and the BUILD-KIT text).
   A file already there with the same size is skipped, so a broken run can be
   repeated.

Signs in with what Git Credential Manager stored (signed in once, in a window of
its own); the token is never printed. GitHub owner and repo names are read from
each folder's remote, never written here.
"""
import hashlib
import http.client
import json
import os
import re
import subprocess
import sys
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CLEAN = ROOT.parent / (ROOT.name.rsplit("_", 1)[0] + "_clean")
DIST = Path("C:/pio3/jc_clean/dist")

FAMILY_TEXT = """The version for the family.

- `JewishClock-update-{v}.jcupdate` - the update, installed from the clock's settings page (Firmware Update)
- `JewishClock-install-guide-{v}.pdf` - how to install it, step by step
- `JewishClock-{v}-whats-new.pdf` - what changed
- `JewishClock-manual-{v}.pdf` - the printed manual

Source package and build kit for this version: the {v} release of the clean copy.
"""
UPDATES_TEXT = """Jewish Clock {v} - the update package. Clocks with automatic updates install it at night;
any clock can install it from Settings > Clock software. Signed: a clock installs only packages its firmware's key confirms.

The update key (key id {key_id}) - for a clock that needs it (a new key, or none): Settings > Clock software > Update key > Paste a key:

```
{key_hex}
```
"""
CLEAN_TEXT = """Jewish Clock {v}.

- `sources-{v}.zip` - the exact source of every library and of the Arduino core this version is built with
- `build-kit-{v}-*.zip` - compiler, ESP-IDF libraries, platform and Python packages, to rebuild this version offline
- `BUILD-KIT-{v}.txt` - how to use the kit
- `JewishClock-Simulator-{v}.zip` - the clock on a Windows PC, no hardware: unpack, double-click Start.cmd

Licence of this software: © Azaria Elburg, all rights reserved. The licence is yet to be decided; until then nobody may use, copy, change or redistribute it without written permission from Azaria.

Third-party licenses: `THIRD_PARTY_LICENSES/` in the code, and `Data/licenses.txt` (also on the clock: Settings > Clock software > Licenses).
"""


def git(folder, *args):
    r = subprocess.run(["git", *args], cwd=folder, capture_output=True, text=True)
    if r.returncode:
        sys.exit("git %s failed in %s:\n%s" % (" ".join(args), folder, r.stderr.strip()))
    return r.stdout.strip()


def owner_repo(folder):
    m = re.search(r"github\.com[:/]([^/]+)/(.+?)(\.git)?$", git(folder, "remote", "get-url", "origin"))
    if not m:
        sys.exit("%s: origin is not a GitHub repository" % folder)
    return m.group(1), m.group(2)


def token():
    out = subprocess.run(["git", "credential", "fill"], input="protocol=https\nhost=github.com\n\n",
                         capture_output=True, text=True, env=dict(os.environ, GCM_INTERACTIVE="never"))
    for line in out.stdout.splitlines():
        if line.startswith("password="):
            return line[len("password="):]
    sys.exit("no GitHub sign-in stored - push once from a window of its own to sign in")


def call(method, host, path, tok, body=None, headers=None, size=None):
    c = http.client.HTTPSConnection(host, timeout=600)
    h = {"Authorization": "Bearer " + tok, "Accept": "application/vnd.github+json",
         "User-Agent": "jewishclock-release", "X-GitHub-Api-Version": "2022-11-28"}
    h.update(headers or {})
    if size is not None:
        h["Content-Length"] = str(size)
    c.request(method, path, body=body, headers=h)
    r = c.getresponse()
    data = r.read()
    return r.status, (json.loads(data) if data else None)


class _NoAuthOnRedirect(urllib.request.HTTPRedirectHandler):
    """GitHub redirects an asset download to its file store with a signed address; the sign-in
    must not go along (the store refuses two ways of signing in, and it is not its business)."""
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        new = super().redirect_request(req, fp, code, msg, headers, newurl)
        if new is not None:
            new.remove_header("Authorization")
        return new


def read_back(owner, repo, asset_id, tok):
    """An asset's bytes as anyone downloads them (GitHub redirects to its file store)."""
    req = urllib.request.Request("https://api.github.com/repos/%s/%s/releases/assets/%d" % (owner, repo, asset_id),
                                 headers={"Authorization": "Bearer " + tok, "Accept": "application/octet-stream",
                                          "User-Agent": "jewishclock-release"})
    with urllib.request.build_opener(_NoAuthOnRedirect).open(req, timeout=600) as r:
        return r.read()


def release(folder, tag, title, text, files, tok, where=None):
    """A release page with these files. Made as a DRAFT and published only once every file is
    up and read back byte for byte (2026-11-15): a clock asking the update feed never sees a
    release whose package is still arriving (OnlineUpdate.cpp takes only "uploaded" ones too)."""
    owner, repo = where or owner_repo(folder)
    st, rel = call("GET", "api.github.com", "/repos/%s/%s/releases/tags/%s" % (owner, repo, tag), tok)
    if st == 404:   # a draft has no tag yet as far as this lookup goes: find it in the list
        st2, rels = call("GET", "api.github.com", "/repos/%s/%s/releases?per_page=100" % (owner, repo), tok)
        rel = next((r for r in rels or [] if r.get("draft") and r.get("tag_name") == tag), None) if st2 == 200 else None
        st = 200 if rel else 404
    if st == 404:
        body = json.dumps({"tag_name": tag, "name": title, "body": text, "draft": True})
        st, rel = call("POST", "api.github.com", "/repos/%s/%s/releases" % (owner, repo), tok, body.encode(),
                       {"Content-Type": "application/json"})
        if st != 201:
            sys.exit("%s: release not created: %s %s" % (repo, st, rel))
        print("release page made (draft until its files are up):", rel["html_url"])
    elif st == 200:
        print("release page there%s:" % (" (draft)" if rel.get("draft") else ""), rel["html_url"])
    else:
        sys.exit("%s: %s %s" % (repo, st, rel))
    have = {a["name"]: a for a in rel.get("assets", [])}
    for f in files:
        size = f.stat().st_size
        a = have.get(f.name)
        if a and a["size"] == size and a.get("state") == "uploaded":
            print("  already there:", f.name)
            continue
        if a:   # a broken earlier try (wrong size, never finished): away with it
            call("DELETE", "api.github.com", "/repos/%s/%s/releases/assets/%d" % (owner, repo, a["id"]), tok)
        with open(f, "rb") as fh:
            st, a = call("POST", "uploads.github.com", "/repos/%s/%s/releases/%d/assets?name=%s" % (
                owner, repo, rel["id"], urllib.parse.quote(f.name)), tok, fh,
                {"Content-Type": "application/octet-stream"}, size)
        if st != 201:
            sys.exit("  %s not uploaded: %s %s" % (f.name, st, a))
        have[f.name] = a
        print("  uploaded: %s (%.1f MB)" % (f.name, size / 1e6), flush=True)
    # Read back what a clock would download - exactly the file made here?
    for f in files:
        a = have[f.name]
        if f.suffix != ".jcupdate":
            continue   # the big zips: their size is GitHub's own check; the packages are what clocks take
        got = read_back(owner, repo, a["id"], tok)
        if hashlib.sha256(got).digest() != hashlib.sha256(f.read_bytes()).digest():
            sys.exit("  %s: what GitHub gives back is not the file sent (%d bytes) - the release stays a draft" % (f.name, len(got)))
        print("  read back, the same bytes:", f.name)
    if rel.get("draft"):
        st, rel = call("PATCH", "api.github.com", "/repos/%s/%s/releases/%d" % (owner, repo, rel["id"]), tok,
                       json.dumps({"draft": False, "make_latest": "true"}).encode(), {"Content-Type": "application/json"})
        if st != 200:
            sys.exit("%s: not published: %s %s" % (repo, st, rel))
        print("published:", rel["html_url"])


def main():
    if len(sys.argv) != 2 or not re.fullmatch(r"\d+\.\d+\.\d+", sys.argv[1]):
        sys.exit(__doc__)
    v = sys.argv[1]
    tag = "v" + v
    family = [DIST / v / n for n in ("JewishClock-update-%s.jcupdate" % v, "JewishClock-install-guide-%s.pdf" % v,
                                     "JewishClock-%s-whats-new.pdf" % v, "JewishClock-manual-%s.pdf" % v)]
    src = DIST / "sources" / v
    sources = sorted(src.glob("sources-%s.zip" % v)) + sorted(src.glob("BUILD-KIT-%s.txt" % v)) + \
        sorted(src.glob("build-kit-%s-*.zip" % v))
    missing = [str(f) for f in family if not f.is_file()] + ([] if len(sources) >= 3 else [str(src) + " (sources, kit)"])
    if missing:
        sys.exit("missing:\n  " + "\n  ".join(missing))
    git(ROOT, "rev-parse", "--verify", tag + "^{commit}")
    if not git(CLEAN, "tag", "-l", tag):
        sys.exit("the clean copy has no tag %s - utils/clean/snapshot.py --ref %s --commit MSG --tag %s first" % (tag, tag, tag))
    # The simulator download (2026-11-14): built from the clean copy at the tag - nothing private in it
    sim_zip = DIST / ("JewishClock-Simulator-%s.zip" % v)
    if not sim_zip.is_file():
        if git(CLEAN, "rev-parse", "HEAD") != git(CLEAN, "rev-parse", tag + "^{commit}"):
            sys.exit("the clean copy is not at %s - check it out there to build the simulator download" % tag)
        subprocess.run([sys.executable, str(ROOT / "utils" / "make_sim_download.py"), "--tree", str(CLEAN)], check=True)
    if not sim_zip.is_file():
        sys.exit("no %s - the simulator download names another version (is the clean copy exactly at %s?)" % (sim_zip, tag))
    sources.append(sim_zip)
    tok = token()

    git(ROOT, "branch", "-f", "main", tag)
    git(ROOT, "push", "origin", "main", "ai-assisted", tag)
    print("pushed: main (= %s), ai-assisted, %s" % (tag, tag))
    git(CLEAN, "push", "origin", "main", tag)
    print("clean copy pushed: main,", tag)
    release(ROOT, tag, v, FAMILY_TEXT.format(v=v), family, tok)
    release(CLEAN, tag, v, CLEAN_TEXT.format(v=v), sources, tok)
    # The public update feed the clocks ask (OnlineUpdate.h, Config.h UPDATE_FEED_URL): the
    # package only - and never while the firmware still has a compiled-in Wi-Fi password
    # (the family's, until their clock has copied it into its settings: CLAUDE.md).
    cfg_h = (ROOT / "include" / "Config.h").read_text(encoding="utf-8")
    if re.search(r'#define AP_PASSWORD_DEFAULT\s+"[^"]+"', cfg_h):
        print("NOT on the public update feed: this firmware still has a compiled-in Wi-Fi password "
              "(AP_PASSWORD_DEFAULT) - send this one by email; set it to \"\" once the family's clock has it in its settings")
    else:
        sys.path.insert(0, str(ROOT / "utils"))
        import update_key
        from check_feed import feed_repo
        pub = update_key.header_public_der()
        release(None, tag, v, UPDATES_TEXT.format(v=v, key_id=update_key.key_id(pub), key_hex=pub.hex()), family[:1], tok,
                where=tuple(feed_repo().split("/")))
    print("OK")


if __name__ == "__main__":
    main()
