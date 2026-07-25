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
    install -D -m 644 install/modprobe.conf /etc/modprobe.d/xone-blacklist.conf
    install -D -m 755 install/firmware.sh /usr/local/bin/xone-get-firmware.sh

    # Wireless dongle firmware (required; historically a separate manual step)
    if [ -f /lib/firmware/xow_dongle.bin ]; then
        echo 'Dongle firmware already present (/lib/firmware/xow_dongle.bin).'
    else
        echo 'Installing wireless dongle firmware...'
        echo "Subject to Microsoft's Terms of Use: https://www.microsoft.com/en-us/legal/terms-of-use"
        if [ -f install/apt-deps.sh ]; then
            install -D -m 755 install/apt-deps.sh /usr/local/lib/xone/apt-deps.sh
            sh install/apt-deps.sh install curl cabextract
        fi
        if sh install/firmware.sh --skip-disclaimer; then
            echo 'Dongle firmware installed.'
        else
            echo 'Warning: firmware download failed. Fix network deps, then run:' >&2
            echo '  sudo xone-get-firmware.sh' >&2
        fi
    fi

    # Permissions: udev (dongle power + input/uaccess) and group "input"
    if [ -f install/user-access.sh ]; then
        install -D -m 755 install/user-access.sh /usr/local/lib/xone/user-access.sh
        # Keep helpers next to each other for uninstall from any cwd
        install -D -m 644 install/udev-xone.rules /usr/local/lib/xone/udev-xone.rules
        if [ -f install/apt-deps.sh ]; then
            install -D -m 755 install/apt-deps.sh /usr/local/lib/xone/apt-deps.sh
        fi
        sh install/user-access.sh install
    fi

    # Avoid conflicts between xpad and xone
    if lsmod | grep -q '^xpad'; then
        modprobe -r xpad
    fi

    # Avoid conflicts between mt76x2u and xone
    if lsmod | grep -q '^mt76x2u'; then
        modprobe -r mt76x2u
    fi

    # Userspace Guide-button overlay (Linux Mint / Debian family)
    if [ -f install/overlay.sh ]; then
        sh install/overlay.sh
    fi

    echo
    echo "Installation finished."
    echo "If this is the first time joining group 'input', log out and back in (or reboot)."
else
    if [ -r "$log" ]; then
        cat "$log" >&2
    fi

    exit 1
fi
