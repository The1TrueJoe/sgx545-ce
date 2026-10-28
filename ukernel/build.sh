#!/bin/sh
# Build openHC's microkernel and run its offline tests.
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
python3 "$R/ukernel/build.py"
python3 "$R/ukernel/test_emu.py"
