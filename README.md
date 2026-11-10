# xfce-removable-plugin

Panel plugin for Xfce that manages removable drives, phones and cameras, and shows other connected USB devices.

Inspired by KDE Plasma's Device Notifier, built natively for Xfce using UDisks2, gvfs and GUdev. Works on X11 and on Wayland (Xfce 4.20).

> Independent project, not affiliated with or endorsed by the Xfce project.

## Features

- Lists removable storage devices (USB drives, SD cards, external disks) via UDisks2
- Drives with a single partition take one row; drives with several partitions show them under the disk header, expanded by default
- Mount / Unmount buttons with intermediate state feedback ("Mounting…" / "Unmounting…")
- **Mount** always opens the drive in the file manager
- **Eject** / **Safely remove** button on each drive — unmounts all partitions first and only reports success when the drive is really safe to unplug
- **Phones and cameras**: Android phones in file-transfer mode (MTP), cameras (PTP) and iPhones (afc), via gvfs — mount, open in the file manager, safely remove
- **Other devices** section: gamepads, graphics tablets, security keys, serial boards (Arduino, ESP32), printers, audio and MIDI devices, webcams — classified with udev; built-in devices are left out
- Optional display of keyboards and mice (hidden by default)
- Desktop notifications for mount, unmount, safe removal and connection events (libnotify), with per-device notification replacement and configurable device categories
- Optional connection/disconnection sounds through libcanberra and the desktop sound theme (respecting GTK event-sound settings)
- Hides the panel button when no devices are present, configurable
- Scrollable popup with configurable width and height; it follows the GTK theme's popover look and floats with rounded corners when a compositor is running (configurable: automatic, floating or attached to the panel)
- Configurable button size (square or fixed width) and icon size
- Auto-refresh via UDisks2 DBus signals, gvfs volume monitor and GUdev uevents, with a 300 ms debounce
- Translated into 13 languages

## Install

Published packages are listed on the [Releases](https://github.com/Tantin1/xfce-removable-plugin/releases) page. Requires Xfce 4.18 or newer.

| Distribution | File |
|---|---|
| Debian 12 | `xfce-removable-plugin_<version>_debian12_amd64.deb` |
| Debian 13 | `xfce-removable-plugin_<version>_debian13_amd64.deb` |
| Ubuntu 24.04, Xubuntu 24.04, Linux Mint 22 Xfce | `xfce-removable-plugin_<version>_ubuntu24.04_amd64.deb` |
| Arch Linux, CachyOS and derivatives | `xfce-removable-plugin-<version>-x86_64.pkg.tar.zst` |

Debian / Ubuntu / Mint (`apt` installs the dependencies too):

```bash
sudo apt install ./xfce-removable-plugin_<version>_<distro>_amd64.deb
```

Arch Linux and derivatives:

```bash
sudo pacman -U xfce-removable-plugin-<version>-x86_64.pkg.tar.zst
```

An AUR package is planned.

### After installing

Restart the panel (`xfce4-panel -r`) and add **Removable Devices** from the panel's "Add New Items" dialog.

By default the button is hidden while nothing is connected. Right after adding it, it stays visible until the panel restarts, so you can place it (right-click → Move). Instances you no longer want, even hidden ones, are listed in *Panel Preferences → Items*.

For phones and cameras, install gvfs's MTP / gphoto2 / afc backends: `gvfs-backends` on Debian / Ubuntu (recommended by the package, usually already installed), `gvfs-mtp`, `gvfs-gphoto2`, `gvfs-afc` on Arch.

## Build from source

| Library | Version |
|---|---|
| GTK 3 | ≥ 3.24 |
| libxfce4panel | ≥ 4.18 |
| libnotify | ≥ 0.7 |
| gudev-1.0 | any |
| libcanberra-gtk3 | any (sound notifications) |
| meson, pkg-config, gettext | build only (meson ≥ 0.60) |
| udisks2 | runtime |
| gvfs + MTP / gphoto2 / afc backends | runtime, optional (phones and cameras) |

Build dependencies:

```bash
# Debian / Ubuntu / Mint
sudo apt install build-essential meson pkgconf gettext \
     libgtk-3-dev libxfce4panel-2.0-dev libxfce4ui-2-dev libnotify-dev libgudev-1.0-dev libcanberra-gtk3-dev

# Arch Linux
sudo pacman -S --needed base-devel meson gettext xfce4-panel gtk3 libnotify libgudev libcanberra
```

Build without installing (recommended for verifying a source checkout):

```bash
meson setup build src -Dprefix=/usr
meson compile -C build
```

Prefer installation through the distribution packages above. **Do not run `sudo meson install` over files already owned by Pacman or dpkg**, as this bypasses the package manager. For a manual installation on a system without a packaged copy, check file ownership first, then run `sudo meson install -C build` and `xfce4-panel -r`.

The plugin module is installed where the installed panel looks for it, taken from `libxfce4panel-2.0` via pkg-config (`/usr/lib/xfce4/panel/plugins` on Arch, `/usr/lib/x86_64-linux-gnu/xfce4/panel/plugins` on Debian / Ubuntu). It can be forced with `-Dplugin-dir=<path>`.

To remove an unmanaged source installation, use `sudo ninja -C build uninstall` from the corresponding build directory. Do not use this command to remove a Pacman/dpkg-managed installation.

### Building the packages

The [Paquetes](https://github.com/Tantin1/xfce-removable-plugin/actions) workflow builds and test-installs all four packages on every push; pushing a `v<version>` tag creates a draft release with them attached.

By hand:

- **Debian / Ubuntu**: `sh packaging/build-deb.sh` builds a `.deb` for the distribution it runs on.
- **Arch**: `packaging/arch/PKGBUILD` (run `updpkgsums` first; the copy in the repository uses `SKIP`).
- `sh packaging/check-version.sh` checks that the version matches in `src/meson.build`, `debian/changelog` and the PKGBUILD.

## Source layout

```
src/
├── meson.build, meson_options.txt
├── xfce-removable-plugin.desktop.in
├── plugin.c               — XfcePanelPlugin entry point, config, properties and about dialogs
├── popup.h / popup.c      — popup window (X11 and Wayland), device list rendering
├── ud-dbus.h / ud-dbus.c  — UDisks2 backend + USB device classification (GUdev)
├── ud-phones.c            — phones and cameras via gvfs, combined device scan
├── rp-notify.h / rp-notify.c — per-device desktop notifications
└── po/                    — translations (gettext, POTFILES and message catalog)
debian/                    — Debian / Ubuntu packaging
packaging/                 — build-deb.sh, check-version.sh, arch/PKGBUILD
.github/workflows/         — package builds (GitHub Actions)
```

## Configuration

Right-click the panel button → **Properties**. Settings are stored per plugin instance by the panel (`~/.config/xfce4/panel/xfce-removable-plugin-<id>.rc`).

**Appearance**
- Square button / fixed width
- Plugin size (px)
- Use panel icon size / custom icon size (px)
- Popup width (320–800 px) and height (px)
- Popup style: Automatic (floating with rounded corners when a compositor is running and the theme's corners are visibly rounded), Floating, or Attached to the panel

**Behavior**
- Show notifications for device operations
- Hide button when no removable devices
  - Sub-option: also hide when only other devices are present
- Show keyboards and mice (disabled by default)
- Notify when devices are connected or disconnected
- Play a sound when devices are connected or disconnected (requires an enabled desktop sound theme)
- Choose which categories of devices trigger connection notifications and sounds

## Device detection logic

**Storage devices** (`ud_get_devices`): UDisks2 objects that have both `Block` and `Filesystem` interfaces, `HintIgnore=false`, `HintSystem=false`, a valid parent Drive, and `Drive.Removable=true`.

**Phones and cameras** (`ud_get_phones`): gvfs volumes and mounts whose URI scheme is `mtp`, `gphoto2` or `afc`. They are matched to USB devices through `/dev/bus/usb/…` and removed from *Other devices*.

**Other USB devices** (`ud_get_usb_devices`): every external `usb_device` (hubs and ports the firmware reports as `fixed` are skipped), classified from its USB interfaces plus what udev already tagged on its child devices (`ID_INPUT_JOYSTICK`, `ID_INPUT_TABLET`, `ID_SECURITY_TOKEN`, network, Bluetooth and serial ports):
- Shown: Phone / Camera, Gamepad / Joystick, Graphics tablet, Security key / Smart card, Printer, Camera / Webcam, Audio device, MIDI device, Serial device / Board, and other standard-class devices as "USB device" (listed, not notified)
- Hidden by default: keyboards and mice (optional), storage (already in the main list), network adapters and Bluetooth (other panel plugins handle them), and pure HID or vendor-specific devices with no recognizable function

## License

GPL-2.0-only. See [COPYING](COPYING) for the complete GNU GPL version 2 license text.
