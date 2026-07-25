# xone Overlay (Linux Mint)

Userspace companion for the [xone](../README.md) driver: a Game Bar–style panel for **Linux Mint / Cinnamon (X11)**.

> **Support:** Developed and tested on **Linux Mint** (Cinnamon + X11).  
> It *may* work on other **Ubuntu/Debian-based** distributions, **but that is not guaranteed** — Wayland, other desktops, or different packages may require changes.

Press the **Guide** button (center Xbox button) to show or hide the panel. Use **✕** or `Esc` to close.

---

## Features

| Feature | Details |
|---------|---------|
| Clock | Current time in the header |
| Batteries | Laptop + controller (UPower / sysfs; controller levels: Low/Normal/High/Full) |
| Performance | CPU, RAM, and GPU (NVIDIA or AMD when available) |
| Audio | Default sink volume and mute (`pactl`) |
| Screenshot | Capture-time menu: **area**, **window/app**, **monitor**, or **full screen** |
| Screenshot folder | Configurable (default `~/Pictures/Screenshots`) |
| OBS Studio | **Record** / **Stream** via WebSocket (does not encode video inside the overlay) |
| Power off controller | Writes xone sysfs `power_off`; wake with the Xbox button |
| Appearance | Follows the GTK theme (e.g. Mint-Y); four edge positions; background opacity |
| Autostart | `systemd --user` + fallback under `~/.config/autostart/` |

---

## Installation (recommended)

The main project script installs the overlay with the driver:

```bash
cd /path/to/xone
sudo ./install.sh
```

This (via [`install/overlay.sh`](../install/overlay.sh), [`install/user-access.sh`](../install/user-access.sh), [`install/apt-deps.sh`](../install/apt-deps.sh)):

1. Installs overlay apt dependencies (**only packages that were not already present**; list in `/var/lib/xone/apt-packages.list`)
2. Copies the app to `/usr/local/share/xone-overlay`
3. Creates the `xone-overlay` command and `systemd --user` unit
4. Adds your user to group `input` (tracked so uninstall can reverse it)
5. Installs udev rules (dongle autosuspend off + controller access)
6. Sets up autostart under `~/.config/autostart/`

Then **log out and back in** (`input` group) — the overlay starts on its own.

```bash
sudo ./uninstall.sh   # removes overlay, service, udev, and apt packages tracked by xone
```

### Apt dependencies (installed by install)

- `python3-gi`, `python3-gi-cairo`, `gir1.2-gtk-3.0`
- `gir1.2-upowerglib-1.0`, `python3-evdev`, `python3-websocket`
- `gnome-screenshot`
- `pulseaudio-utils` or `pipewire-pulse` (if `pactl` is missing)
- Optional on the system: **OBS Studio** (Flatpak or apt) — xone does **not** install OBS

### Local development (without `install.sh`)

```bash
sudo apt install python3-gi python3-gi-cairo gir1.2-gtk-3.0 \
  gir1.2-upowerglib-1.0 python3-evdev python3-websocket \
  pulseaudio-utils gnome-screenshot
sudo usermod -aG input "$USER"   # log out/in
cd overlay
python3 xone-overlay.py
# optional:
./install-autostart.sh
```

---

## Usage

### Open the panel

1. With the service running and a controller connected (xone driver), press **Guide**.
2. To close: **✕**, `Esc`, or Guide again.

```bash
# start now (without re-login)
systemctl --user enable --now xone-overlay

# status / logs
systemctl --user status xone-overlay
tail -f ~/.local/state/xone-overlay/overlay.log
```

### Screenshots

On **Screenshot**, choose:

- **Screen area…** — drag to select a region  
- **Window / application…** — click a window  
- **Monitor: …** — one specific display (useful with 2+ monitors)  
- **Full screen** — all monitors  

Default folder: `~/Pictures/Screenshots` (change under **Settings**).

### Power off the controller

**Power off controller** → confirm → the pad shuts down (same as holding the Xbox button).  
To turn it on again: press the Xbox button (pairing is kept).

### OBS (record / stream)

The overlay does **not** record video itself; it controls OBS over WebSocket (`localhost:4455`).

1. Install OBS: `flatpak install flathub com.obsproject.Studio` or `sudo apt install obs-studio`
2. Open OBS → **Tools → WebSocket Server Settings → Enable**
3. If a password is set, save it under overlay **Settings**
4. On the panel: **Record** / **Stream**

If OBS is missing or the WebSocket does not respond, the overlay shows install/enable instructions.

### Settings (gear icon)

- **Position** — Top / Right / Bottom / Left (horizontal or vertical layout)
- Panel background **opacity**
- **Screenshot folder**
- **OBS** — WebSocket host, port, and password

Preferences: `~/.config/xone-overlay/config.json`

---

## Troubleshooting

| Symptom | Check |
|---------|--------|
| Guide does not open the overlay | `groups` should list `input`; log out/in; `python3 -c "import evdev"`; controller on xone |
| No controller battery | `upower -e` / `ls /sys/class/power_supply/` |
| No volume | `pactl info` |
| Screenshot fails | `gnome-screenshot --version` |
| OBS not connected | OBS open + WebSocket enabled; password in settings; `python3-websocket` installed |
| Dongle missing firmware | `ls /lib/firmware/xow_dongle.bin` or `sudo xone-get-firmware.sh` |
| Panel misaligned after changing position | Change position again after layout settles; buttons stay disabled during repositioning |

---

## Scope and limitations

- Focused on **Linux Mint / Cinnamon / X11**
- No FPS counter, no per-application mixer
- Recording/streaming depend on external **OBS**
- Other Ubuntu-like distros: use at your own risk; report your environment if something fails
