#!/bin/sh
# Cross-build for aarch64/musl (Alpine-style) without a full cross
# toolchain: clang targets aarch64-linux-musl and links against a scratch
# sysroot assembled from Alpine packages.
#
# usage: SYSROOT=/tmp/mysysroot scripts/build-aarch64.sh
set -eu

cd "$(dirname "$0")/.."

SYSROOT=${SYSROOT:-/tmp/doomgeneric-aarch64-sysroot}

if [ ! -f "$SYSROOT/usr/lib/crt1.o" ]; then
	scripts/build-scratch-sysroot.sh "$SYSROOT"
fi

make clean
make \
	CC="clang --target=aarch64-linux-musl --sysroot=$SYSROOT -fuse-ld=lld" \
	LDFLAGS="-nostdlib" \
	LIBS="$(scripts/musl-link-flags.sh "$SYSROOT")"

file doomgeneric
