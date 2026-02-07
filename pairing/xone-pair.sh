#!/usr/bin/env bash

# Find all xone-dongle pairing attributes
PAIR_FILES=$(ls /sys/bus/usb/drivers/xone-dongle/*/pair 2>/dev/null)

if [ -z "$PAIR_FILES" ]; then
    echo "No Xbox dongle found with pairing support."
    exit 1
fi

for f in $PAIR_FILES; do
    echo "Triggering pairing for $(dirname $f)..."
    echo 1 > "$f"
done

echo "Pairing mode active for 30 seconds."
