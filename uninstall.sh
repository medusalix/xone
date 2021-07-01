#!/usr/bin/env sh

MODULES=$(lsmod | grep '^xone_' | cut -d ' ' -f 1 | tr '\n' ' ')
INSTALLED=$(dkms status xone | tr -s ',:' ' ' | cut -d ' ' -f 2)
SOURCE="/usr/src/xone-$INSTALLED"
BLACKLIST='/etc/modprobe.d/xone-blacklist.conf'

if [ -n "$MODULES" ]; then
    echo "Unloading modules: $MODULES..."
    # shellcheck disable=SC2086
    modprobe -r -a $MODULES
fi

if [ -n "$INSTALLED" ]; then
    echo "Uninstalling xone $INSTALLED..."
    dkms remove --all "xone/$INSTALLED"
    rm -r "$SOURCE"
    rm -f "$BLACKLIST"
else
    echo 'Driver is not installed!'
fi
