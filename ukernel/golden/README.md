# Golden microkernel images

Byte-for-byte references for the serial (non-overlap) microkernel image of each
core. The images themselves are never checked in — `ukernel/.gitignore` ignores
every built `*.bin` — so the golden is the image's **SHA-256 and size**.
`ukernel-golden` CI rebuilds the image from source and fails on any mismatch, so
refactors meant to preserve the image are caught automatically.

- `sgx545.sha256` — SGX545 serial image (14400 B / 1800 instructions), built by
  `ukernel/build.py` with `UK_OVERLAP` unset.

Verify locally:

    python3 ukernel/golden/check.py

Regenerate after an intentional change:

    python3 ukernel/golden/check.py --write

Debug a mismatch — build a reference from a known-good checkout, then diff:

    git worktree add /tmp/good <known-good-commit>
    python3 /tmp/good/ukernel/build.py /tmp/ref.bin /tmp/ref.json
    python3 ukernel/golden/check.py --ref /tmp/ref.bin   # prints the first differing word
