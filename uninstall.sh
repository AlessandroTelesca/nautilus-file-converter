#!/usr/bin/env bash
# Remove the installed extension (needs sudo: the extension dir is root-owned).
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$REPO_DIR/build"

if [ -d "$BUILD_DIR" ] && [ -f "$BUILD_DIR/build.ninja" ]; then
    sudo ninja -C "$BUILD_DIR" uninstall
else
    EXT_DIR="$(pkg-config --variable=extensiondir libnautilus-extension-4 2>/dev/null || echo /usr/lib64/nautilus/extensions-4)"
    sudo rm -f -- "$EXT_DIR/libnautilus-file-converter.so"
fi

nautilus -q || true
echo "removed. Reopen Files."
