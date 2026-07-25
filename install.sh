#!/usr/bin/env bash
# Installs/uninstalls the dnsl tray binary + assets — mirrors linker-linux's install.sh shape.
# Install never touches the systemd service: dnsl's own installer.c registers/starts dnsl.service
# itself (via a one-time pkexec prompt) the first time the tray can't reach it — see CLAUDE.md
# "Why a systemd service". This script only places files on disk and never needs to run as root
# for the default per-user PREFIX; use a system PREFIX (e.g. /usr/local, this Makefile's default)
# with sudo if you want dnsl available for every user on the machine.
#
# Uninstall DOES tear the service down too, but only when run as root (it can't sudo internally
# without surprising a piped `curl | bash` — if run unprivileged, it just prints the two commands
# to do it yourself). Since the default PREFIX (/usr/local) already needs root to remove its own
# files, `sudo ./install.sh --uninstall` gets a fully automatic teardown in the common case.
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

# Kills any running tray instances (but never a live `--daemon` service, which systemd owns and
# would otherwise be left in a stopped state until manually restarted — see below). Needed because
# GtkApplication's single-instance mode means a tray process left running across an
# uninstall/install cycle just gets silently re-activated by the next launch instead of a fresh
# process starting, and after this script replaces the on-disk binary that stale process's
# /proc/self/exe resolves to "<path> (deleted)" — which breaks its own "Install / start background
# service…" menu item (pkexec can't exec a path that no longer exists).
kill_running_tray_instances() {
    local pid
    for pid in $(pgrep -x dnsl 2>/dev/null || true); do
        if ! grep -qz -- '--daemon' "/proc/$pid/cmdline" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
        fi
    done
}

BIN_DIR="$PREFIX/bin"
ASSETS_DIR="$PREFIX/share/dnsl"
APPS_DIR="$PREFIX/share/applications"
ICON_THEME_DIR="$PREFIX/share/icons/hicolor"
DESKTOP_FILE="$APPS_DIR/dnsl.desktop"

if [ "$MODE" = "uninstall" ]; then
    echo "Uninstalling dnsl from $PREFIX..."
    kill_running_tray_instances
    if systemctl is-enabled dnsl.service >/dev/null 2>&1 || systemctl is-active dnsl.service >/dev/null 2>&1; then
        if [ "$(id -u)" -eq 0 ]; then
            echo "Stopping and removing dnsl.service..."
            systemctl disable --now dnsl.service >/dev/null 2>&1 || true
            rm -f /etc/systemd/system/dnsl.service
            systemctl daemon-reload
        else
            echo "note: dnsl.service is still installed — run as root:"
            echo "  systemctl disable --now dnsl.service && rm -f /etc/systemd/system/dnsl.service && systemctl daemon-reload"
        fi
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
kill_running_tray_instances
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
