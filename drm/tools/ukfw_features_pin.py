#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""Pin the shipped firmware image's features word to the driver's accepted mask.

The image declares capability bits in its mkukfw header (PVRSGX_UKFW_*). On load
the driver rejects any host-visible bit (0-15) it does not know, so a feature the
image turns on before the driver learns it is a clean probe failure, not a hang
(docs/UKFW-FEATURES-MIGRATION.md). This check keeps the two in step inside one
tree: the shipped image's host-visible features must be a subset of the in-tree
driver's UKFW_FEATURES. Private bits 16-31 are the microkernel's own and ignored.

    ukfw_features_pin.py                     build the default image, check it
    ukfw_features_pin.py --image FILE        check a built sgx545_ukernel.bin
    ukfw_features_pin.py --features N         check a literal features word
    ukfw_features_pin.py --driver DIR         driver dir (default drm/powervr_sgx)
    ukfw_features_pin.py --kernel-mask N      override the accepted mask (testing)
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import mkukfw  # noqa: E402  (HEADER layout; same dir)

HOST_VISIBLE = 0xFFFF  # bits 0-15; 16-31 are UKFW_FEATURES_PRIVATE, always ignored


def driver_mask(driver_dir):
    """The driver's host-visible accepted mask (UKFW_FEATURES), as {mask, names}."""
    h = open(os.path.join(driver_dir, "pvrsgx_fw.h")).read()
    c = open(os.path.join(driver_dir, "pvrsgx_fw.c")).read()
    bits = {m.group(1): 1 << int(m.group(2))
            for m in re.finditer(r"#define\s+(PVRSGX_UKFW_\w+)\s+BIT\((\d+)\)", h)}
    body = macro_body(c, "UKFW_FEATURES")
    if body is None:
        sys.exit("error: no #define UKFW_FEATURES in pvrsgx_fw.c")
    mask, names = 0, []
    for tok in re.findall(r"PVRSGX_UKFW_\w+", body):
        if tok not in bits:
            sys.exit(f"error: UKFW_FEATURES names {tok}, undefined in pvrsgx_fw.h")
        mask |= bits[tok]
        names.append(tok)
    return mask, names, bits


def macro_body(text, name):
    """The right-hand side of `#define <name> ...`, across backslash continuation."""
    m = re.search(r"#define\s+" + re.escape(name) + r"\b(.*)", text)
    if not m:
        return None
    line, rest = m.group(1), text[m.end():]
    while line.rstrip().endswith("\\"):
        nl = rest.split("\n", 1)
        line = line.rstrip()[:-1] + nl[0]
        rest = nl[1] if len(nl) > 1 else ""
    return line


def image_features(path):
    """The features word from a built sgx545_ukernel.bin (mkukfw header)."""
    data = open(path, "rb").read()
    f = mkukfw.HEADER.unpack_from(data)
    if tuple(f[0:2]) != mkukfw.MAGIC:
        sys.exit(f"error: {path} is not a microkernel file (no magic)")
    return f[4]  # magic0, magic1, header_size, interface, features, ...


def default_features():
    """Build the default image and read its declared features from the manifest."""
    import json
    fd, binp = tempfile.mkstemp(suffix=".bin")
    os.close(fd)
    jsonp = binp[:-4] + ".json"
    env = dict(os.environ)
    env.pop("UK_OVERLAP", None)
    subprocess.run([sys.executable, os.path.join(REPO, "ukernel", "build.py"), binp, jsonp],
                   check=True, stdout=subprocess.DEVNULL, env=env)
    feat = json.load(open(jsonp))["features"]
    for p in (binp, jsonp, binp + ".pat"):
        if os.path.exists(p):
            os.remove(p)
    return feat


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image")
    ap.add_argument("--features", type=lambda s: int(s, 0))
    ap.add_argument("--driver", default=os.path.join(REPO, "drm", "powervr_sgx"))
    ap.add_argument("--kernel-mask", type=lambda s: int(s, 0))
    a = ap.parse_args()

    if a.kernel_mask is not None:
        mask, names, bits = a.kernel_mask, [], {}
    else:
        mask, names, bits = driver_mask(a.driver)

    if a.features is not None:
        feat = a.features
    elif a.image:
        feat = image_features(a.image)
    else:
        feat = default_features()

    host = feat & HOST_VISIBLE
    rev = {v: k for k, v in bits.items()}

    def show(m):
        ns = [rev.get(1 << i, f"BIT({i})") for i in range(16) if m & (1 << i)]
        return f"{m:#06x}" + (f" ({' | '.join(ns)})" if ns else "")

    print(f"image features   {feat:#06x}  host-visible {show(host)}")
    print(f"driver accepts   {show(mask)}" + (f"  [{', '.join(names)}]" if names else ""))

    missing = host & ~mask
    if missing:
        print(f"error: image declares host-visible features the driver does not accept: "
              f"{show(missing)}", file=sys.stderr)
        print("the shipped image and the in-tree driver are out of step; a kernel with this "
              "mask would reject this image at probe (docs/UKFW-FEATURES-MIGRATION.md)",
              file=sys.stderr)
        return 1
    print("ok: shipped image features are a subset of the driver's accepted mask")
    return 0


if __name__ == "__main__":
    sys.exit(main())
