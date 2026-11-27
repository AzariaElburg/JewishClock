"""
The update signing key (2026-11-14): every update package is signed on this PC; the clock
installs only packages whose signature its firmware's public key confirms - from GitHub or
from a file.

    python utils/update_key.py init      # once: make the key pair, write include/UpdateKey.h
    python utils/update_key.py header    # include/UpdateKey.h again from the key
    python utils/update_key.py where     # where the private key is
    python utils/update_key.py public    # the public key to paste into a clock (Settings > Update key > Paste a key)

The PRIVATE key never goes into git or OneDrive: %USERPROFILE%/.jewishclock/update_signing_key.pem
(or the file in JC_SIGNING_KEY). Lost = no more updates the clock accepts until a firmware
with a new public key is installed over USB. Keep a copy somewhere safe (not in the project).
The PUBLIC key is in include/UpdateKey.h (in git: anyone may check a signature with it).

ECDSA P-256 / SHA-256, made and used with OpenSSL (Git for Windows has it); the clock checks
with mbedtls (UpdateSignature.cpp).
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "include" / "UpdateKey.h"
KEY = Path(os.environ.get("JC_SIGNING_KEY") or Path.home() / ".jewishclock" / "update_signing_key.pem")


def openssl():
    exe = shutil.which("openssl") or "C:/Program Files/Git/mingw64/bin/openssl.exe"
    if not Path(exe).exists() and not shutil.which(exe):
        sys.exit("OpenSSL not found (it comes with Git for Windows)")
    return exe


def public_der(key=KEY):
    return subprocess.run([openssl(), "ec", "-in", str(key), "-pubout", "-outform", "DER"], capture_output=True, check=True).stdout


def header_public_der():
    """The public key the firmware carries (include/UpdateKey.h)."""
    import re
    text = HEADER.read_text(encoding="utf-8")
    body = re.search(r"UPDATE_PUBLIC_KEY_DER\[\] = \{([^}]*)\}", text).group(1)
    return bytes(int(x, 16) for x in body.replace(" ", "").split(",") if x)


def key_id(pub):
    return hashlib.sha256(pub).hexdigest()[:16]


def sign(data, key=KEY):
    """The DER signature (ECDSA P-256 over SHA-256) of data."""
    if not Path(key).exists():
        sys.exit("No signing key at %s - `python utils/update_key.py init` once (or JC_SIGNING_KEY)" % key)
    with tempfile.TemporaryDirectory() as d:
        f = Path(d) / "data"
        f.write_bytes(data)
        return subprocess.run([openssl(), "dgst", "-sha256", "-sign", str(key), str(f)], capture_output=True, check=True).stdout


def verify(data, sig, pub_der):
    with tempfile.TemporaryDirectory() as d:
        f, s, p = Path(d) / "data", Path(d) / "sig", Path(d) / "pub.der"
        f.write_bytes(data); s.write_bytes(sig); p.write_bytes(pub_der)
        r = subprocess.run([openssl(), "dgst", "-sha256", "-verify", str(p), "-keyform", "DER", "-signature", str(s), str(f)],
                           capture_output=True, text=True)
        return r.returncode == 0


def write_header(pub):
    arr = ", ".join("0x%02x" % b for b in pub)
    HEADER.write_text("""#pragma once
// The public half of the update signing key (utils/update_key.py - made there, never by hand).
// The clock installs only update packages signed with its private half, which stays on the
// author's PC. Key id %s.
#include <stdint.h>
#include <stddef.h>

static const uint8_t UPDATE_PUBLIC_KEY_DER[] = { %s };
static const char    UPDATE_PUBLIC_KEY_ID[] = "%s";
""" % (key_id(pub), arr, key_id(pub)), encoding="utf-8")
    print("wrote", HEADER, "key id", key_id(pub))


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "where"
    if cmd == "init":
        if KEY.exists():
            sys.exit("A key exists already: %s - never replace it by accident (the clocks know only its public half)" % KEY)
        KEY.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([openssl(), "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(KEY)], check=True)
        write_header(public_der())
        print("private key:", KEY, "- keep a copy somewhere safe, outside the project")
    elif cmd == "header":
        write_header(public_der())
    elif cmd == "where":
        print(KEY, "(exists)" if KEY.exists() else "(not made yet)")
    elif cmd == "public":   # the public key as pasted into the clock (Settings > Update key > Paste a key)
        pub = header_public_der()
        print(pub.hex())
        print("key id", key_id(pub))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
