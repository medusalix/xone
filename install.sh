#!/usr/bin/env sh

# The blacklist should be placed in /usr/local/lib/modprobe.d for kmod 29+
VERSION=$(git describe --tags 2> /dev/null || echo 'unknown')
SOURCE="/usr/src/xone-$VERSION"
BLACKLIST='/etc/modprobe.d/xone-blacklist.conf'
LOG="/var/lib/dkms/xone/$VERSION/build/make.log"

echo "Installing xone $VERSION..."
cp -r . "$SOURCE"
find "$SOURCE" -type f \( -name 'dkms.conf' -o -name '*.c' \) -exec sed -i "s/#VERSION#/$VERSION/" {} +

if [ "$1" != '--release' ]; then
    echo 'ccflags-y += -DDEBUG' >> "$SOURCE/Kbuild"
fi

if dkms install "xone/$VERSION"; then
    install -D -m 644 modprobe.conf "$BLACKLIST"
else
    cat "$LOG"
    exit 1
fi
