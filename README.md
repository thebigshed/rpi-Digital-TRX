# rpi-Digital-TRX

A stand-alone digital voice network radio on a Raspberry Pi 4 or Pi Zero 2 W. It is the hardware version of
[Digital Transceiver](https://github.com/thebigshed/digital-transceiver): it listens and
transmits on DMR, D-Star, YSF, P25, NXDN and M17, and receives DAPNET pages, over Wi-Fi.
There's no desktop. It boots straight into the radio, and you drive it from a front panel: mode
buttons with RGB LEDs, a channel knob, a volume knob and a PTT button, with a 20×4 LCD.

## Hardware

| Part | Used for | Connection |
|---|---|---|
| Raspberry Pi 4, or Pi Zero 2 W | Runs everything | — |
| NW Digital Radio DV3000 (ASSY1101, AMBE-3000R) | AMBE / AMBE+2 encode and decode in hardware: DMR, D-Star, YSF (DN), NXDN | First 26 header pins: serial port `/dev/ttyAMA0` at 230400 baud, reset on GPIO 4 |
| 20×4 LCD (HD44780) with a PCF8574 I2C backpack | Display | I2C at 0x27 or 0x3F, on the 5 V side of an I2C level shifter |
| Front panel ([wiring](docs/wiring.md)) | Six mode buttons, Menu, PTT, two KY-040 rotary encoders (channel and volume) | GPIO, through a 40-pin splitter |
| Two MCP23017 I/O expander modules | The seven common-cathode RGB LEDs (six modes and status) | I2C at 0x21 and 0x22, powered from 5 V on the 5 V side of the level shifter |
| USB headset | Speaker and microphone; its call button toggles PTT | USB |
| Adafruit RGB LCD Pi Plate (optional) | The old display and five buttons, used if the 20×4 isn't fitted | I2C at 0x20 |

The AMBE board sits on the first 26 header pins, and the panel is wired to the other side of a
"1 to 2" header splitter. They use different pins: the AMBE board uses the serial port
(GPIO 14/15) and GPIO 4, and the panel uses I2C (GPIO 2/3) and other GPIO pins. See
[docs/wiring.md](docs/wiring.md).

### Codecs

| Mode | Codec | Where it runs |
|---|---|---|
| DMR, NXDN, YSF DN | AMBE+2 | AMBE board |
| D-Star | AMBE | AMBE board |
| P25, YSF VW | IMBE | Software (OP25 / mbelib), easily fast enough on a Pi 4 |
| M17 | Codec2 | Software |

## Design

- **OS**: a minimal Linux image built with [Buildroot](https://buildroot.org) (2026.02 LTS).
  It has no desktop. It holds only what the radio needs: Qt 6 Core and Network (no Qt GUI,
  fonts or display platform), PulseAudio (system mode), wpa_supplicant, chrony, Dropbear SSH and
  i2c-tools.
- **App** (`app/`): the radio. It started as the Digital Transceiver desktop app and is now
  cut down to what the Pi needs: there are no windows or dialogs, and it is a plain Qt Core
  program. It is in three layers:
  - `Radio` (`Radio.cpp`, `RadioNet.cpp`): networks, decoding, calls, transmit, logs and
    recordings. It has no user interface.
  - `FrontPanel`: what the display shows and what each control does. It knows nothing about
    the hardware: displays implement `Display`, and input devices call `press()`,
    `release()`, `turn()` and `togglePtt()`.
  - Hardware: `GpioPanel` (the panel's buttons and encoders, as kernel GPIO events),
    `LedPanel` (the LEDs), `Lcd2004` (the 20×4 display), `LcdPlate` (the old 16×2 plate), `HeadsetButtons` (the
    headset's call button) and `SimPanel` (a stand-in for testing on a PC). Each part is
    optional; the radio logs what it can't find and carries on.

  Settings, call logs and recordings are the same as the desktop app's, so its
  `settings.json` can be copied over.
- **Display** (20×4):

  ```
  DSTR REF001 E  12:41     TX mode, where it transmits, time
  M6OZE            DSTR    the call on air: callsign and mode
  Ozzy                     name
  >CQCQCQ          0:07    target and call time
  ```

  When no call is on air, lines 2 and 3 show the last call heard, and the bottom line shows the
  IP address and volume, or a problem (a failed login, or the Pi-Star online). While browsing destinations it
  shows the one selected and the next. If the 20×4 isn't fitted, the Adafruit plate's 16×2
  shows a two-line version.
- **Controls** (full details in [docs/wiring.md](docs/wiring.md#what-the-controls-do)):

  | Control | Action |
  |---|---|
  | Mode button | Tap: listen to the mode or not. Hold: make it the TX mode |
  | Channel knob | Turn: step through the TX mode's destinations (reflectors, talkgroups, and for DMR the talkgroups listed in `settings.json`). Push: go there. Push and turn: D-Star / M17 module letter |
  | Volume knob | Turn: volume. Push: mute |
  | PTT | Hold to talk |
  | Menu | Tap: back. Hold 3 s: shut down; unplug when the display says "Safe to unplug" |
  | Headset call ("Teams") button | PTT on / off: press to start transmitting, press again to stop (the button only clicks, so it can't be held) |
  | LCD plate | Select: PTT, or go to the destination shown. Up / Down: TX mode. Left / Right: browse |

- **LEDs**: each mode's LED is green when listening, red when it's the TX mode (yellow for
  both) and blue while a call is on air in it. The status LED is red while transmitting, green
  while receiving and blue for a network problem or an unread page.

- **AMBE board**: the app opens the DV3000 with `--ambe /dev/ttyAMA0`. It resets the board on
  GPIO 4 (header pin 7) and talks to it at 230400 baud using DVSI's packet protocol
  (`app/src/AmbeChip.cpp`).
  - D-Star frames go to the chip as they are.
  - DMR, YSF DN and NXDN go to the chip as the 49 AMBE+2 voice bits, after mbelib's error
    correction (rate 34). The chip's bit order differs from mbelib's; the mapping was measured
    on the chip and is built in.
  - The chip has one decoder, so it follows one call at a time; any other call is decoded in
    software.
  - P25 (IMBE) and M17 (Codec2) are always decoded in software.
  - Check a chip with
    `digital-transceiver --ambe /dev/ttyAMA0 --ambe-test recording.wav`. It runs a recording
    through every chip and software combination and compares the result.

## Building

| Board | Command | Image |
|---|---|---|
| Raspberry Pi 4 | `./build.sh` | `output/images/sdcard.img` |
| Raspberry Pi Zero 2 W | `BOARD=zero2w ./build.sh` | `output-zero2w/images/sdcard.img` |

The first build for a board fetches Buildroot 2026.02.3 (once) and builds everything, which
takes a while. Each board has its own output folder, and the downloaded sources are shared. The
app is built from `app/` in this project. To rebuild only the app after changing it, run
`./build.sh digital-transceiver-rebuild all`, with `BOARD=zero2w` in front for the Zero.

`build.sh` reconfigures Buildroot when `external/configs/dtrx.fragment` or the board's fragment
changes. Buildroot leaves removed packages in the image until a clean build, so after removing
any, run `./build.sh clean all`.

### Raspberry Pi Zero 2 W

The Zero 2 W image runs the same software with the same hardware: the DV3000 fits the first 26
header pins, and the front panel wiring is unchanged. The differences are:

- **USB:** the headset goes in the **USB** port (the inner micro-USB, not PWR IN) through a
  micro-USB OTG adapter. Anything else needs a small hub.
- **No Ethernet:** Wi-Fi is the only network, so `wpa_supplicant.conf` must be on the boot
  partition before the first boot. Wi-Fi is 2.4 GHz only.
- **No analog audio:** the USB headset is the only audio device.
- **Header:** the plain Zero 2 W has no header fitted. Use a "WH" board, or solder one on.
- **Power:** a 5 V 2.5 A supply is plenty.

### Trying the app on a PC

The app also builds on a Linux PC with Qt 6 (Core and Network) and, for M17, Codec2:

```sh
cmake -S app -B app/build && cmake --build app/build -j
app/build/digital-transceiver --sim
```

With `--sim` (or `--sim-16x2` for the plate's layout), the display and LEDs are printed to the terminal and the controls are typed, one
per line (`+` = press, `-` = release): `1+`…`6-` the mode buttons, `t+`/`t-` and `v+`/`v-` turn
the channel and volume knobs, `k+`/`k-` push the channel knob, `m` pushes the volume knob,
`p+`/`p-` is PTT, `n+`/`n-` Menu and `h` the headset button. `s`, `u`, `d`, `l` and `r` are the
LCD plate's buttons. It uses the PC's settings in
`~/.config/digital-transceiver/digital-transceiver/`, so it logs in to the networks with
them; don't run it while the radio or the desktop app is online with the same IDs.

## Setting up the SD card

1. Write the board's image to the card (`output/images/sdcard.img` for the Pi 4,
   `output-zero2w/images/sdcard.img` for the Zero 2 W), for example with
   `sudo dd if=output/images/sdcard.img of=/dev/sdX bs=4M conv=fsync`. Write to the whole card
   (`/dev/sdX`), not a partition (`/dev/sdX1`).
2. On the card's boot partition (FAT, readable on any PC), add:

   | File | What it's for |
   |---|---|
   | `wpa_supplicant.conf` | Your Wi-Fi network; copy `boot-examples/wpa_supplicant.conf` and fill it in |
   | `settings.json` | Your Digital Transceiver settings; copy the desktop app's `~/.config/digital-transceiver/digital-transceiver/settings.json` |
   | `authorized_keys` | Optional: SSH public keys, for `ssh root@dtrx` |

   `settings.json` is copied in on the first boot and then renamed `settings.json.applied`, so
   changes made on the radio are kept.
3. Plug in the USB headset and boot. To turn the radio off, hold Menu for 3 seconds and unplug
   it when the display says "Safe to unplug". The app's log is `/var/log/dtrx.log`, and its event log
   is in `/root/.local/share/digital-transceiver/digital-transceiver/events.log`.

Don't run the radio and the desktop app (or a Pi-Star) at the same time with the same
settings: they would log in to the networks with the same IDs. Give the Pi its own DMR hotspot
ID (for example your DMR ID plus `03`) in its `settings.json`.

## Layout

```
build.sh            Configure and build the SD card image
app/                The radio app (CMake, Qt 6 Core and Network)
  src/              Radio, FrontPanel, the hardware drivers, decoders and network clients
  third_party/      mbelib (AMBE / IMBE decoding) and the OP25 vocoder (encoding)
external/           Buildroot external tree (BR2_EXTERNAL "DTRX")
  configs/          dtrx.fragment (all boards), rpi4.fragment and zero2w.fragment: added to
                    Buildroot's raspberrypi4_64 / raspberrypizero2w_64 defconfig
  package/          codec2, and digital-transceiver (built from app/)
  board/common/     post-build.sh and the rootfs overlay, for all boards
                    (init scripts: boot-partition config, Wi-Fi, the app)
  board/rpi4/       Pi 4 firmware config.txt and cmdline.txt
  board/zero2w/     Zero 2 W firmware config.txt and cmdline.txt
boot-examples/      Example files for the boot partition
buildroot/          Buildroot (fetched by build.sh, not committed)
output/             Pi 4 build output (not committed)
output-zero2w/      Zero 2 W build output (not committed)
```

## Next steps

1. **Menu**: a menu for RX modes, DMR talkgroup, volume and IP address (a long Menu press is
   now shutdown, so it will need another way in, such as pushing both knobs).

## Licence

GPL-3.0-or-later, the same as Digital Transceiver, whose code it reuses. mbelib (`app/third_party/mbelib`)
and OP25 (`app/third_party/op25_vocoder`) keep their own licences.
