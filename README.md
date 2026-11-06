# Switchlens

Use USB-C DisplayPort AR glasses on the Nintendo Switch's own USB-C port. No dock and no adapter.
Switchlens talks to the console's USB-PD controller (BM92T36), enables DisplayPort Alt Mode, switches the
console to docked output, and restores everything when you unplug the glasses.

It is an [Atmosphere](https://github.com/Atmosphere-NX/Atmosphere) boot2 system module plus a
[Tesla](https://github.com/WerWolv/Tesla-Menu)/[Ultrahand](https://github.com/ppkantorski/Ultrahand-Overlay) overlay for the on/off switch.

## Read this first

- **Experimental. Tested on one setup only:** firmware 23.0.1, Atmosphere 1.12.1, emuMMC, one pair of glasses.
  Other firmware, Atmosphere versions, glasses, and Switch models (Lite, OLED) are **untested**.
- **It runs at boot and changes display output and sleep behaviour.** A bug can freeze the console, leave it
  with a black screen, or make it misbehave on wake. Back up your SD card before installing, and read
  [Safe mode](#safe-mode) before you start.
- **Not affiliated with or endorsed by Nintendo.** Provided as-is, without warranty (see [LICENSE](LICENSE)).
- **Gyro is not enabled in this release.** The code has an experimental gyro path that is off by default.
  Do not set `controller.flag` to `2` or `3`; those values are for development and are not tested.
- **NFC is not forwarded.** emuiibo (if installed and emulation is on) intercepts NFC, so real amiibo may not
  scan. Turn emuiibo emulation off to test real amiibo.
- **Joy-Con Support is on by default.** While AR is on, attached Joy-Cons act as one wireless Pro Controller.
  This changes how controller input reaches games; turn it off in the overlay if you see problems.

## Features

- **DisplayPort Alt Mode on the console's USB-C port.** Plug in, turn AR on, and the picture moves to the glasses.
- **Clean revert.** Unplug and the console returns to its own screen. Replug and AR comes back.
- **Sleep and wake handling.** The module stays out of the way during the sleep transition and re-engages after wake.
- **Joy-Cons keep working while AR is on**, presented to games as a wireless Pro Controller (this is what makes
  Mario Kart 8 Deluxe work).
- **Overlay status card**: service state, USB-C state, Joy-Con state, and two switches (AR Display, Joy-Con Support).

## Requirements

- Nintendo Switch with [Atmosphere](https://github.com/Atmosphere-NX/Atmosphere) (tested 1.12.1) and firmware 23.0.1.
- A Tesla-compatible overlay loader: [nx-ovlloader](https://github.com/ppkantorski/nx-ovlloader) plus
  Tesla Menu or Ultrahand, installed separately. See the [ConsoleMods overlay guide](https://consolemods.org/wiki/Switch:Overlay_Apps).
- AR glasses that accept DisplayPort over USB-C.

## Install (release zip)

1. Download `Switchlens-v1.0.0.zip` from the Releases page. Back up your SD card.
2. Extract it. You get two folders: `atmosphere/` and `switch/`.
3. Copy both folders to the **root** of the SD card, merging with what is already there.
4. Put the SD card back in the Switch and boot it.
5. Open the overlay menu, choose **Switchlens**, plug in the glasses, then turn on **AR Display**.

What you should see:
- The status card shows the service as running within a few seconds of boot.
- After plugging in the glasses and turning on AR Display, the USB-C row changes to connected and the picture moves to the glasses.
- Unplugging returns the picture to the console.

The first boot creates `switch/ar_glasses/` and writes `log.txt`.

## Use

- **AR Display**: on or off. Off gives the screen back to the console but leaves the glasses connected.
- **Joy-Con Support**: on by default. Off stops the virtual Pro Controller and returns Joy-Con input to the console.

## Safe mode

- **Disable the module:** rename `atmosphere/contents/420000000007E51C/flags/boot2.flag` to `boot2.flag.disabled`, then reboot.
- **Skip sleep handling:** create an empty file `switch/ar_glasses/no_psc.flag`, then reboot.
- **Remove:** delete `atmosphere/contents/420000000007E51C/`, `switch/.overlays/switchlens.ovl`, and `switch/ar_glasses/`.

If the console freezes at boot, boot into safe mode (or edit the SD card on a PC) and disable the module as above.

## Files in `switch/ar_glasses/`

| File | Meaning |
|---|---|
| `active.flag` | `1` AR wanted, `0` off. Written by the overlay. |
| `controller.flag` | `0` off, `1` Joy-Cons as Pro Controller (default when missing). `2`/`3` are experimental gyro values; do not use. |
| `state.bin` | Live status for the overlay (see the docs). |
| `log.txt`, `log_prev.txt` | Current and previous boot logs. |
| `no_psc.flag` | Optional: disables sleep handling. |

## Reporting problems

Please include: firmware and Atmosphere version, Switch model, glasses model, what you did, what happened, and
`switch/ar_glasses/log.txt` and `log_prev.txt`. Also include any new files in `atmosphere/fatal_reports/` or
`atmosphere/crash_reports/` made around the time of the problem. Open an issue on this repository.

## Build from source

Needs devkitPro (devkitA64, libnx, switch-tools), Python 3, and for the overlay
[libtesla](https://github.com/WerWolv/libtesla).

```
python build.py all --libtesla /path/to/libtesla     # or: sysmodule | overlay
```

Output goes to `dist/` in SD-card layout. `DEVKITPRO` selects the toolchain (defaults to `C:\devkitPro` on Windows,
`/opt/devkitpro` elsewhere). Source for the standalone probes used during research is in `probes/`.

## How it works

See [docs/how-it-works.md](docs/how-it-works.md).

## License

GPL-2.0-or-later, see [LICENSE](LICENSE). The overlay links [libtesla](https://github.com/WerWolv/libtesla)
(GPL-2.0-or-later). Built on [libnx](https://github.com/switchbrew/libnx) (ISC), Atmosphere, and the Switchbrew documentation.
