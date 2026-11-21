# Front panel wiring

This panel gives the radio:
- a 20×4 display;
- six mode buttons with LEDs;
- a status LED;
- two rotary encoders, one for **channel** and one for **volume**;
- a PTT button.

It replaces the Adafruit LCD plate. The DV3000 AMBE board stays as it is. A diagram of the whole
panel is in [`wiring.svg`](wiring.svg).

> The radio drives all of it: the display (`app/src/Lcd2004.cpp`), the buttons and encoders
> (`app/src/GpioPanel.cpp`) and the LEDs (`app/src/LedPanel.cpp`). Each part is optional: if one
> is missing, the radio logs it in `events.log` and carries on. If the 20×4 display isn't found,
> it uses the Adafruit plate instead, if that's fitted.
>
> The diagram is also available as [`wiring.png`](wiring.png). To redraw it after a change, edit
> and run `python3 make_wiring_svg.py wiring.svg`.

## Parts

| Qty | Part | Notes |
|---|---|---|
| 1 | 20×4 character LCD (HD44780) with an I2C backpack (PCF8574) | The display. If yours has no backpack, see [The 20×4 display](#the-204-display) |
| 1 | I2C level shifter (2-channel or more, BSS138 type) | Joins the Pi's 3.3 V I2C bus to a 5 V one for the display and both LED modules |
| 2 | 16-channel I/O expander modules (MCP23017, e.g. CJMCU-2317) | Drive all the LEDs: module **A** at 0x21, module **B** at 0x22 |
| 7 | 5 mm RGB LEDs, **common cathode** | 6 mode LEDs plus 1 status LED, all full RGB |
| 8 | 5 mm momentary push buttons | 6 modes, Menu and PTT (PTT can be a bigger button) |
| 2 | KY-040 rotary encoders | **Channel**: turn = browse reflectors or talkgroups, push = link. **Volume**: turn = volume, push = mute |
| 7 | 470 Ω resistors | One per red leg |
| 14 | 330 Ω resistors | One per green and blue leg |
| 1 | 40-pin GPIO splitter ("1 to 2" expansion board) or extra-tall stacking header | See [Getting at the pins](#getting-at-the-pins) |
| — | Hook-up wire, stripboard or perfboard | |
| *(optional)* | 3.5 mm jack | For a fist-mic or foot-switch PTT, wired in parallel with the PTT button |

## How it's laid out

| What | Connects to | Why |
|---|---|---|
| Buttons, PTT, encoder | Pi GPIO pins directly | The kernel reads them by interrupt and debounces them, so they react instantly with no polling |
| LEDs | Two MCP23017 modules on I2C, addresses **0x21** and **0x22** | 7 RGB LEDs need 21 outputs; module A takes five mode LEDs, module B the M17 and status LEDs, with 10 outputs spare |
| LED modules' supply | **5 V**, on the 5 V side of the level shifter with the display | From 3.3 V the outputs couldn't light green and blue properly (they need about 3 V); from 5 V every colour lights evenly |
| LED wiring | Each LED's common leg to **GND** | The LEDs are common cathode, so each colour is lit by driving its output high |
| 20×4 display | I2C, address **0x27** (or 0x3F), on the 5 V side of the level shifter | Four lines leave room for the call, name, target and status |
| Adafruit LCD plate | **Removed** (still works if fitted instead of the 20×4) | The 20×4 display and the panel's buttons take over from it |
| DV3000 | Unchanged (serial on GPIO 14/15, reset on GPIO 4) | Those pins are left alone |

## Getting at the pins

The DV3000 sits on the first 26 header pins, so those pins are covered. The panel needs some of
them (3.3 V, 5 V, I2C, GPIO 18, 22, 23, 24 and 27). Use either of these:

- **A 40-pin GPIO splitter** ("1 to 2" or "1 to 3" expansion board) on the Pi. The DV3000 plugs
  into one side, and the panel wires go to the other. This is the easiest option.
- **An extra-tall 2×20 stacking header** on the Pi under the DV3000. Then solder or crimp to the
  pins that stick out at the side.

Pins 27–40 are not used by the DV3000, so the mode buttons, Menu and the volume encoder can use
them either way.

## Pi header connections

Pin numbers are physical header pins; GPIO numbers are BCM.

| Header pin | Signal | Goes to |
|---|---|---|
| 1 | 3.3 V | Both KY-040s **+**, level shifter **LV** |
| 2 | 5 V | Level shifter **HV**, 20×4 display backpack **VCC**, both MCP23017 modules **VCC** (and their A0/A1 and RESET pins as below) |
| 3 | GPIO 2 (SDA) | Level shifter **LV1** only |
| 5 | GPIO 3 (SCL) | Level shifter **LV2** only |
| 6 | GND | Both MCP23017 modules **GND**, the LEDs' common legs, both KY-040s **GND**, display **GND**, level shifter **GND** |

The level shifter makes a second, 5 V I2C bus from **HV1** (SDA) and **HV2** (SCL). The display
and both LED modules connect to that bus, never straight to the Pi's pins 3 and 5: their pull-up
resistors go to 5 V, which would damage the Pi.
| 7 | GPIO 4 | **DV3000 reset: don't use** |
| 8, 10 | GPIO 14, 15 | **DV3000 serial: don't use** |
| 11 | GPIO 17 | **Used by the DV3000** (it holds this pin low): don't use |
| 18 | GPIO 24 | **Channel** encoder CLK |
| 13 | GPIO 27 | **Channel** encoder DT |
| 15 | GPIO 22 | **Channel** encoder SW (push) |
| 12 | GPIO 18 | **Volume** encoder SW (push = mute) |
| 16 | GPIO 23 | **PTT** button (other side to GND) and optional PTT jack |
| 29 | GPIO 5 | **DMR** button |
| 31 | GPIO 6 | **D-Star** button |
| 32 | GPIO 12 | **YSF** button |
| 33 | GPIO 13 | **P25** button |
| 35 | GPIO 19 | **NXDN** button |
| 36 | GPIO 16 | **M17** button |
| 37 | GPIO 26 | **Menu / Back** button |
| 30, 34, 39 | GND | Common ground for all the buttons |
| 38 | GPIO 20 | **Volume** encoder CLK |
| 40 | GPIO 21 | **Volume** encoder DT |
| 22 | GPIO 25 | Spare (e.g. a second PTT or a foot switch later) |

Every button goes **between its GPIO pin and GND**. No resistors are needed: the Pi's internal
pull-ups are switched on in software.

## MCP23017 modules

Both modules connect in parallel to the same four wires: **5 V**, GND, and SDA and SCL from the
level shifter's **HV1** and **HV2**. They differ only in their address pins.

### Addresses

| Pin | Module **A** (0x21) | Module **B** (0x22) |
|---|---|---|
| A0 | **VCC** (5 V) | GND |
| A1 | GND | **VCC** (5 V) |
| A2 | GND | GND |
| RESET | VCC | VCC |

Most modules already pull RESET up; check yours. Some have solder jumpers for A0–A2 instead of
pins.

> **Warning: modules with A0–A2 jumper pads.** On the common green module (header ITB, ITA,
> RST, SDA, SCL, GND, VCC), each of A0–A2 has three pads: GND, the address pin, and VCC. A 0 Ω
> resistor joins the address pin to GND as supplied. To set a pin to VCC, **move** that resistor
> to the VCC side (or remove it and bridge the address pin to VCC). Never add a solder bridge to
> VCC with the resistor still in place: that shorts VCC straight to GND. Before connecting,
> meter VCC to GND on the module: it must not read near 0 Ω. 0x20 is left free so the Adafruit plate can still be fitted. If both modules have their own
I2C pull-up resistors (often 10 kΩ), that's fine together with the Pi's; there's no need to
remove them.

### Module A (0x21): DMR, D-Star, YSF, P25, NXDN

DMR, D-Star and YSF run blue, green, red; P25 and NXDN run red, blue, green, as they are wired on
the prototype panel.

| Pin | LED | Colour |
|---|---|---|
| GPA0 | DMR | Blue |
| GPA1 | DMR | Green |
| GPA2 | DMR | Red |
| GPA3 | D-Star | Blue |
| GPA4 | D-Star | Green |
| GPA5 | D-Star | Red |
| GPA6 | YSF | Blue |
| GPA7 | YSF | Green |
| GPB0 | YSF | Red |
| GPB1 | P25 | Red |
| GPB2 | P25 | Blue |
| GPB3 | P25 | Green |
| GPB4 | NXDN | Red |
| GPB5 | NXDN | Blue |
| GPB6 | NXDN | Green |
| GPB7 | — | Spare |

### Module B (0x22): M17 and status

| Pin | LED | Colour |
|---|---|---|
| GPA0 | M17 | Blue |
| GPA1 | M17 | Green |
| GPA2 | M17 | Red |
| GPA3 | Status | Blue (network fault / new page) |
| GPA4 | Status | Green (RX: a call is being heard) |
| GPA5 | Status | Red (TX) |
| GPA6 – GPA7, GPB0 – GPB7 | — | Spare (10 outputs), e.g. a DAPNET/pager LED or a signal-level bar later |

**Without module B**, the M17 and status LEDs can go straight to Pi GPIO pins instead: M17 blue,
green, red on GPIO 10, 9, 11 (pins 19, 21, 23) and status on GPIO 8, 7, 25 (pins 24, 26, 22),
with the common legs to GND. The Pi's pins give only 3.3 V, so use 220 Ω for red and 100 Ω for
green and blue there. The radio uses these pins only when
nothing answers at 0x22; `events.log` says which it found.

## Wiring the LEDs

The LEDs are **common cathode**. An RGB LED has 4 legs; the **longest** is the common one, and
goes to **GND**. Each colour leg goes through its resistor to its module pin. Looking at the flat
side of the rim, with the legs down, the usual order is **Red, common, Green, Blue**. Check yours,
because some makes differ: with a 3 V coin cell and a resistor, **−** on the long leg and **+** on
a short leg lights that colour.

```
         GPA0           GPA1           GPA2       ← MCP23017 output pins (DMR shown)
           │              │              │
          330Ω           330Ω           470Ω
           │              │              │
        ┌──┴──┐        ┌──┴──┐        ┌──┴──┐     one LED shown; repeat for each
        │  B  │        │  G  │        │  R  │     (inside one RGB LED: three diodes
        └──┬──┘        └──┬──┘        └──┬──┘      sharing the cathode)
           │              │              │
           ├──────────────┴──────────────┘
           │ common (long) leg
          GND
```

The module **drives an output high to light the LED**. The current goes from the module pin,
through the resistor and LED, to GND.

- **Resistors:** the modules run on 5 V, so a lit output gives about 5 V. Red needs about 2 V, so
  470 Ω gives about 6 mA. Green and blue need about 3 V, so 330 Ω gives about 6 mA. If one colour
  is too dim or too bright beside the others, change its resistor (but not below 220 Ω).
- **Total:** the LEDs never show more than two colours at once (yellow is red + green), so a
  module supplies at most about 65 mA, within the MCP23017's limits (25 mA a pin, 125 mA in all).
- **Off state:** off is a low output, so nothing glows faintly.

## The 20×4 display

### With an I2C backpack (PCF8574): the usual kind

The backpack is a small board on the back with 4 pins: GND, VCC, SDA and SCL. It runs from
**5 V**, and most backpacks pull SDA and SCL up to 5 V, which is too much for the Pi's pins. So
connect it through the level shifter, on the same 5 V bus as the LED modules:

```
 Pi pin 1 (3.3 V) ── LV        HV ── Pi pin 2 (5 V) ── backpack VCC, both MCP23017 VCC
 Pi pin 6 (GND)  ─── GND      GND ─────────────────── backpack GND, both MCP23017 GND
 Pi pin 3 (SDA)  ─── LV1      HV1 ─────────────────── backpack SDA, both MCP23017 SDA
 Pi pin 5 (SCL)  ─── LV2      HV2 ─────────────────── backpack SCL, both MCP23017 SCL
                  (level shifter)
```

- **Address:** usually **0x27** (PCF8574), or **0x3F** (PCF8574A). The test below shows which.
- **Contrast:** the blue trimmer on the backpack sets the contrast. If the screen shows only
  blocks or nothing, turn it slowly while powered.

### Without a backpack (a bare 16-pin display)

Either solder on a PCF8574 backpack (cheap, and then wire it as above), or say so and I'll work
out a direct connection. A bare display needs 6–7 GPIO pins in 4-bit mode.

## Rotary encoders (KY-040)

| KY-040 pin | **Channel** encoder | **Volume** encoder |
|---|---|---|
| CLK | Pin 18 (GPIO 24) | Pin 38 (GPIO 20) |
| DT | Pin 13 (GPIO 27) | Pin 40 (GPIO 21) |
| SW | Pin 15 (GPIO 22) | Pin 12 (GPIO 18) |
| + | Pin 1 (**3.3 V, not 5 V**) | Pin 1 (**3.3 V, not 5 V**) |
| GND | Pin 6 (GND) | Pin 6, or any GND (30, 34, 39) |

The Pi's GPIO pins are 3.3 V only, so power the encoders from 3.3 V. The KY-040 has its own
pull-up resistors on CLK and DT; the push switch is pulled up by the Pi. If an encoder counts
the wrong way, swap its CLK and DT wires.

## PTT

- **Button:** GPIO 23 (pin 16) to GND. It's hold-to-talk with no delay, unlike the headset's
  Teams button.
- **Optional jack:** wire a 3.5 mm jack's tip to GPIO 23 and its sleeve to GND, in parallel with
  the button, for a fist-mic or foot-switch PTT.

## What the controls do

| Control | Action |
|---|---|
| Mode button, tap | Switch RX on or off for that mode |
| Mode button, hold (0.7 s) | Make it the TX mode |
| Mode LED | **Green:** listening. **Red:** TX mode, not listening. **Yellow** (red + green): TX mode and listening. **Blue:** a call on air in that mode. **Off:** RX off, or the mode isn't enabled |
| **Channel** encoder, turn | Step through the TX mode's destinations, starting at the current one: reflectors for D-Star, YSF and M17, talkgroups for P25 and NXDN, and for DMR the talkgroups listed in `settings.json` (`transmit.dmrTalkgroups`). Spin it quickly to move faster through long lists |
| **Channel** encoder, push | Go to the one shown: link to the reflector or talkgroup, or for DMR transmit to that talkgroup. Pushed when not browsing, it shows where the TX mode transmits |
| **Channel** encoder, push and turn | D-Star and M17: change the module letter (e.g. REF001 C → E for the echo test) |
| **Volume** encoder, turn | Volume, 5% a step |
| **Volume** encoder, push | Mute / unmute |
| Menu, tap | Back: stop browsing |
| Menu, hold 3 s | **Shut down safely.** The display counts down (let go to cancel), then shows "Shutting down". The radio unlinks from the networks and the Pi powers off. When the display shows **"Safe to unplug"**, pull the power |
| PTT | Hold to talk |
| Status LED | **Red:** transmitting. **Green:** receiving a call. **Blue:** a network problem, or a page not yet seen (any control clears it) |

The DMR list starts as `9 Local`, `91 Worldwide`, `92 Europe`, `235 UK`, `2350 UK Call` and
`9990 Parrot` (called privately). Edit it in `settings.json`, as `"<id> <name>"` entries. When
TX follows RX, a talkgroup just heard that isn't in the list is shown first.

## Testing the wiring

Do these over SSH (`ssh root@dtrx`, or the radio's IP address) once it's wired. Stop the radio
first (`/etc/init.d/S90dtrx stop`), because it holds the GPIO lines and sets the LEDs, and start
it again afterwards (`/etc/init.d/S90dtrx start`).

1. **All I2C devices are on the bus:** `i2cdetect -y 1` should show **21** and **22** (the LED
   modules) and **27** or **3f** (the display). It also shows **20** if the Adafruit plate is
   fitted.
2. **One LED lights:** these commands make port A outputs and drive GPA0 high, which should light
   DMR blue:
   ```
   i2cset -y 1 0x21 0x00 0x00    # IODIRA: all outputs
   i2cset -y 1 0x21 0x14 0x01    # OLATA: GPA0 high
   ```
   `0x02` lights GPA1 (DMR green), `0x04` GPA2 (DMR red), and so on; `0x00` turns them all off.
   Registers 0x01 / 0x15 are the GPB equivalents, and use address 0x22 for module B.
3. **Buttons and encoders:** `gpiomon -B pull-up gpiochip0 5 6 12 13 19 16 26 23 22 18` prints
   an event for every press and release (a press is a falling edge). For the encoders,
   `gpiomon -B pull-up gpiochip0 17 27 20 21` shows CLK and DT changing as you turn them.
4. **Everything together:** start the radio. Each button and knob should change the display and
   the LEDs as in [What the controls do](#what-the-controls-do). If a knob counts the wrong way,
   swap its CLK and DT wires.
