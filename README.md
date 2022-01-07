# xone [![Release Badge](https://img.shields.io/github/v/release/medusalix/xone?logo=github)](https://github.com/medusalix/xone/releases/latest) [![Discord Badge](https://img.shields.io/discord/733964971842732042?label=discord&logo=discord)](https://discord.gg/FDQxwWk) [![Donate Button](https://www.paypalobjects.com/en_US/i/btn/btn_donate_SM.gif)](https://www.paypal.com/donate?hosted_button_id=BWUECKFDNY446)

xone is a Linux kernel driver for Xbox One and Xbox Series X|S accessories. It serves as a modern replacement for `xpad`, aiming to be compatible with Microsoft's *Game Input Protocol* (GIP).
If you are looking for a way to use your controller via Bluetooth, check out [xpadneo](https://github.com/atar-axis/xpadneo).

## Compatibility

Take a look at [this spreadsheet](https://docs.google.com/spreadsheets/d/1fVGtqHTo9PRdmFVgEjmWuJIjuYEE_OziktNifFZIEgg) for a comparison between all the different Linux drivers and the devices they support.

## Important notes

This driver is still in active development. Use at your own risk!
If you are running `xow` upgrading to `xone` is *highly recommended*!
**Any feedback including bug reports, suggestions or ideas is [*greatly appreciated*](https://discord.gg/FDQxwWk).**

## Features

- [x] Input and force feedback (rumble)
- [x] Battery reporting (`UPower` integration)
- [x] LED brightness control (using `/sys/class/leds`)
- [x] Audio capture/playback (through `ALSA`)
- [x] Power management (suspend/resume and remote wakeup)
- [x] Wireless connectivity (via dongle)

## Supported devices

- [x] Gamepads
    - [x] Xbox One Controllers
    - [x] Xbox Series X|S Controllers
    - [x] Third party controllers (PowerA, PDP, etc.)
- [x] Headsets
    - [x] Xbox One Chat Headset
    - [x] Xbox One Stereo Headset (adapter or jack)
    - [x] Xbox Wireless Headset
    - [x] Third party wireless headsets (SteelSeries, Razer, etc.)
- [ ] Racing wheels
- [x] Xbox One Chatpad
- [x] Xbox Adaptive Controller
- [ ] Mad Catz Rock Band 4 Wireless Stratocaster
- [ ] Mad Catz Rock Band 4 Wireless Drum Kit

## Releases

[![Packaging status](https://repology.org/badge/vertical-allrepos/xone.svg)](https://repology.org/project/xone/versions)

Feel free to package xone for any Linux distribution or hardware you like.
Any issues regarding the packaging should be reported to the respective maintainers.

## Installation

### Prerequisites

- Linux (kernel 4.15+ and headers)
- DKMS

### Guide

1. Clone the repository:

```
git clone https://github.com/medusalix/xone
```

2. Install `xone` using the following command:

```
cd xone
sudo ./install.sh --release
```

**NOTE:** Please omit the `--release` flag when asked for your debug logs.

3. Download the firmware for the wireless dongle:

```
sudo xone-get-firmware.sh
```

**NOTE:** The `--skip-disclaimer` flag might be useful for scripting purposes.

4. Restart your computer
5. Unplug your device (or dongle) and plug it back in
6. Verify that `xone` is connected to your device by checking that the following command doesn't return any obvious errors from `xone` (look for "fail" or "error"):

```
sudo dmesg | tail -n 20
```

If everything succeeded, your device name and manufacturer should be listed in the output and you should be able to use your device! 🥳

### Extra steps for the Xbox Wireless Dongle

7. Ensure that your dongle is in pairing mode (see the [Kernel Interface](#kernel-interface) section for more info)
8. Press the pairing button on your wireless device to connect and use your device! 🥳

## Kernel interface

The pairing mode of the dongle can be queried via `sysfs`:

```
cat /sys/bus/usb/drivers/xone-dongle/*/pairing
```
If it outputs `1` it is currently in pairing mode, otherwise if it outputs `0` pairing mode is disabled

You can enable (or disable) the pairing using the following command:

```
echo 1 | sudo tee /sys/bus/usb/drivers/xone-dongle/*/pairing
```

## Updating

Make sure to completely uninstall xone before updating:

```
sudo ./uninstall.sh
```

## Troubleshooting

### General Troubleshooting Steps

In general, the following steps will help you figure out how to troubleshoot `xone`:
- Uninstall the release version and reinstall the debug version (omitting the `--release` flag from the install command) to give more descriptive errors
```
sudo ./install.sh
```
- Repeat steps 5 & 6 in the guide above (running `dmesg`) to gather error messages
- Join the [Discord server](https://discord.gg/FDQxwWk) and ask questions from the community

### Missing Firmware

```
Dez 09 23:52:24 NoXPC kernel: xone-dongle 1-8.2:1.0: Direct firmware load for xow_dongle.bin failed with error -2
Dez 09 23:52:24 NoXPC kernel: xone-dongle 1-8.2:1.0: xone_mt76_load_firmware: firmware not found
Dez 09 23:52:24 NoXPC kernel: xone-dongle 1-8.2:1.0: xone_dongle_init: load firmware failed: -2
Dez 09 23:52:24 NoXPC kernel: xone-dongle: probe of 1-8.2:1.0 failed with error -2
```

If you see an error like this from the `dmesg` command, then it is likely that you haven't installed the device firmware yet. See step 3 of the installation guide for how to install the firmware

## License

xone is released under the [GNU General Public License, Version 2](LICENSE).

```
Copyright (C) 2021 Severin von Wnuck

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
```
