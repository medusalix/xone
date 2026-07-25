#!/usr/bin/env sh
# Install userspace overlay companion + apt dependencies (Debian/Ubuntu/Mint).
# Permissions (input group, udev) are handled by install/user-access.sh from install.sh.
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
OVERLAY_SRC="$ROOT/overlay"
DEST=/usr/local/share/xone-overlay
BIN=/usr/local/bin/xone-overlay
UNIT_DIR=/etc/systemd/user
UNIT_NAME=xone-overlay.service
APT_DEPS="$ROOT/install/apt-deps.sh"

if [ ! -d "$OVERLAY_SRC" ] || [ ! -f "$OVERLAY_SRC/xone-overlay.py" ]; then
    echo 'Overlay sources not found; skipping overlay install.' >&2
    exit 0
fi

if ! command -v apt-get >/dev/null 2>&1; then
    echo 'apt-get not found; skipping overlay package install.' >&2
    exit 0
fi

echo 'Installing xone overlay dependencies...'
# Keep a packaged copy for uninstall from any cwd
install -D -m 755 "$APT_DEPS" /usr/local/lib/xone/apt-deps.sh
sh "$APT_DEPS" install \
    python3-gi \
    python3-gi-cairo \
    gir1.2-gtk-3.0 \
    gir1.2-upowerglib-1.0 \
    python3-evdev \
    python3-websocket \
    gnome-screenshot

if ! command -v pactl >/dev/null 2>&1; then
    sh "$APT_DEPS" install pulseaudio-utils || true
fi
if ! command -v pactl >/dev/null 2>&1; then
    sh "$APT_DEPS" install pipewire-pulse || true
fi
if ! command -v pactl >/dev/null 2>&1; then
    echo 'Warning: could not install pactl provider; audio controls may not work.' >&2
fi

echo 'Installing xone overlay to '"$DEST"'...'
rm -rf "$DEST"
mkdir -p "$DEST"
cp -a "$OVERLAY_SRC/overlay" "$DEST/"
cp -a "$OVERLAY_SRC/data" "$DEST/"
cp -a "$OVERLAY_SRC/README.md" "$DEST/" 2>/dev/null || true
install -m 755 "$OVERLAY_SRC/xone-overlay.py" "$DEST/xone-overlay.py"

cat > "$BIN" << 'EOF'
#!/bin/sh
DEST=/usr/local/share/xone-overlay
STATE="${XDG_STATE_HOME:-$HOME/.local/state}/xone-overlay"
mkdir -p "$STATE"
export PYTHONUNBUFFERED=1
[ -n "${DISPLAY:-}" ] || DISPLAY=:0
export DISPLAY
exec python3 "$DEST/xone-overlay.py" "$@"
EOF
chmod 755 "$BIN"

install -D -m 644 "$OVERLAY_SRC/data/xone-overlay.service" "$UNIT_DIR/$UNIT_NAME"

TARGET_USER="${SUDO_USER:-}"
if [ -n "$TARGET_USER" ] && [ "$TARGET_USER" != root ]; then
    HOME_DIR="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
    UID_NUM="$(id -u "$TARGET_USER")"
    RUNTIME_DIR="/run/user/$UID_NUM"

    if [ -n "$HOME_DIR" ] && [ -d "$HOME_DIR" ]; then
        autostart_dir="$HOME_DIR/.config/autostart"
        mkdir -p "$autostart_dir"
        sed "s|^Exec=.*|Exec=$BIN|" \
            "$OVERLAY_SRC/data/xone-overlay.desktop" \
            > "$autostart_dir/xone-overlay.desktop"
        if [ -d "$HOME_DIR/.config" ]; then
            chown "$TARGET_USER:" "$HOME_DIR/.config" 2>/dev/null || true
        fi
        chown "$TARGET_USER:" "$autostart_dir" 2>/dev/null || true
        chown "$TARGET_USER:" "$autostart_dir/xone-overlay.desktop" 2>/dev/null \
            || chown "$TARGET_USER" "$autostart_dir/xone-overlay.desktop"
        echo "Installed Cinnamon autostart: $autostart_dir/xone-overlay.desktop"
    fi

    if [ -d "$RUNTIME_DIR" ] && command -v systemctl >/dev/null 2>&1; then
        if sudo -u "$TARGET_USER" env XDG_RUNTIME_DIR="$RUNTIME_DIR" \
            systemctl --user daemon-reload \
            && sudo -u "$TARGET_USER" env XDG_RUNTIME_DIR="$RUNTIME_DIR" \
                systemctl --user enable --now "$UNIT_NAME"; then
            echo "Enabled systemd --user service: $UNIT_NAME"
        else
            echo "Warning: could not enable systemd --user service now; it will apply after re-login." >&2
        fi
    else
        echo "Note: enable after login with: systemctl --user enable --now $UNIT_NAME"
    fi
else
    echo "Run as sudo from your user account to enable autostart and the systemd user service."
fi

echo "xone overlay ready."
echo "  Status:  systemctl --user status xone-overlay"
echo "  Logs:    ~/.local/state/xone-overlay/overlay.log"
