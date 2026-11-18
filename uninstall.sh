#!/usr/bin/env bash
# Remove the installed extension and its helper (sudo only for the rm: both live
# in root-owned directories). Nothing is built or run as root, so ./build is
# left alone.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$REPO_DIR/build"

# where the extension went: pkg-config's extensiondir, as at install time
EXT_DIR="$(pkg-config --variable=extensiondir libnautilus-extension-4 2>/dev/null || true)"
EXT_DIR="${EXT_DIR:-/usr/lib64/nautilus/extensions-4}"

# where the helper went: <prefix>/<libexecdir> of the configured build, else
# the meson defaults (/usr/libexec)
PREFIX=/usr
LIBEXEC=libexec
if [ -f "$BUILD_DIR/build.ninja" ] && command -v meson >/dev/null 2>&1; then
    OPTS="$(meson configure "$BUILD_DIR" 2>/dev/null || true)"
    P="$(printf '%s\n' "$OPTS" | awk '$1 == "prefix" { print $2; exit }')"
    L="$(printf '%s\n' "$OPTS" | awk '$1 == "libexecdir" { print $2; exit }')"
    PREFIX="${P:-$PREFIX}"
    LIBEXEC="${L:-$LIBEXEC}"
fi
case "$LIBEXEC" in
    /*) LIBEXEC_DIR="$LIBEXEC" ;;
    *)  LIBEXEC_DIR="$PREFIX/$LIBEXEC" ;;
esac

sudo rm -f -- "$EXT_DIR/libnautilus-file-converter.so" "$LIBEXEC_DIR/nfc-decode"

nautilus -q || true
echo "removed $EXT_DIR/libnautilus-file-converter.so and $LIBEXEC_DIR/nfc-decode. Reopen Files."
