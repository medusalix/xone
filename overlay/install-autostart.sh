#!/usr/bin/env bash
# Install Cinnamon/GNOME autostart entry for a local (dev) checkout.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
ENTRY="$ROOT/xone-overlay.py"
DESKTOP_SRC="$ROOT/data/xone-overlay.desktop"
DEST_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/autostart"
DEST="$DEST_DIR/xone-overlay.desktop"

if [[ ! -f "$ENTRY" ]]; then
  echo "error: missing $ENTRY" >&2
  exit 1
fi

mkdir -p "$DEST_DIR"
sed "s|^Exec=.*|Exec=python3 $ENTRY|" "$DESKTOP_SRC" > "$DEST"
chmod +x "$ENTRY"

echo "Installed autostart: $DEST"
grep '^Exec=' "$DEST"
echo "Log out/in or start manually: python3 \"$ENTRY\""
