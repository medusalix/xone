# xone [![Release Badge](https://img.shields.io/github/v/release/medusalix/xone?logo=github)](https://github.com/medusalix/xone/releases/latest) [![Discord Badge](https://img.shields.io/discord/733964971842732042?label=discord&logo=discord)](https://discord.gg/FDQxwWk) [![Donate Button](https://www.paypalobjects.com/en_US/i/btn/btn_donate_SM.gif)](https://www.paypal.com/donate?hosted_button_id=BWUECKFDNY446)

`xone` is a Linux kernel driver for Xbox One and Xbox Series X|S accessories. It serves as a modern replacement for `xpad`, aiming to be compatible with Microsoft's *Game Input Protocol* (GIP).

## Compatibility

- [x] Wired devices (via USB)
- [x] Wireless devices (with Xbox Wireless Dongle)
- [ ] Bluetooth devices (check out [`xpadneo`](https://github.com/atar-axis/xpadneo))

## Important notes

This driver is still in active development. Use at your own risk!
If you are running `xow` upgrading to `xone` is *highly recommended*!
Always update your Xbox devices to the latest firmware version!
**Any feedback including bug reports, suggestions or ideas is [*greatly appreciated*](https://discord.gg/FDQxwWk).**

## Features

- [x] Input and force feedback (rumble)
- [x] Battery reporting (`UPower` integration)
- [x] LED control (using `/sys/class/leds`)
- [x] Audio capture/playback (through `ALSA`)
- [x] Power management (suspend/resume and remote/wireless wakeup)

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
- [ ] Third party racing wheels (Thrustmaster, Logitech, etc.)
- [x] Xbox One Chatpad
- [x] Xbox Adaptive Controller
- [x] Mad Catz Rock Band 4 Wireless Stratocaster

## Releases

[![Packaging status](https://repology.org/badge/vertical-allrepos/xone.svg)](https://repology.org/project/xone/versions)

Feel free to package `xone` for any Linux distribution or hardware you like.
Any issues regarding the packaging should be reported to the respective maintainers.

## Installation

### Prerequisites

- Linux (kernel 4.15+ and headers)
- DKMS
- curl (for firmware download)
- cabextract (for firmware extraction)

### Guide

1. Unplug your Xbox devices.

2. Clone the repository:

```
git clone https://github.com/medusalix/xone
```

3. Install `xone`:

```
cd xone
sudo ./install.sh --release
```

**NOTE:** Please omit the `--release` flag when asked for your debug logs.

4. Download the firmware for the wireless dongle:

```
sudo xone-get-firmware.sh
```

**NOTE:** The `--skip-disclaimer` flag might be useful for scripting purposes.

5. Plug in your Xbox devices.

### Updating

Make sure to completely uninstall `xone` before updating:

```
sudo ./uninstall.sh
```

## Wireless pairing

Xbox devices have to be paired to the wireless dongle. They will not automatically connect to the dongle if they have been previously plugged into a USB port or used via Bluetooth.

Instructions for pairing your devices can be found [here](https://support.xbox.com/en-US/help/hardware-network/controller/connect-xbox-wireless-controller-to-pc) (see the section on *Xbox Wireless*).

## LED control

The guide button LED can be controlled via `sysfs`:

```
echo 2 | sudo tee /sys/class/leds/gip*/mode
echo 5 | sudo tee /sys/class/leds/gip*/brightness
```

Replace the wildcard (`gip*`) if you want to control the LED of a specific device.
The modes and the maximum brightness can vary from device to device.

## Troubleshooting

Uninstall the release version and install a debug build of `xone` (see installation guide).
Run `sudo dmesg` to gather logs and check for any error messages related to `xone`.
If `xone` is not being loaded automatically you might have to reboot your system.

### Error messages

- `Direct firmware load for xow_dongle.bin failed with error -2`
    - Download the firmware for the wireless dongle (see installation guide).

### Input issues

You can use `evtest` and `fftest` to check the input and force feedback functionality of your devices.

### Other problems

Please join the [Discord server](https://discord.gg/FDQxwWk) in case of any other problems.

## License

`xone` is released under the [GNU General Public License, Version 2](LICENSE).

```
Copyright (C) 2021 Severin von Wnuck-Lipinski

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
```
