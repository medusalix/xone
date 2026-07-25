#!/usr/bin/env sh
# Manage user permissions for xone (input group, udev, session markers).
# Usage:
#   sh install/user-access.sh install
#   sh install/user-access.sh remove
set -eu

ACTION="${1:-}"
STATE_DIR=/var/lib/xone
UDEV_RULE=/etc/udev/rules.d/99-xone.rules
ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
# Prefer packaged copy under /usr/local/lib/xone when uninstalling outside the git tree
if [ -f /usr/local/lib/xone/udev-xone.rules ]; then
    UDEV_SRC=/usr/local/lib/xone/udev-xone.rules
else
    UDEV_SRC="$ROOT/install/udev-xone.rules"
fi


TARGET_USER="${SUDO_USER:-}"
if [ -z "$TARGET_USER" ] || [ "$TARGET_USER" = root ]; then
    TARGET_USER=""
fi

marker_for() {
    echo "$STATE_DIR/added-input-group-$1"
}

install_udev() {
    if [ ! -f "$UDEV_SRC" ]; then
        echo "Warning: missing $UDEV_SRC" >&2
        return 0
    fi
    install -D -m 644 "$UDEV_SRC" "$UDEV_RULE"
    if command -v udevadm >/dev/null 2>&1; then
        udevadm control --reload-rules || true
        udevadm trigger --subsystem-match=usb --attr-match=idVendor=045e || true
        udevadm trigger --subsystem-match=input || true
    fi
    echo "Installed udev rules: $UDEV_RULE"
}

remove_udev() {
    rm -f "$UDEV_RULE"
    # Legacy rule name from earlier builds
    rm -f /etc/udev/rules.d/99-xone-dongle.rules
    if command -v udevadm >/dev/null 2>&1; then
        udevadm control --reload-rules || true
        udevadm trigger --subsystem-match=usb --attr-match=idVendor=045e || true
        udevadm trigger --subsystem-match=input || true
    fi
}

add_input_group() {
    if [ -z "$TARGET_USER" ]; then
        echo "Note: run install via sudo from your user account to join group 'input'."
        return 0
    fi
    if ! getent group input >/dev/null 2>&1; then
        echo "Warning: group 'input' does not exist on this system." >&2
        return 0
    fi

    mkdir -p "$STATE_DIR"
    marker="$(marker_for "$TARGET_USER")"

    if id -nG "$TARGET_USER" 2>/dev/null | tr ' ' '\n' | grep -qx input; then
        if [ ! -f "$marker" ]; then
            # Already a member before xone — do not claim ownership for uninstall
            echo "User $TARGET_USER already in group 'input' (leaving as-is on uninstall)."
        else
            echo "User $TARGET_USER already in group 'input'."
        fi
        return 0
    fi

    usermod -aG input "$TARGET_USER"
    echo 1 > "$marker"
    chmod 644 "$marker"
    echo "Added $TARGET_USER to group 'input'."
    echo "Log out and back in (or reboot) so the new group applies to your desktop session."
}

remove_input_group() {
    if [ -z "$TARGET_USER" ]; then
        return 0
    fi
    marker="$(marker_for "$TARGET_USER")"
    if [ ! -f "$marker" ]; then
        echo "Group 'input': not removing for $TARGET_USER (not added by xone install)."
        return 0
    fi
    if getent group input >/dev/null 2>&1; then
        if command -v gpasswd >/dev/null 2>&1; then
            gpasswd -d "$TARGET_USER" input 2>/dev/null || true
        elif command -v deluser >/dev/null 2>&1; then
            deluser "$TARGET_USER" input 2>/dev/null || true
        else
            # Fallback: rewrite group list without input
            echo "Warning: could not remove $TARGET_USER from group input automatically." >&2
        fi
        echo "Removed $TARGET_USER from group 'input'."
    fi
    rm -f "$marker"
    # Clean empty state dir
    rmdir "$STATE_DIR" 2>/dev/null || true
}

remove_user_session_bits() {
    if [ -z "$TARGET_USER" ]; then
        return 0
    fi
    HOME_DIR="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
    UID_NUM="$(id -u "$TARGET_USER" 2>/dev/null || true)"
    RUNTIME_DIR="/run/user/${UID_NUM:-}"

    if [ -n "$UID_NUM" ] && [ -d "$RUNTIME_DIR" ] && command -v systemctl >/dev/null 2>&1; then
        sudo -u "$TARGET_USER" env XDG_RUNTIME_DIR="$RUNTIME_DIR" \
            systemctl --user disable --now xone-overlay.service 2>/dev/null || true
        sudo -u "$TARGET_USER" env XDG_RUNTIME_DIR="$RUNTIME_DIR" \
            systemctl --user daemon-reload 2>/dev/null || true
    fi

    if [ -n "$HOME_DIR" ] && [ -d "$HOME_DIR" ]; then
        rm -f "$HOME_DIR/.config/autostart/xone-overlay.desktop"
        rm -f "$HOME_DIR/.config/systemd/user/xone-overlay.service"
        rm -rf "$HOME_DIR/.config/systemd/user/xone-overlay.service.d"
        # Enable symlinks left behind if systemctl --user disable could not run
        find "$HOME_DIR/.config/systemd/user" -type l -name 'xone-overlay.service' \
            -delete 2>/dev/null || true
        # Runtime prefs / logs created by the overlay (not system packages)
        rm -rf "$HOME_DIR/.config/xone-overlay"
        rm -rf "$HOME_DIR/.local/state/xone-overlay"
    fi
}

case "$ACTION" in
install)
    install_udev
    add_input_group
    ;;
remove)
    remove_user_session_bits
    remove_input_group
    remove_udev
    ;;
*)
    echo "Usage: $0 {install|remove}" >&2
    exit 1
    ;;
esac
