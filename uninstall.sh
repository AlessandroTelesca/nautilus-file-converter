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

# What to remove: the two paths computed above, plus every installed copy that
# meson recorded in build/meson-logs/install-log.txt (one path per line, after
# comment lines starting with "#"), so a helper installed under an older prefix
# is removed too. Only absolute paths whose name is exactly one of the two files
# this project installs are taken from the log (an allow-list: nothing else in
# that file is ever removed), and never a path with a ".." component. A symlinked
# log is not read.
TARGETS=("$EXT_DIR/libnautilus-file-converter.so" "$LIBEXEC_DIR/nfc-decode")
LOG="$BUILD_DIR/meson-logs/install-log.txt"
if [ -f "$LOG" ] && [ ! -L "$LOG" ]; then
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            /*/libnautilus-file-converter.so | /*/nfc-decode) ;;
            *) continue ;;
        esac
        case "$line" in
            */../* | */..) continue ;;
        esac
        dup=0
        for t in "${TARGETS[@]}"; do
            [ "$t" = "$line" ] && dup=1
        done
        [ "$dup" -eq 0 ] && TARGETS+=("$line")
    done < "$LOG"
fi

sudo rm -f -- "${TARGETS[@]}"

nautilus -q || true
echo "removed:"
printf '  %s\n' "${TARGETS[@]}"
echo "Reopen Files."
