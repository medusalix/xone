#!/usr/bin/env sh

set -eu

if [ "$(id -u)" -eq 0 ]; then
    echo 'Please do not run this script as root!' >&2
    exit 1
fi

modules=$(lsmod | grep '^xone_' | cut -d ' ' -f 1 | tr '\n' ' ')
version=$(dkms status xone | head -n 1 | tr -s ',:/' ' ' | cut -d ' ' -f 2)

if [ -n "$modules" ]; then
    echo "Unloading modules: $modules..."
    # shellcheck disable=SC2086
    sudo modprobe -r -a $modules
fi

if [ -n "$version" ]; then
    echo "Uninstalling xone $version..."
    sudo dkms remove -m xone -v "$version" --all
    sudo rm -r "/usr/src/xone-$version"
    sudo rm -f /etc/modprobe.d/xone-blacklist.conf
    sudo rm -f /usr/local/bin/xone-get-firmware.sh
else
    echo 'Driver is not installed!' >&2
fi
