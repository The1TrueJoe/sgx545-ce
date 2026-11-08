#!/usr/bin/env python3
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
"""A Debian root from packages alone, no debootstrap: for an X server on the EA and as the
sysroot our Mesa's X11/GLX build compiles against.

    mkroot.py ROOT [--arch amd64] [--suite bookworm] PACKAGE...

Reads the suite's main/<arch> Packages index, resolves Depends and Pre-Depends (the first
alternative that exists, virtual packages through Provides), downloads each .deb into
ROOT/../debs (checked against the index's SHA256) and unpacks its data into ROOT. Maintainer
scripts are not run: the root is for running given programs in a chroot and for linking
against, not for apt. Packages that Mesa itself replaces are left out (--skip).
"""
import argparse
import hashlib
import io
import lzma
import os
import re
import subprocess
import sys
import tarfile
import urllib.request

MIRROR = "https://deb.debian.org/debian"


def fetch(url):
    with urllib.request.urlopen(url, timeout=120) as r:
        return r.read()


def index(suite, arch, cache):
    path = os.path.join(cache, f"Packages-{suite}-{arch}.xz")
    if not os.path.exists(path):
        data = fetch(f"{MIRROR}/dists/{suite}/main/binary-{arch}/Packages.xz")
        with open(path, "wb") as f:
            f.write(data)
    pkgs, provides = {}, {}
    for stanza in lzma.decompress(open(path, "rb").read()).decode().split("\n\n"):
        fields, key = {}, None
        for line in stanza.split("\n"):
            if line.startswith(" ") and key:
                fields[key] += "\n" + line
            elif ":" in line:
                key, val = line.split(":", 1)
                fields[key] = val.strip()
        if "Package" not in fields:
            continue
        pkgs[fields["Package"]] = fields
        for p in re.split(r",\s*", fields.get("Provides", "")):
            if p:
                provides.setdefault(p.split()[0], []).append(fields["Package"])
    return pkgs, provides


def deps(fields):
    out = []
    for key in ("Pre-Depends", "Depends"):
        for group in re.split(r",\s*", fields.get(key, "")):
            if group:
                out.append([a.strip().split()[0].split(":")[0] for a in group.split("|")])
    return out


def resolve(roots, pkgs, provides, skip):
    want, todo = [], list(roots)
    seen = set()
    while todo:
        name = todo.pop()
        if name in seen or name in skip:
            continue
        seen.add(name)
        if name not in pkgs:
            if name in provides:
                todo.append(provides[name][0])
                continue
            print(f"mkroot: {name}: not in the index", file=sys.stderr)
            continue
        want.append(name)
        for group in deps(pkgs[name]):
            pick = next((a for a in group if a in pkgs or a in provides), None)
            if pick and not any(a in seen for a in group):
                todo.append(pick)
    return want


def unpack(deb, root):
    # a .deb is an ar archive: debian-binary, control.tar.*, data.tar.*
    data = open(deb, "rb").read()
    assert data[:8] == b"!<arch>\n"
    off = 8
    while off < len(data):
        name = data[off:off + 16].decode().strip().rstrip("/")
        size = int(data[off + 48:off + 58].decode().strip())
        body = data[off + 60:off + 60 + size]
        off += 60 + size + (size & 1)
        if name.startswith("data.tar"):
            if name.endswith(".zst"):
                body = subprocess.run(["zstd", "-dc"], input=body, capture_output=True, check=True).stdout
                mode = "r:"
            else:
                mode = "r:*"
            with tarfile.open(fileobj=io.BytesIO(body), mode=mode) as t:
                for m in t.getmembers():
                    # keep everything inside the root
                    if m.name.startswith("/") or ".." in m.name.split("/"):
                        continue
                    if m.isdev():
                        continue
                    dest = os.path.join(root, m.name)
                    if (m.isfile() or m.issym() or m.islnk()) and os.path.lexists(dest) and not os.path.isdir(dest):
                        os.unlink(dest)
                    t.extract(m, root, filter="fully_trusted")  # modes kept; owners only as root


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("packages", nargs="+")
    ap.add_argument("--arch", default="amd64")
    ap.add_argument("--suite", default="bookworm")
    ap.add_argument("--skip", default="", help="comma-separated packages to leave out")
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    cache = os.path.join(os.path.dirname(root), "debs")
    os.makedirs(root, exist_ok=True)
    os.makedirs(cache, exist_ok=True)
    pkgs, provides = index(a.suite, a.arch, cache)
    want = resolve(a.packages, pkgs, provides, set(filter(None, a.skip.split(","))))
    print(f"mkroot: {len(want)} packages")
    for name in sorted(want):
        f = pkgs[name]
        deb = os.path.join(cache, os.path.basename(f["Filename"]))
        if not os.path.exists(deb):
            data = fetch(f"{MIRROR}/{f['Filename']}")
            if hashlib.sha256(data).hexdigest() != f["SHA256"]:
                sys.exit(f"mkroot: {name}: sha256 mismatch")
            with open(deb, "wb") as out:
                out.write(data)
        unpack(deb, root)
    with open(os.path.join(root, "mkroot-packages.txt"), "w") as out:
        out.write("\n".join(f"{n} {pkgs[n]['Version']}" for n in sorted(want)) + "\n")


if __name__ == "__main__":
    main()
