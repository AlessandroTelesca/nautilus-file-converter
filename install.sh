#!/usr/bin/env bash
# Build the C extension as the current user and install it system-wide.
#
# Only the final `meson install --no-rebuild` runs under sudo (the extension
# dir and libexecdir are root-owned). Compiling never does, so ./build never
# ends up with root-owned ninja logs or objects.
set -euo pipefail

if [ "$(id -u)" -eq 0 ]; then
    echo "Do not run this script as root or with sudo: it calls sudo itself, only for" >&2
    echo "the install step, so that ./build stays owned by you." >&2
    exit 1
fi

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$REPO_DIR/build"

cd "$REPO_DIR"

# --prefix=/usr: the helper goes to /usr/libexec, next to the system-wide extension
if [ -d "$BUILD_DIR" ]; then
    meson setup "$BUILD_DIR" --reconfigure --buildtype=release --prefix=/usr
else
    meson setup "$BUILD_DIR" --buildtype=release --prefix=/usr
fi
meson compile -C "$BUILD_DIR"

# Installs libnautilus-file-converter.so (Nautilus extension dir) and the
# nfc-decode helper (libexecdir). --no-rebuild: do not run ninja as root.
sudo meson install --no-rebuild -C "$BUILD_DIR"
# meson install writes meson-logs/install-log.txt as root; give it back. Never
# through a symlink (a planted link there must not make sudo chown its target):
# a symlink is skipped, and chown -h does not follow one that appears meanwhile.
LOG="$BUILD_DIR/meson-logs/install-log.txt"
if [ -e "$LOG" ] && [ ! -L "$LOG" ]; then
    sudo chown -h "$(id -u):$(id -g)" "$LOG" 2>/dev/null || true
fi

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
