<p align="center">
    <img src="logo.svg" alt="Logo" width="200">
</p>

<p align="center">
    <a href="https://github.com/medusalix/xone/releases/latest"><img src="https://img.shields.io/github/v/release/medusalix/xone?logo=github" alt="Release Badge"></a>
    <a href="https://discord.gg/FDQxwWk"><img src="https://img.shields.io/discord/733964971842732042?label=discord&logo=discord" alt="Discord Badge"></a>
    <a href="https://www.paypal.com/donate?hosted_button_id=BWUECKFDNY446"><img src="https://www.paypalobjects.com/en_US/i/btn/btn_donate_SM.gif" alt="Donate Button"></a>
</p>

`xone` is a Linux kernel driver for Xbox One and Xbox Series X|S accessories. It serves as a modern replacement for `xpad`, aiming to be compatible with Microsoft's *Game Input Protocol* (GIP).

## Compatibility

- [x] Wired devices (via USB)
- [x] Wireless devices (with Xbox Wireless Dongle)
- [ ] Bluetooth devices (check out [`xpadneo`](https://github.com/atar-axis/xpadneo))

Installing `xone` will disable the `xpad` kernel driver. If you are still using Xbox or Xbox 360 peripherals, you will have to install [`xpad-noone`](https://github.com/medusalix/xpad-noone) as a replacement for `xpad`.

> [!IMPORTANT]
> This project is in maintenance mode and will receive only occasional minor fixes. An actively developed fork is available [here](https://github.com/dlundqvist/xone).

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
    - [x] Xbox Adaptive Controller
    - [x] Third party controllers (PowerA, PDP, etc.)
- [x] Headsets
    - [x] Xbox One Chat Headset
    - [x] Xbox One Stereo Headset (adapter or jack)
    - [x] Xbox Wireless Headset
    - [x] Third party wireless headsets (SteelSeries, Razer, etc.)
- [x] Guitars & Drums
    - [x] Mad Catz Rock Band 4 Wireless Fender Stratocaster
    - [x] Mad Catz Rock Band 4 Wireless Drum Kit
    - [x] PDP Rock Band 4 Wireless Fender Jaguar
- [x] Xbox One Chatpad
- [ ] Third party racing wheels (Thrustmaster, Logitech, etc.)

## Releases

[![Packaging status](https://repology.org/badge/vertical-allrepos/xone.svg)](https://repology.org/project/xone/versions)

Feel free to package `xone` for any Linux distribution or hardware you like.
Any issues regarding the packaging should be reported to the respective maintainers.

## Installation

### Prerequisites

- Linux (kernel 5.13+ and headers)
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
sudo ./install.sh
```

**NOTE:** You can use the `--release` flag to disable debug logging.

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

Changing the LED in the above way is temporary, it will only last until the device disconnects. To apply these settings automatically when a device connects, you can create a new `udev` rule in `/etc/udev/rules.d/50-xone.rules` with the following content:

```
ACTION=="add", SUBSYSTEM=="leds", KERNEL=="gip*", ATTR{mode}="2", ATTR{brightness}="5"
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

### Secure Boot

If your sysem is using secure boot the module will not load.
To fix this issue:
1. Follow the DKMS instructions for signing modules here: https://github.com/dell/dkms#secure-boot
2. reinstall xone using the steps above

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
