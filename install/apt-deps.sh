#!/usr/bin/env sh
# Track apt packages installed by xone so uninstall can remove only what we added.
# Usage:
#   sh install/apt-deps.sh install pkg1 pkg2 ...
#   sh install/apt-deps.sh remove
set -eu

STATE_DIR=/var/lib/xone
LIST_FILE="$STATE_DIR/apt-packages.list"
ACTION="${1:-}"
shift || true

pkg_is_installed() {
    dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q "install ok installed"
}

record_pkg() {
    mkdir -p "$STATE_DIR"
    touch "$LIST_FILE"
    if ! grep -qx "$1" "$LIST_FILE" 2>/dev/null; then
        echo "$1" >> "$LIST_FILE"
    fi
}

install_pkgs() {
    if [ "$#" -eq 0 ]; then
        return 0
    fi
    if ! command -v apt-get >/dev/null 2>&1; then
        echo 'apt-get not found; skipping package install.' >&2
        return 0
    fi

    missing=""
    for pkg in "$@"; do
        if pkg_is_installed "$pkg"; then
            echo "apt: $pkg already installed (leaving for uninstall)"
        else
            missing="$missing $pkg"
        fi
    done
    # shellcheck disable=SC2086
    missing=$(echo $missing)
    if [ -z "$missing" ]; then
        return 0
    fi

    echo "apt: installing$missing ..."
    # shellcheck disable=SC2086
    DEBIAN_FRONTEND=noninteractive apt-get install -y $missing
    for pkg in $missing; do
        if pkg_is_installed "$pkg"; then
            record_pkg "$pkg"
        fi
    done
}

remove_tracked() {
    if [ ! -f "$LIST_FILE" ]; then
        echo 'apt: no xone-tracked packages to remove.'
        return 0
    fi
    if ! command -v apt-get >/dev/null 2>&1; then
        rm -f "$LIST_FILE"
        return 0
    fi

    pkgs=""
    while IFS= read -r pkg || [ -n "$pkg" ]; do
        [ -z "$pkg" ] && continue
        if pkg_is_installed "$pkg"; then
            pkgs="$pkgs $pkg"
        fi
    done < "$LIST_FILE"

    # shellcheck disable=SC2086
    pkgs=$(echo $pkgs)
    if [ -n "$pkgs" ]; then
        echo "apt: removing packages installed by xone:$pkgs"
        # shellcheck disable=SC2086
        DEBIAN_FRONTEND=noninteractive apt-get remove -y $pkgs || true
        # Drop unused deps pulled only by those packages (safe-ish)
        DEBIAN_FRONTEND=noninteractive apt-get autoremove -y || true
    else
        echo 'apt: tracked packages already absent.'
    fi
    rm -f "$LIST_FILE"
    rmdir "$STATE_DIR" 2>/dev/null || true
}

case "$ACTION" in
install)
    install_pkgs "$@"
    ;;
remove)
    remove_tracked
    ;;
*)
    echo "Usage: $0 {install pkg...|remove}" >&2
    exit 1
    ;;
esac
