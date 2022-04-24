#!/usr/bin/env sh

set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo 'This script must be run as root!' >&2
    exit 1
fi

if ! [ -x "$(command -v dkms)" ]; then
    echo 'This script requires DKMS!' >&2
    exit 1
fi

if [ -n "$(dkms status xone)" ]; then
    echo 'Driver is already installed!' >&2
    exit 1
fi

if [ -f /usr/local/bin/xow ]; then
    echo 'Please uninstall xow!' >&2
    exit 1
fi

if [ -n "${SUDO_USER:-}" ]; then
    # Run as normal user to prevent "unsafe repository" error
    version=$(sudo -u "$SUDO_USER" git describe --tags 2> /dev/null || echo unknown)
else
    version=unknown
fi

source="/usr/src/xone-$version"
log="/var/lib/dkms/xone/$version/build/make.log"

echo "Installing xone $version..."
cp -r . "$source"
find "$source" -type f \( -name dkms.conf -o -name '*.c' \) -exec sed -i "s/#VERSION#/$version/" {} +

if [ "${1:-}" != --release ]; then
    echo 'ccflags-y += -DDEBUG' >> "$source/Kbuild"
fi

if dkms install -m xone -v "$version"; then
    # The blacklist should be placed in /usr/local/lib/modprobe.d for kmod 29+
    install -D -m 644 install/modprobe.conf /etc/modprobe.d/xone-blacklist.conf
    install -D -m 755 install/firmware.sh /usr/local/bin/xone-get-firmware.sh

    # Avoid conflicts between xpad and xone
    if lsmod | grep -q '^xpad'; then
        modprobe -r xpad
    fi

    # Avoid conflicts between mt76x2u and xone
    if lsmod | grep -q '^mt76x2u'; then
        modprobe -r mt76x2u
    fi
else
    if [ -r "$log" ]; then
        cat "$log" >&2
    fi

    exit 1
fi
