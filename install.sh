#!/usr/bin/env bash
# Installs/uninstalls the dnsl tray binary + assets — mirrors linker-linux's install.sh shape.
# This does NOT touch the systemd service: dnsl's own installer.c registers/starts dnsl.service
# itself (via a one-time pkexec prompt) the first time the tray can't reach it — see CLAUDE.md
# "Why a systemd service". This script only places files on disk and never needs to run as root
# for the default per-user PREFIX; use a system PREFIX (e.g. /usr/local, this Makefile's default)
# with sudo if you want dnsl available for every user on the machine.
#
# Usage:
#   ./install.sh [PREFIX]              (default: /usr/local)
#   ./install.sh --uninstall [PREFIX]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

MODE="install"
PREFIX="/usr/local"
if [ "${1:-}" = "--uninstall" ]; then
    MODE="uninstall"
    PREFIX="${2:-/usr/local}"
elif [ -n "${1:-}" ]; then
    PREFIX="$1"
fi

BIN_DIR="$PREFIX/bin"
ASSETS_DIR="$PREFIX/share/dnsl"
APPS_DIR="$PREFIX/share/applications"
ICON_THEME_DIR="$PREFIX/share/icons/hicolor"
DESKTOP_FILE="$APPS_DIR/dnsl.desktop"

if [ "$MODE" = "uninstall" ]; then
    echo "Uninstalling dnsl from $PREFIX..."
    if systemctl is-enabled dnsl.service >/dev/null 2>&1 || systemctl is-active dnsl.service >/dev/null 2>&1; then
        echo "note: dnsl.service is still installed — run as root:"
        echo "  systemctl disable --now dnsl.service && rm -f /etc/systemd/system/dnsl.service && systemctl daemon-reload"
    fi
    rm -f "$BIN_DIR/dnsl"
    rm -rf "$ASSETS_DIR/fonts" "$ASSETS_DIR/icons"
    rmdir "$ASSETS_DIR" 2>/dev/null || true
    rm -f "$DESKTOP_FILE"
    for size in 16 32 48 256; do
        rm -f "$ICON_THEME_DIR/${size}x${size}/apps/dnsl.png"
    done
    command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPS_DIR" >/dev/null 2>&1 || true
    command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -t "$ICON_THEME_DIR" >/dev/null 2>&1 || true
    echo "Done. /etc/dnsl/settings.json (if any) was left alone."
    exit 0
fi

if [ ! -x "$SCRIPT_DIR/dnsl" ]; then
    echo "error: dnsl binary not found next to install.sh — run 'make' first" >&2
    exit 1
fi

echo "Installing dnsl to $PREFIX..."
mkdir -p "$BIN_DIR" "$ASSETS_DIR/fonts" "$ASSETS_DIR/icons" "$APPS_DIR"

install -m 755 "$SCRIPT_DIR/dnsl" "$BIN_DIR/dnsl"
cp "$SCRIPT_DIR"/data/fonts/*.ttf "$ASSETS_DIR/fonts/"
cp "$SCRIPT_DIR"/data/fonts/MARTIAN_MONO_LICENSE.txt "$ASSETS_DIR/fonts/" 2>/dev/null || true
cp "$SCRIPT_DIR"/data/icons/*.png "$ASSETS_DIR/icons/"

for size in 16 32 48 256; do
    dir="$ICON_THEME_DIR/${size}x${size}/apps"
    mkdir -p "$dir"
    cp "$SCRIPT_DIR/data/icons/dnsl-enabled-${size}.png" "$dir/dnsl.png"
done

cat > "$DESKTOP_FILE" <<EOF
[Desktop Entry]
Type=Application
Name=dnsl
Comment=DNS-over-TLS tray
Exec=$BIN_DIR/dnsl
Icon=dnsl
Terminal=false
Categories=Network;
NoDisplay=false
EOF

command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPS_DIR" >/dev/null 2>&1 || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -t "$ICON_THEME_DIR" >/dev/null 2>&1 || true

echo "Installed."
echo "Make sure $BIN_DIR is on your PATH, then run 'dnsl' — or find it in your applications menu."
echo "First launch will prompt once (via pkexec) to install/start the dnsl background service."
