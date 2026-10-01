#!/usr/bin/env python3
"""
Fetch and unpack Debian arm64 packages into a rootfs without needing root.

The debootstrap first stage left us with an extracted base but an empty dpkg
database, so apt/dpkg inside the chroot are useless.  This does the job the
other way round: resolve the dependency closure from the Packages index, then
unpack each .deb with dpkg-deb -x straight into the tree.

Usage:
    fetchdeb.py <rootfs> <package> [package...]
"""

import gzip
import os
import subprocess
import sys
import urllib.request

MIRROR = "http://deb.debian.org/debian"
DIST = "bookworm"
INDEX = f"{MIRROR}/dists/{DIST}/main/binary-arm64/Packages.gz"

# Packages debootstrap's minbase already put in the tree.  Re-unpacking them
# would be harmless but wasteful, and for a few of them (usrmerge) it is not.
BASE = {
    "libc6", "libgcc-s1", "libcrypt1", "libgomp1", "libstdc++6", "libzstd1",
    "libselinux1", "libpcre2-8-0", "libbz2-1.0", "liblzma5",
    "gcc-12-base", "libgcc-s1", "multiarch-support", "debconf", "dpkg",
    "install-info", "libgcrypt20", "libgpg-error0", "liblz4-1", "libsystemd0",
    "libudev1", "libcap2", "libpam0g", "libaudit1", "libtinfo6", "libgmp10",
    "libidn2-0", "libunistring2", "libssl3", "libffi8", "mount", "util-linux",
    "libncursesw6", "libncurses6", "libtinfo6", "bash", "coreutils", "dash",
    "sed", "grep", "gzip", "xz-utils", "tar", "findutils", "diffutils",
    "mawk", "libpcre3", "libgdbm6", "libdb5.3", "libbpf1", "libmnl0", "libxtables12",
    "libprocps8", "libsepol2", "libpopt0", "libtirpc3", "libargon2-1",
    "libjson-c5", "libjansson4", "libnettle8", "libhogweed6", "libgmp10",
    "libgnutls30", "libavahi-client3", "libavahi-common3", "libdbus-1-3",
    "libsystemd0", "libapparmor1", "libkmod2", "libk5crypto3", "libcom-err2",
    "libkrb5-3", "libkrb5support0", "libkeyutils1", "libgssapi-krb5-2",
    "libtasn1-6", "libp11-kit0", "libfribidi0", "libthai0", "libthai-data",
    "libdatrie1", "libunistring2", "libcairo2", "libpango-1.0-0", "libcups2",
}


def fetch_index():
    print(f"fetching {INDEX}", flush=True)
    with urllib.request.urlopen(INDEX, timeout=120) as r:
        raw = gzip.decompress(r.read())

    pkgs = {}
    for block in raw.split(b"\n\n"):
        if not block.strip():
            continue
        fields = {}
        key = None
        for line in block.split(b"\n"):
            if line[:1] in (b" ", b"\t") and key:
                continue
            if b":" in line:
                key, _, val = line.partition(b":")
                fields[key.decode().strip()] = val.strip().decode()
        if "Package" in fields:
            pkgs[fields["Package"]] = fields
    print(f"  {len(pkgs)} packages in index", flush=True)
    return pkgs


def deps_of(fields):
    out = []
    for line in (fields.get("Depends", ""), fields.get("Pre-Depends", "")):
        for alt in line.split(","):
            alt = alt.strip()
            if not alt:
                continue
            # take the first alternative of "a | b"
            name = alt.split("|")[0].strip()
            name = name.split()[0].split(":")[0]
            if name:
                out.append(name)
    return out


def resolve(pkgs, roots):
    seen, order, missing = set(), [], set()
    stack = list(roots)
    while stack:
        name = stack.pop()
        if name in seen:
            continue
        seen.add(name)
        p = pkgs.get(name)
        if p is None:
            missing.add(name)
            continue
        order.append(p)
        for d in deps_of(p):
            if d not in seen and d not in BASE:
                stack.append(d)
    return order, missing


def main():
    root = sys.argv[1]
    roots = sys.argv[2:]
    pkgs = fetch_index()
    order, missing = resolve(pkgs, roots)

    print(f"resolved {len(order)} packages", flush=True)
    if missing:
        print(f"  not in index (skipped): {', '.join(sorted(missing))}", flush=True)

    cache = "/tmp/opencode/debcache"
    os.makedirs(cache, exist_ok=True)

    for i, p in enumerate(order, 1):
        name = p["Package"]
        deb = os.path.join(cache, os.path.basename(p["Filename"]))
        if not os.path.exists(deb):
            url = f"{MIRROR}/{p['Filename']}"
            try:
                urllib.request.urlretrieve(url, deb)
            except Exception as e:
                print(f"  [{i}/{len(order)}] FAIL {name}: {e}", flush=True)
                continue
        r = subprocess.run(["dpkg-deb", "-x", deb, root],
                           capture_output=True)
        status = "ok" if r.returncode == 0 else "FAIL"
        print(f"  [{i}/{len(order)}] {status} {name}", flush=True)
        if r.returncode != 0:
            print("      " + r.stderr.decode()[:200], flush=True)

    print("done", flush=True)


if __name__ == "__main__":
    main()
