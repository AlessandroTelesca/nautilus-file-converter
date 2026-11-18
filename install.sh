#!/usr/bin/env bash
# Build the C extension and install it into Nautilus' system extension dir.
# Needs sudo for the final `meson install` (the extension dir is root-owned).
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$REPO_DIR/build"

cd "$REPO_DIR"

if [ -d "$BUILD_DIR" ]; then
    meson setup "$BUILD_DIR" --reconfigure --buildtype=release
else
    meson setup "$BUILD_DIR" --buildtype=release
fi
meson compile -C "$BUILD_DIR"

# `sudo meson install` would re-run as root; do it explicitly.
sudo meson install -C "$BUILD_DIR"

# The old nautilus-python version (a symlink or a copy) would make the menu
# appear twice; remove it if present. No sudo needed, it lives in $HOME.
OLD="${XDG_DATA_HOME:-$HOME/.local/share}/nautilus-python/extensions/nautilus_file_converter.py"
if [ -e "$OLD" ] || [ -L "$OLD" ]; then
    rm -f -- "$OLD"
    echo "removed old extension: $OLD"
fi

echo "installed: $(pkg-config --variable=extensiondir libnautilus-extension-4)/libnautilus-file-converter.so"
nautilus -q || true
echo "Nautilus was restarted; reopen Files to use the new Convert to menu."
