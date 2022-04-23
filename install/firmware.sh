#!/usr/bin/env sh

set -eu

if [ "$(id -u)" -eq 0 ]; then
    echo 'Please do not run this script as root!' >&2
    exit 1
fi

if ! [ -x "$(command -v curl)" ]; then
    echo 'This script requires curl!' >&2
    exit 1
fi

if ! [ -x "$(command -v cabextract)" ]; then
    echo 'This script requires cabextract!' >&2
    exit 1
fi

if [ "${1:-}" != --skip-disclaimer ]; then
    echo "The firmware for the wireless dongle is subject to Microsoft's Terms of Use:"
    echo 'https://www.microsoft.com/en-us/legal/terms-of-use'
    echo
    echo 'Press enter to continue!'
    read -r _
fi

driver_url='http://download.windowsupdate.com/c/msdownload/update/driver/drvs/2017/07/1cd6a87c-623f-4407-a52d-c31be49e925c_e19f60808bdcbfbd3c3df6be3e71ffc52e43261e.cab'
firmware_hash='48084d9fa53b9bb04358f3bb127b7495dc8f7bb0b3ca1437bd24ef2b6eabdf66'

curl -L -o driver.cab "$driver_url"
cabextract -F FW_ACC_00U.bin driver.cab
echo "$firmware_hash" FW_ACC_00U.bin | sha256sum -c
sudo mv FW_ACC_00U.bin /lib/firmware/xow_dongle.bin
rm driver.cab
