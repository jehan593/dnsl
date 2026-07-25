#!/usr/bin/env bash
# One-line install/update/uninstall for dnsl, fetching the latest prebuilt release from GitHub.
# Always system-wide (/usr/local) and always run as root — the background daemon that does the
# actual DNS work needs root regardless (binding port 53, the resolve1 D-Bus calls), so there's no
# real no-root install path to offer here; keep it to one command.
#
#   curl -fsSL https://raw.githubusercontent.com/jehan593/dnsl/main/scripts/get.sh | sudo bash
#   curl -fsSL https://raw.githubusercontent.com/jehan593/dnsl/main/scripts/get.sh | sudo bash -s -- --uninstall
#
# This script only places the tray binary + assets on disk, same as install.sh; it never touches
# dnsl.service or /etc/dnsl/settings.json directly (install.sh --uninstall handles the former when
# run as root).
#
# Install and update are the same command: it always re-fetches the latest release and reinstalls
# over the previous copy. The extracted release is kept under CACHE_DIR so a later --uninstall (or
# a manual `make uninstall` from within that dir) works without re-downloading anything.
set -euo pipefail

REPO="jehan593/dnsl"
CACHE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/dnsl/current-release"
PREFIX="/usr/local"

if [ "${1:-}" = "--uninstall" ]; then
    if [ ! -x "$CACHE_DIR/install.sh" ]; then
        echo "error: no cached install found at $CACHE_DIR — nothing to uninstall via this script." >&2
        echo "If you installed a different way, run its install.sh --uninstall (or 'make uninstall') directly." >&2
        exit 1
    fi
    "$CACHE_DIR/install.sh" --uninstall "$PREFIX"
    exit 0
fi

arch="$(uname -m)"
if [ "$arch" != "x86_64" ]; then
    echo "error: no prebuilt release for architecture '$arch' yet." >&2
    echo "Clone https://github.com/$REPO and run 'make install' to build from source instead." >&2
    exit 1
fi

echo "Looking up the latest release of $REPO..."
asset_url=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" \
    | grep -o '"browser_download_url": *"[^"]*x86_64\.tar\.gz"' \
    | head -1 \
    | sed -E 's/.*"(https:[^"]+)"/\1/')

if [ -z "$asset_url" ]; then
    echo "error: couldn't find an x86_64 release asset for $REPO." >&2
    exit 1
fi

tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT

echo "Downloading $asset_url..."
curl -fsSL "$asset_url" -o "$tmp_dir/release.tar.gz"

echo "Extracting..."
tar -xzf "$tmp_dir/release.tar.gz" -C "$tmp_dir"
extracted_dir=$(find "$tmp_dir" -mindepth 1 -maxdepth 1 -type d)

rm -rf "$CACHE_DIR"
mkdir -p "$(dirname "$CACHE_DIR")"
mv "$extracted_dir" "$CACHE_DIR"

"$CACHE_DIR/install.sh" "$PREFIX"

echo
echo "To update later, just re-run this same command."
echo "To uninstall: curl -fsSL https://raw.githubusercontent.com/$REPO/main/scripts/get.sh | sudo bash -s -- --uninstall"
