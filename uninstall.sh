#!/usr/bin/env sh

set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo 'This script must be run as root!' >&2
    exit 1
fi

modules=$(lsmod | grep '^xone_' | cut -d ' ' -f 1 | tr '\n' ' ')
version=$(dkms status xone | head -n 1 | tr -s ',:/' ' ' | cut -d ' ' -f 2)

if [ -n "$modules" ]; then
    echo "Unloading modules: $modules..."
    # shellcheck disable=SC2086
    modprobe -r -a $modules
fi

if [ -n "$version" ]; then
    echo "Uninstalling xone $version..."
    dkms remove -m xone -v "$version" --all
    rm -rf "/usr/src/xone-$version"
    rm -f /etc/modprobe.d/xone-blacklist.conf
    rm -f /usr/local/bin/xone-get-firmware.sh
else
    echo 'Driver is not installed!' >&2
fi

# Stop overlay + drop permissions/udev/autostart/input group (if we added it)
if [ -f /usr/local/lib/xone/user-access.sh ]; then
    sh /usr/local/lib/xone/user-access.sh remove
elif [ -f install/user-access.sh ]; then
    sh install/user-access.sh remove
fi

# Userspace overlay companion files
if [ -e /usr/local/bin/xone-overlay ] || [ -d /usr/local/share/xone-overlay ] \
    || [ -f /etc/systemd/user/xone-overlay.service ] \
    || [ -f /etc/xdg/systemd/user/xone-overlay.service ]; then
    echo 'Removing xone overlay files...'
    rm -f /usr/local/bin/xone-overlay
    rm -rf /usr/local/share/xone-overlay
    rm -f /etc/systemd/user/xone-overlay.service
    # Some distros alias /etc/xdg/systemd/user → /etc/systemd/user (same inode);
    # remove both paths so leftovers never remain.
    rm -f /etc/xdg/systemd/user/xone-overlay.service
fi

rm -f /etc/udev/rules.d/99-xone-dongle.rules /etc/udev/rules.d/99-xone.rules
if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules || true
fi

# Remove apt packages that *this* install added (never removes pre-existing ones,
# and never removes OBS Studio — we don't install it).
# Must run before deleting /usr/local/lib/xone (packaged helper lives there).
if [ -f /usr/local/lib/xone/apt-deps.sh ]; then
    sh /usr/local/lib/xone/apt-deps.sh remove
elif [ -f install/apt-deps.sh ]; then
    sh install/apt-deps.sh remove
fi

rm -rf /usr/local/lib/xone

echo 'Uninstall finished.'
# Firmware (/lib/firmware/xow_dongle.bin) is left in place on purpose.
