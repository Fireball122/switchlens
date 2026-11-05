# Switchlens

Use USB-C DisplayPort AR glasses on the Nintendo Switch's own USB-C port. No dock, no adapter
tricks: Switchlens talks to the console's USB-PD controller (BM92T36), puts the port into DisplayPort
Alt Mode, switches the console to docked output, and puts everything back when you unplug.

It is an [Atmosphere](https://github.com/Atmosphere-NX/Atmosphere) boot2 sysmodule plus a
[Tesla](https://github.com/WerWolv/Tesla-Menu)/Ultrahand overlay for the on/off switch.

> **Status:** works on the author's console. Tested only on firmware 23.0.1, Atmosphere 1.12.1, emuMMC,
> one pair of glasses. Anything else is unverified, so please report results (see [Reporting](#reporting)).
> Use at your own risk. This is not affiliated with or endorsed by Nintendo.

## Features

- **DisplayPort Alt Mode on the console's USB-C port**: plug in, flip one switch, the picture moves to the glasses.
- **Clean revert**: unplug and the console goes back to its own screen; replug and AR resumes.
- **Sleep and wake safe**: the module stays out of the way during the sleep transition and re-engages after wake.
- **Joy-Cons keep working** while AR is on. They are presented to games as one wireless Pro Controller
  (this is what makes Mario Kart 8 Deluxe work).
- **Overlay**: live status card (service, USB-C, Joy-Cons) and two switches.

## Not supported / limits

- Gyro through the virtual Pro Controller is **experimental** (`controller.flag` = `2` or `3`) and its result in games is not yet confirmed. See [docs/how-it-works.md](docs/how-it-works.md#gyro-experimental).
- Joy-Con NFC is not forwarded to the virtual controller. If real amiibo do not scan, check whether
  emuiibo emulation is on: emuiibo intercepts all NFC while enabled.
- Switch Lite: not expected to work (no docked output); untested. Other Switch models: unverified.
- Only the BM92T36 USB-PD controller path is implemented.

## Install

1. Power off, put the SD card in your PC.
2. Copy the contents of a release (or of `dist/` after building) onto the SD card root, merging folders:
   - `atmosphere/contents/420000000007E51C/` (`exefs.nsp`, `toolbox.json`, `flags/boot2.flag`)
   - `switch/.overlays/switchlens.ovl`
3. Boot Atmosphere. The state folder `switch/ar_glasses/` is created automatically.

Requires Atmosphere, a Tesla-compatible overlay loader (nx-ovlloader + Tesla Menu or Ultrahand), and glasses
that speak DisplayPort over USB-C.

## Use

Open the overlay menu, choose **Switchlens**, plug in the glasses, switch **AR Display** on. Unplug to return to
the console screen. **Joy-Con Support** (on by default) makes attached Joy-Cons act as a wireless Pro Controller
while AR is on.

## Uninstall / safe mode

- Disable: rename `atmosphere/contents/420000000007E51C/flags/boot2.flag` to `boot2.flag.disabled`, reboot.
- Remove: delete `atmosphere/contents/420000000007E51C`, `switch/.overlays/switchlens.ovl`, `switch/ar_glasses`.
- Sleep problems: create an empty `switch/ar_glasses/no_psc.flag` to skip all sleep handling, reboot, and report it.

## Files in `switch/ar_glasses/`

| File | Meaning |
|---|---|
| `active.flag` | `1` AR wanted, `0` off. Written by the overlay. |
| `controller.flag` | `0` off, `1` Joy-Cons as Pro Controller (default when missing), `2`/`3` plus experimental gyro. |
| `state.bin` | Live status for the overlay (see docs). |
| `log.txt`, `log_prev.txt` | Current and previous boot log. |
| `no_psc.flag` | Optional: disables sleep handling. |

## Build

Needs devkitPro (devkitA64, libnx, switch-tools), Python 3, and for the overlay
[libtesla](https://github.com/WerWolv/libtesla).

```
python build.py all --libtesla /path/to/libtesla     # or: sysmodule | overlay
```

Output goes to `dist/` in SD-card layout. `DEVKITPRO` selects the toolchain (defaults to `C:\devkitPro` on Windows,
`/opt/devkitpro` elsewhere). `probes/` holds the standalone homebrew probes used during research; see the docs.

## Reporting

Please include: firmware and Atmosphere version, Switch model, glasses model, what happened, and
`switch/ar_glasses/log.txt` / `log_prev.txt`, plus anything in `atmosphere/crash_reports`.

## How it works

[docs/how-it-works.md](docs/how-it-works.md)

## License

GPL-2.0-or-later, see [LICENSE](LICENSE). The overlay links [libtesla](https://github.com/WerWolv/libtesla)
(GPL-2.0-or-later); the whole project uses one license for simplicity. Built on
[libnx](https://github.com/switchbrew/libnx) (ISC), Atmosphere, and the Switchbrew documentation.
