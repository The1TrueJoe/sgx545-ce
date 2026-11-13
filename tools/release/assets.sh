#!/bin/sh
# A release's files, as GitHub serves them: the .debs with each ~ renamed to a dot
# (GitHub drops a ~ from asset names), then SHA256SUMS over those names.
#
#   tools/release/assets.sh DIR
set -eu
cd "$1"
rm -f SHA256SUMS
for f in *.deb; do
	case $f in *'~'*) mv -- "$f" "$(printf '%s' "$f" | tr '~' '.')" ;; esac
done
sha256sum -- *.deb > SHA256SUMS
cat SHA256SUMS
