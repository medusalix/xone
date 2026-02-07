#!/usr/bin/env bash
if [ "$EUID" -ne 0 ]; then
  echo "Please run as root (sudo)"
  exit 1
fi

echo "Installing pairing script to /usr/local/bin/xone-pair..."
cp xone-pair.sh /usr/local/bin/xone-pair
chmod +x /usr/local/bin/xone-pair

echo "Installing udev rule..."
cp 99-xone-pairing.rules /etc/udev/rules.d/
udevadm control --reload-rules
udevadm trigger

echo "Installing desktop shortcut..."
# Copy to both global and local if possible
cp xone-pair.desktop /usr/share/applications/
if [ -n "$SUDO_USER" ]; then
    USER_HOME=$(getent passwd "$SUDO_USER" | cut -d: -f6)
    mkdir -p "$USER_HOME/Desktop"
    cp xone-pair.desktop "$USER_HOME/Desktop/"
    chown "$SUDO_USER:$SUDO_USER" "$USER_HOME/Desktop/xone-pair.desktop"
    chmod +x "$USER_HOME/Desktop/xone-pair.desktop"
fi

echo "Done! You can now start pairing by clicking the 'Xbox Pairing' icon on your desktop or running 'xone-pair' in terminal."
