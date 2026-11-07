#!/bin/sh
# Print the link flags needed to produce a dynamic aarch64-musl binary from
# a scratch sysroot (used with LDFLAGS="-nostdlib").
#
# usage: musl-link-flags.sh <sysroot>
set -eu

SYSROOT=${1:?usage: musl-link-flags.sh <sysroot>}
GCCDIR=$(ls -d "$SYSROOT"/usr/lib/gcc/*/*/ 2>/dev/null | head -1)

if [ -z "$GCCDIR" ]; then
	echo "error: no gcc crt files in $SYSROOT" >&2
	exit 1
fi

printf '%s\n' \
	"$SYSROOT/usr/lib/crt1.o" \
	"$SYSROOT/usr/lib/crti.o" \
	"${GCCDIR}crtbegin.o" \
	"-L$SYSROOT/usr/lib" \
	"-lX11" "-lm" "-lc" \
	"${GCCDIR}crtend.o" \
	"$SYSROOT/usr/lib/crtn.o"
