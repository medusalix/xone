#!/usr/bin/env sh

set -eu

if [ "$(id -u)" -eq 0 ]; then
    echo 'Please do not run this script as root!' >&2
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

version=$(git describe --tags 2> /dev/null || echo unknown)
source="/usr/src/xone-$version"
log="/var/lib/dkms/xone/$version/build/make.log"

echo "Installing xone $version..."
sudo cp -r . "$source"
find "$source" -type f \( -name dkms.conf -o -name '*.c' \) -exec sudo sed -i "s/#VERSION#/$version/" {} +

if [ "${1:-}" != --release ]; then
    echo 'ccflags-y += -DDEBUG' | sudo tee -a "$source/Kbuild"
fi

if sudo dkms install -m xone -v "$version"; then
    # The blacklist should be placed in /usr/local/lib/modprobe.d for kmod 29+
    sudo install -D -m 644 install/modprobe.conf /etc/modprobe.d/xone-blacklist.conf
    sudo install -D -m 755 install/firmware.sh /usr/local/bin/xone-get-firmware.sh

    # Avoid conflicts between xpad and xone
    if lsmod | grep -q '^xpad'; then
        sudo modprobe -r xpad
    fi

    # Avoid conflicts between mt76x2u and xone
    if lsmod | grep -q '^mt76x2u'; then
        sudo modprobe -r mt76x2u
    fi
else
    if [ -r "$log" ]; then
        cat "$log" >&2
    fi

    exit 1
fi
