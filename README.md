# Bluetooth page turner for Kobo

This plugin adds support to Kobo eReaders for turning pages using buttons on
connected Bluetooth devices.  It has only been tested on a Kobo Libra 2, but I
don't see why it wouldn't work on other Nickel devices.

## Installation

Copy `KoboRoot.tgz` into the `.kobo` folder of your eReader and eject it.

After it installs, you'll see a `.btpt` folder on the device.  Add
configuration files for your Bluetooth devices there, and connect to them using
the standard Kobo Bluetooth settings menu.  You can now use the configured
buttons to change pages in the reading view.

To uninstall it, create a file in `.btpt` called `uninstall` and restart the
device.

### Configuration

You need two pieces of information about a Bluetooth device to configure it:

1. The Bluetooth device name which you can find in the Kobo Bluetooth settings
   menu.

2. The [Linux Input Subsystem event codes][0] that represent the input events
   you want to use to turn pages.  You can find these using the [evtest][1]
   tool, for example.

Once you have those, create a configuration file under `.btpt`.  The filename
should be the exact Bluetooth device name.

The file should have a line for each input mapping in the format:

```
METHOD TYPE CODE VALUE
```

`METHOD` is the name of the ReadingView method to invoke.  `prevPage` and
`nextPage` turn the pages.  `prevChapter` and `nextChapter` are also valid
choices, but I'm not sure if any other methods make sense here.

`TYPE`, `CODE`, and `VALUE` specify the input event values to match.  C-style
integers in decimal, octal, or hexadecimal are accepted, and so are #defines
from `<linux/input-event-codes.h>`. 

[0]: https://www.kernel.org/doc/html/v4.14/input/event-codes.html
[1]: https://cgit.freedesktop.org/evtest/

#### Example configuration

I use an 8BitDo Zero 2 in XInput mode.  I want it to work sideways in either
orientation, so I want Down (65535 on the Y-axis) and the X button to go to the
previous page, and Up (0 on the Y-axis) and the B button to go to the next
page.

In XInput mode, the device identifies itself as "8BitDo Zero 2 gamepad", so
the configuration file is `.btpt/8BitDo Zero 2 gamepad` with the following
contents:

```
prevPage EV_ABS ABS_Y 65535
nextPage EV_ABS ABS_Y 0
prevPage EV_KEY BTN_NORTH 0
nextPage EV_KEY BTN_SOUTH 0
```

This file is also present in the `examples` directory of this repository.

#### Advanced configuration

You can also name configuration files after a device's 48-bit Bluetooth address
(often represented like `11:22:33:44:FF:EE`, though the filename should have no
semicolons, like `11223344FFEE`).  When present, these files take precedence
over files named after the device name, so you can override default behavior
for specific devices.

The addresses are case insensitive, but behavior is undefined if there are
collisions.

## Building from source

`make` builds the shared library, `libbtpt.so`, which can be installed in
/usr/local/Kobo/imageformats.

`make koboroot` builds `KoboRoot.tgz` which can be installed over USB.

Or get pre-built `KoboRoot.tgz` from the release section.
