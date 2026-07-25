<p align="center">
    <img src="logo.svg" alt="Logo" width="200">
</p>

<p align="center">
    <a href="https://github.com/medusalix/xone/releases/latest"><img src="https://img.shields.io/github/v/release/medusalix/xone?logo=github" alt="Release Badge"></a>
    <a href="https://discord.gg/FDQxwWk"><img src="https://img.shields.io/discord/733964971842732042?label=discord&logo=discord" alt="Discord Badge"></a>
    <a href="https://www.paypal.com/donate?hosted_button_id=BWUECKFDNY446"><img src="https://www.paypalobjects.com/en_US/i/btn/btn_donate_SM.gif" alt="Donate Button"></a>
</p>

`xone` is a Linux kernel driver for Xbox One and Xbox Series X|S accessories. It serves as a modern replacement for `xpad`, aiming to be compatible with Microsoft's *Game Input Protocol* (GIP).

## Extensions in this tree (Linux Mint)

> **Target platform:** [Linux Mint](https://linuxmint.com/) with **Cinnamon** on **X11**.  
> It *may* work on other Ubuntu/Debian-based distributions, **but that is not guaranteed** (Wayland, other desktops, or different packaging can break the overlay or udev rules).

In addition to the upstream driver, this repository includes:

| Area | What was added |
|------|----------------|
| **Driver / dongle** | Wireless adapter reconnect mitigations; autosuspend disabled; dongle firmware installed by `install.sh` when missing |
| **Power-off** | Sysfs `power_off` on GIP clients (`/sys/bus/xone-gip/devices/gip*.*/power_off`) |
| **Permissions** | Udev + `input` group for Guide / overlay (`install/user-access.sh`) |
| **Overlay** | Userspace Game Bar–style companion — see [`overlay/README.md`](overlay/README.md) |
| **Install / uninstall** | Overlay, udev, firmware, and tracked apt dependencies (`install/apt-deps.sh`) |

### Overlay — quick start

1. Install with `sudo ./install.sh`, then **log out and back in** (`input` group).
2. The `xone-overlay` service starts at login (`systemd --user`).
3. Press the **Guide** button (center Xbox button) to show or hide the panel.
4. On the panel: batteries, performance, volume, screenshot (area / window / monitor / full), OBS (record / stream), power off controller, and settings (position, opacity, screenshot folder, OBS WebSocket).

Full overlay documentation: **[`overlay/README.md`](overlay/README.md)**.

### Update / remove

```bash
sudo ./uninstall.sh   # removes driver, overlay, udev, and apt packages *this* install added
sudo ./install.sh     # reinstall everything
```

`/lib/firmware/xow_dongle.bin` is **not** removed on uninstall (the dongle needs it). **OBS Studio** is neither installed nor removed by xone — the overlay only controls it when already present.

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
- [x] Software power-off of controllers via sysfs (`power_off`)

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

3. Install `xone` (driver, udev, overlay, and dongle firmware if missing):

```
cd xone
sudo ./install.sh
```

**NOTE:** You can use the `--release` flag to disable debug logging.

The wireless dongle firmware is installed automatically when `/lib/firmware/xow_dongle.bin` is missing (Microsoft [Terms of Use](https://www.microsoft.com/en-us/legal/terms-of-use)). To (re)download it manually:

```
sudo xone-get-firmware.sh
```

**NOTE:** The `--skip-disclaimer` flag might be useful for scripting purposes.

4. Plug in your Xbox devices.

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

## Controller power-off

Connected GIP clients expose a write-only sysfs attribute:

```
echo 1 | tee /sys/bus/xone-gip/devices/gip0.0/power_off
```

This sends the same power-off command used when the wireless dongle is unplugged.
Press the Xbox button on the controller to turn it back on (pairing is kept).
The xone Overlay also has a **Power off controller** button that writes this attribute
(requires group `input` + the packaged udev rule).

## Troubleshooting

Uninstall the release version and install a debug build of `xone` (see installation guide).
Run `sudo dmesg` to gather logs and check for any error messages related to `xone`.
If `xone` is not being loaded automatically you might have to reboot your system.

### Wireless dongle reconnects slowly / needs replugging

This tree includes mitigations for common Xbox Wireless Adapter issues:

- No `usb_reset_device()` during probe (avoids dongle disappearing until replug)
- RF patch + delays on cold/warm firmware init
- 60s pairing window after init/resume so already-paired controllers can reconnect
- USB autosuspend disabled for the dongle (driver + udev rule)

After updating the driver, reinstall with `sudo ./uninstall.sh` then `sudo ./install.sh`, unplug/replug the adapter, and prefer a rear motherboard USB port.

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
