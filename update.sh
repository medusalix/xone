#!/usr/bin/env sh

if git pull; then
	sudo ./uninstall.sh && sudo ./install.sh --release && sudo xone-get-firmware.sh
fi
