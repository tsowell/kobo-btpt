# Bluetooth page turner for Kobo (Plus)

This plugin adds support to Kobo eReaders for turning pages using buttons on
connected Bluetooth devices (such as Bluetooth page-turner rings, clickers,
camera shutters, and gamepads).

It supports both **Kobo Firmware 4.x (Qt 5)** and **Kobo Firmware 5.x / 6.x (Qt 6)**,
and has been tested on devices including the **Kobo Clara BW** (firmware 6.0.279225)
and **Kobo Libra 2**.

## What's New in Plus

- **Kobo Firmware 5.x & 6.x (Qt 6) Compatibility**: Native build and packaging support (`Kobo.tgz`) for modern Qt 6 Kobo firmware (such as 6.0.279225 on Kobo Clara BW / Clara Colour / Libra Colour).
- **Multi-Plugin Compatibility (`NickelHook`)**: Adds unique plugin metadata (`nh.btpt`) so `libbtpt.so` coexists cleanly with other mods (e.g., NickelMenu, NickelClock) and uninstalls reliably.
- **Multi-Interface Bluetooth Device Support**: Supports Bluetooth rings and controllers that register multiple `/dev/input/eventX` handlers under the same Bluetooth address.
- **Resilient Symbol Resolution**: Non-essential internal firmware symbols (`BluetoothHeartbeat`, `PowerManager`, `TimeEvent`) are resolved optionally with null checks so firmware changes do not break page turning.
- **Bug Fixes & Stability**:
  - Fixed a use-after-free bug when invoking `ReadingView` methods with longer names (such as `goToNextChapter`).
  - Automatic fallback from `nextChapter` / `prevChapter` to `goToNextChapter` / `goToPrevChapter`.
  - Fixed file descriptor (`fd`) leaks on device disconnect and configuration errors.
  - Added support for comments (`#`) and blank lines in device configuration files.

## Installation

### Firmware 5.x and 6.x (Qt 6 — e.g. Kobo Clara BW, Clara Colour, Libra Colour on 6.0+)

1. Download **`Kobo.tgz`** from the [Releases](../../releases) page.
2. Connect your Kobo to your computer via USB and copy **`Kobo.tgz`** into the `.kobo` folder of your eReader.
3. Safely eject and unplug the eReader. It will apply the update and restart.

> **Note:** Full rootfs Kobo firmware updates replace `/usr/local/Kobo`, so reinstall `Kobo.tgz` after updating your Kobo firmware (`your .btpt` configurations on onboard storage are preserved).

### Firmware 4.x (Qt 5 — e.g. Kobo Libra 2, Sage, Elipsa, Clara 2E, or 4.38–4.44 firmware)

1. Download **`KoboRoot.tgz`** from the [Releases](../../releases) page.
2. Connect your Kobo to your computer via USB and copy **`KoboRoot.tgz`** into the `.kobo` folder of your eReader.
3. Safely eject and unplug the eReader.

---

After installation, you will see a `.btpt` folder on the device. Add
configuration files for your Bluetooth devices there (see examples in the
[`examples`](./examples) directory), and connect to them using the standard Kobo
Bluetooth settings menu. You can now use the configured buttons to change pages
in the reading view.

### Uninstalling

To uninstall the plugin, create an empty file in `.btpt` called `uninstall`
(no file extension) and restart the device.

## Configuration

You need two pieces of information about a Bluetooth device to configure it:

1. The Bluetooth device name which you can find in the Kobo Bluetooth settings
   menu.

2. The [Linux Input Subsystem event codes][0] that represent the input events
   you want to use to turn pages. You can find these using the [evtest][1]
   tool, for example.

Once you have those, create a configuration file under `.btpt`. The filename
should be the exact Bluetooth device name (for example `.btpt/BLE-M3` or
`.btpt/8BitDo Zero 2 gamepad`).

The file should have a line for each input mapping in the format:

```
METHOD TYPE CODE VALUE
```

- `METHOD` is the name of the `ReadingView` method to invoke:
  - `prevPage` and `nextPage` turn the pages.
  - `goToNextChapter` (or `nextChapter`) and `prevChapter` navigate chapters.
- `TYPE`, `CODE`, and `VALUE` specify the input event values to match. C-style
  integers in decimal, octal, or hexadecimal are accepted, and so are `#define`
  names from `<linux/input-event-codes.h>`.
- Blank lines and comments starting with `#` are ignored.

[0]: https://www.kernel.org/doc/html/v4.14/input/event-codes.html
[1]: https://cgit.freedesktop.org/evtest/

### Example configurations

Ready-to-use configuration files are available in the [`examples`](./examples) directory:

- **`BLE-M3`** (Bluetooth ring / remote page turner)
- **`D01` / `D01 Pro` / `D02` / `T01`** (Bluetooth page-turner rings)
- **`CheerTok Air`**
- **`8BitDo Zero 2 gamepad`** (XInput mode)
- **`8bitDo Micro gamepad`** (Keyboard mode)
- **`LIFT`** (Logitech Lift Vertical Ergonomic Mouse)

For example, `.btpt/8BitDo Zero 2 gamepad`:

```
prevPage EV_ABS ABS_Y 65535
nextPage EV_ABS ABS_Y 0
prevPage EV_KEY BTN_NORTH 0
nextPage EV_KEY BTN_SOUTH 0
```

### Advanced configuration (MAC address)

You can also name configuration files after a device's 48-bit Bluetooth address
(often represented like `11:22:33:44:FF:EE`, though the filename should have no
colons, like `11223344FFEE`). When present, these files take precedence over
files named after the device name, so you can override default behavior for
specific devices.

The addresses are case-insensitive.

## Building from source

### Firmware 4.x (Qt 5)

With [NickelTC](https://github.com/pgaskin/NickelTC) on your `PATH`:

- `make` builds the shared library, `libbtpt.so`.
- `make koboroot` builds **`KoboRoot.tgz`** for Qt 5 firmware.

### Firmware 5.x and 6.x (Qt 6)

With a Kobo Qt 6.5.2 cross-toolchain (`arm-kobo-linux-gnueabihf-`, such as `ghcr.io/nicoverbruggen/kobuild:qt6-v0.5.3`) on your `PATH`:

```sh
make NH_QT_MAJOR=6 clean all kobo
```

This builds **`Kobo.tgz`**, which installs `imageformats/libbtpt.so` under `/usr/local/Kobo`.
