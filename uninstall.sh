#!/usr/bin/env sh

if [ "$(id -u)" -ne 0 ]; then
    echo 'This script must be run as root!' >&2
    exit 1
fi

MODULES=$(lsmod | grep '^xone_' | cut -d ' ' -f 1 | tr '\n' ' ')
INSTALLED=$(dkms status xone | head -n 1 | tr -s ',:' ' ' | cut -d ' ' -f 2)
SOURCE="/usr/src/xone-$INSTALLED"
BLACKLIST='/etc/modprobe.d/xone-blacklist.conf'

if [ -n "$MODULES" ]; then
    echo "Unloading modules: $MODULES..."
    # shellcheck disable=SC2086
    modprobe -r -a $MODULES
fi

if [ -n "$INSTALLED" ]; then
    echo "Uninstalling xone $INSTALLED..."
    if dkms remove --all "xone/$INSTALLED"; then
        rm -r "$SOURCE"
        rm -f "$BLACKLIST"
    fi
else
    echo 'Driver is not installed!' >&2
fi
