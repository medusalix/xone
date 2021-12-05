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

version=$(git describe --tags 2> /dev/null || echo unknown)
source="/usr/src/xone-$version"
log="/var/lib/dkms/xone/$version/build/make.log"

echo "Installing xone $version..."
cp -r . "$source"
find "$source" -type f \( -name dkms.conf -o -name '*.c' \) -exec sed -i "s/#VERSION#/$version/" {} +

if [ "${1:-}" != --release ]; then
    echo 'ccflags-y += -DDEBUG' >> "$source/Kbuild"
fi

if dkms install xone -v "$version"; then
    # The blacklist should be placed in /usr/local/lib/modprobe.d for kmod 29+
    install -D -m 644 install/modprobe.conf /etc/modprobe.d/xone-blacklist.conf

    # Avoid conflicts between xpad and xone
    if lsmod | grep -q '^xpad'; then
        modprobe -r xpad
    fi
else
    if [ -r "$log" ]; then
        cat "$log" >&2
    fi

    exit 1
fi
