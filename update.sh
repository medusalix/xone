#!/usr/bin/env sh

git pull && sudo ./uninstall.sh && sudo ./install.sh --release && sudo xone-get-firmware.sh
