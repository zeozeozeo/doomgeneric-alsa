#!/bin/sh
# Build a minimal aarch64-musl sysroot out of Alpine packages.
#
# Only the pieces this build needs are fetched: musl headers/runtime, X11
# headers and libraries, and GCC's crt objects. No compiler and no full
# Alpine userland.
#
# usage: build-scratch-sysroot.sh [sysroot-dir]
set -eu

SYSROOT=${1:-/tmp/doomgeneric-aarch64-sysroot}
ALPINE_RELEASE=${ALPINE_RELEASE:-v3.24}
ARCH=aarch64
BASE="https://dl-cdn.alpinelinux.org/alpine/$ALPINE_RELEASE"

PKGS="musl musl-dev libx11 libx11-dev libxcb libxcb-dev xorgproto xtrans xcb-proto linux-headers gcc libgcc"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

: > "$tmp/index"

for repo in main community; do
	url="$BASE/$repo/$ARCH/APKINDEX.tar.gz"
	if ! curl -fsSL -o "$tmp/$repo.tar.gz" "$url"; then
		echo "warning: cannot fetch $url" >&2
		continue
	fi
	tar -xzf "$tmp/$repo.tar.gz" -C "$tmp" APKINDEX
	awk -v repo="$repo" '
		/^P:/ { name = substr($0, 3) }
		/^V:/ { ver = substr($0, 3) }
		/^$/ {
			if (name != "") print name, ver, repo
			name = ""
			ver = ""
		}
	' "$tmp/APKINDEX" >> "$tmp/index"
done

mkdir -p "$SYSROOT"

for pkg in $PKGS; do
	line=$(awk -v p="$pkg" '$1 == p { print; exit }' "$tmp/index")
	if [ -z "$line" ]; then
		echo "error: package '$pkg' not found in the index" >&2
		exit 1
	fi

	set -- $line
	ver=$2
	repo=$3
	file="$pkg-$ver.apk"

	echo "  $file"
	curl -fsSL -o "$tmp/$file" "$BASE/$repo/$ARCH/$file"
	# apk packages are gzipped tarballs with some extra headers.
	tar -xzf "$tmp/$file" -C "$SYSROOT" 2>/dev/null || true
done

# apk metadata has no business in a sysroot
find "$SYSROOT" -maxdepth 1 -name '.PKGINFO' -delete 2>/dev/null || true
find "$SYSROOT" -maxdepth 1 -name '.SIGN.*' -delete 2>/dev/null || true

echo "sysroot ready: $SYSROOT"
