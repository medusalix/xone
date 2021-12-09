# xone [![Release Badge](https://img.shields.io/github/v/release/medusalix/xone?logo=github)](https://github.com/medusalix/xone/releases/latest) [![Discord Badge](https://img.shields.io/discord/733964971842732042?label=discord&logo=discord)](https://discord.gg/FDQxwWk) [![Donate Button](https://www.paypalobjects.com/en_US/i/btn/btn_donate_SM.gif)](https://www.paypal.com/donate?hosted_button_id=BWUECKFDNY446)

xone is a Linux kernel driver for Xbox One and Xbox Series X|S accessories. It serves as a modern replacement for `xpad`, aiming to be compatible with Microsoft's *Game Input Protocol* (GIP).
If you are looking for a way to use your controller via Bluetooth, check out [xpadneo](https://github.com/atar-axis/xpadneo).
Take a look at [this spreadsheet](https://docs.google.com/spreadsheets/d/1fVGtqHTo9PRdmFVgEjmWuJIjuYEE_OziktNifFZIEgg) for a comparison between all the different Linux drivers.

## Important notes

This driver is still in active development. Use at your own risk!
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
- [ ] Headsets
    - [x] Xbox One Chat Headset
    - [x] Xbox One Stereo Headset (adapter or jack)
    - [ ] Xbox Wireless Headset
    - [ ] Third party wireless headsets (SteelSeries, Razer, etc.)
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

Clone the repository:

```
git clone https://github.com/medusalix/xone
```

Install xone using the following command:

```
sudo ./install.sh --release
```

Download the firmware for the wireless dongle:

```
sudo xone-get-firmware.sh
```

**NOTE:** Please omit the `--release` flag when asked for your debug logs.

### Updating

Make sure to completely uninstall xone before updating:

```
sudo ./uninstall.sh
```

## License

xone is released under the [GNU General Public License, Version 2](LICENSE).

```
Copyright (C) 2021 Severin von Wnuck

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
```
