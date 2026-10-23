# xfce-removable-plugin

Panel plugin for Xfce that manages removable storage devices and displays other connected USB devices.

Inspired by KDE Plasma's Device Notifier, built natively for Xfce using UDisks2 and GUdev.

## Features

- Lists removable storage devices (USB drives, SD cards, external disks) via UDisks2
- Groups partitions under their parent disk with expand/collapse
- Mount / Unmount buttons per partition with intermediate state feedback ("Mounting…" / "Unmounting…")
- Eject button on the disk header — unmounts all partitions before ejecting
- **Phones and cameras**: Android phones in file-transfer mode (MTP), cameras (PTP) and iPhones (afc), via gvfs — mount, open in the file manager, safely remove
- **Other devices** section: joysticks, gamepads, printers, audio devices, webcams — detected via GUdev, filtered by USB interface class
- Desktop notifications on connect/disconnect (libnotify), configurable
- Opens Thunar after mounting, configurable
- Hides panel button when no devices are present, configurable
- Scrollable popup with configurable width and height
- Persistent configuration via GKeyFile (`~/.config/xfce4/panel/xfce-removable-plugin.ini`)
- Configurable button size (square or fixed width) and icon size
- Auto-refresh via DBus signals (InterfacesAdded/Removed, PropertiesChanged) + GUdev uevents with 300ms debounce

## Dependencies

| Library | Version |
|---|---|
| GTK+ 3 | ≥ 3.24 |
| libxfce4panel | ≥ 4.18 |
| libnotify | ≥ 0.7 |
| gudev-1.0 | any |
| GLib / GIO | (bundled with GTK) |
| gvfs, gvfs-mtp / gvfs-gphoto2 / gvfs-afc | runtime, optional (phones and cameras section) |

## Build

```bash
meson setup build src -Dprefix=/usr
meson compile -C build
sudo meson install -C build
xfce4-panel --quit && sleep 1 && xfce4-panel &
```

Then add **Removable Devices** from the Xfce panel's "Add New Items" dialog.

## Source layout

```
src/
├── meson.build
├── xfce-removable-plugin.desktop.in
├── plugin.c          — XfcePanelPlugin entry point, config, properties dialog
├── popup.h / popup.c — GTK popup window, device list rendering
├── ud-dbus.h / ud-dbus.c — UDisks2 DBus backend + GUdev USB enumeration
└── po/               — i18n (gettext)
```

## Configuration

Right-click the panel button → **Properties**.

**Appearance**
- Square button / fixed width
- Plugin size (px)
- Use panel icon size / custom icon size (px)
- Popup width and height (px)

**Behavior**
- Show desktop notifications
- Open Thunar after mounting
- Hide button when no removable devices
  - Sub-option: also hide when only non-storage USB devices are present
- Notify when devices are connected or disconnected

## Device detection logic

**Storage devices** (`ud_get_devices`): UDisks2 objects that have both `Block` and `Filesystem` interfaces, `HintIgnore=false`, `HintSystem=false`, a valid parent Drive, and `Drive.Removable=true`.

**Other USB devices** (`ud_get_usb_devices`): GUdev enumeration of `usb_device` nodes, filtered by USB interface class:
- Excluded: hubs (class 09), pure storage (usb-storage/uas driver), pure HID without vendor class (keyboards/mice/receivers), pure vendor class without HID (WiFi dongles), Bluetooth (class e0)
- Included and typed: Phone / Camera (06 or `ID_MTP_DEVICE`, only when gvfs does not list it), Printer (07), Audio device (01), Camera/Webcam (0e), Gamepad/Joystick (03+ff combined)

**Phones and cameras** (`ud_get_phones`): gvfs volumes and mounts whose URI scheme is `mtp`, `gphoto2` or `afc`. They are matched to USB devices through `/dev/bus/usb/…` and removed from *Other devices*.

## License

GPL-2.0
