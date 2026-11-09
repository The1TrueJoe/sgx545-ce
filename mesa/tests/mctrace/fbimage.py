#!/usr/bin/env python3
# Copyright © 2026 Joseph Telaak
# SPDX-License-Identifier: MIT
"""The colour image of an apitrace state dump (eglretrace -D) as a PNG.

    fbimage.py DUMP.json OUT.png [BUFFER]     (BUFFER: GL_BACK by default)

The dumps can hold raw control characters in strings (shader sources), so
they are read without strict JSON, and only the first document is read
(gpuwatch.sh's status line follows it), without gl4es' log lines.
"""
import base64
import json
import re
import sys


def main():
    dump, out = sys.argv[1], sys.argv[2]
    buf = sys.argv[3] if len(sys.argv) > 3 else "GL_BACK"
    with open(dump, encoding="utf-8", errors="replace") as f:
        s = f.read()
    # gl4es logs to stdout, before the dump and in the middle of it
    s = re.sub(r"(LIBGL: |Stubbed )[^\n]*\n", "", s)
    d, _ = json.JSONDecoder(strict=False).raw_decode(s[s.index("{\n"):])
    fb = d.get("framebuffer", {})
    if buf not in fb:
        sys.exit(f"{dump}: no {buf} (have {', '.join(fb)})")
    with open(out, "wb") as f:
        f.write(base64.b64decode(fb[buf]["__data__"]))


if __name__ == "__main__":
    main()
