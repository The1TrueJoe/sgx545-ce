#!/bin/sh
# Build the signed apt repository (Debian 13, amd64 and i386) from releases' .debs.
#
#   tools/release/apt-repo.sh SITE FPR DIR...
#
# Each DIR holds one release's .debs, oldest release first, so every package ends at
# its newest version. FPR is a secret key in $GNUPGHOME. Afterwards SITE holds
# dists/trixie, pool/main, and the public key as powervr-sgx.gpg (binary) and
# powervr-sgx.asc (armored); the signatures are checked against that file.
# Every package of the last DIR must be in the repository, for each architecture
# it serves (an Architecture: all package for both).
set -eu
R=$1 FPR=$2
shift 2
mkdir -p "$R/conf"
cat > "$R/conf/distributions" <<EOF
Origin: powervr_sgx
Label: powervr_sgx
Codename: trixie
Suite: trixie
Architectures: amd64 i386
Components: main
Description: The open PowerVR SGX545 stack for Debian 13
SignWith: $FPR
EOF
last=
for d in "$@"; do
	for f in "$d"/*.deb; do
		[ -e "$f" ] || continue
		# an older release's package that a newer one already replaced is refused; the
		# check below makes sure the newest release's packages all went in
		reprepro -b "$R" --ignore=wrongdistribution includedeb trixie "$f" || true
	done
	last=$d
done
reprepro -b "$R" export trixie
reprepro -b "$R" list trixie | tee "$R.list"
[ -n "$last" ]
for f in "$last"/*.deb; do
	[ -e "$f" ] || continue
	p=$(dpkg-deb -f "$f" Package) v=$(dpkg-deb -f "$f" Version) a=$(dpkg-deb -f "$f" Architecture)
	case $a in all) as="amd64 i386" ;; *) as=$a ;; esac
	for a in $as; do
		grep -qxF "trixie|main|$a: $p $v" "$R.list" ||
			{ echo "apt-repo.sh: $p $v ($a) from $last is not in the repository" >&2; exit 1; }
	done
done
rm -rf "$R/conf" "$R/db" "$R.list"
gpg --batch --export "$FPR" > "$R/powervr-sgx.gpg"
gpg --batch --armor --export "$FPR" > "$R/powervr-sgx.asc"
gpgv --keyring "$R/powervr-sgx.gpg" "$R/dists/trixie/InRelease"
gpgv --keyring "$R/powervr-sgx.gpg" "$R/dists/trixie/Release.gpg" "$R/dists/trixie/Release"
printf 'The powervr_sgx apt repository. Setup: docs/INSTALL.md.\n' > "$R/index.html"
find "$R" -type f | sort
