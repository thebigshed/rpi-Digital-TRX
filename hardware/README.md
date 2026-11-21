# Digital TRX hardware (KiCad 9)

Two boards joined by a 20-way IDC ribbon cable (pin 1 to pin 1):

| Project | What's on it |
|---|---|
| `dtrx-main/` | Raspberry Pi Zero 2 W (face down on a 2x20 socket, ports at the left edge), the DV3000 AMBE-3000 board on a 2x13 header, 5 V input with a PTC fuse, and the ribbon header |
| `dtrx-panel/` | The front panel: 20x4 LCD in the middle, three mode buttons with RGB LEDs down each side, volume encoder and Menu bottom left, PTT and channel encoder bottom right, the status LED under the display. J2 for the LCD's I2C backpack. On the back: both MCP23017 LED drivers, the I2C level shifter and the pull-ups |

Open `dtrx-main/dtrx-main.kicad_pro` or `dtrx-panel/dtrx-panel.kicad_pro` in KiCad. Each has a
schematic, a routed 2-layer PCB with a ground pour on both sides, and a PDF of the schematic.

Both boards were autorouted with Freerouting by `route.py`, ground included, so no pin depends
on the pour. DRC shows every connection made and the PCB matching its schematic. What's left are
warnings: overlapping silkscreen text, and a few ground pins whose thermal relief has only one
spoke to the pour (they're joined by tracks as well). Tidy the silkscreen before ordering.

The design matches the app as it is. Buttons and encoders go to the same Pi GPIO pins, the LED
drivers sit at 0x21 and 0x22, and the LCD keeps its own I2C backpack (0x27 or 0x3F). So no
software changes are needed apart from building the image for the Pi Zero 2 W.

## Regenerating

`gen_boards.py` writes both projects from scratch: `python3 hardware/gen_boards.py` (it needs KiCad's
`pcbnew` Python module). Re-running it **overwrites** any routing or hand changes, so make design
changes in the script first, then route again with `python3 hardware/route.py` (needs Java and the
[Freerouting](https://github.com/freerouting/freerouting/releases) jar; see the top of the script). Part values, pin assignments, positions and the LCD
dimensions are all near the top of the script.

## Ribbon (J3 on the main board, J1 on the panel)

| Pin | Signal | Pin | Signal |
|---|---|---|---|
| 1 | +5 V | 2 | GND |
| 3 | +3.3 V | 4 | SDA (3.3 V) |
| 5 | SCL (3.3 V) | 6 | GND |
| 7 | PTT (GPIO 23) | 8 | Menu (GPIO 26) |
| 9 | Channel A (GPIO 17) | 10 | Channel B (GPIO 27) |
| 11 | Channel push / Link (GPIO 22) | 12 | Volume A (GPIO 20) |
| 13 | Volume B (GPIO 21) | 14 | Volume push / Mute (GPIO 18) |
| 15 | DMR (GPIO 5) | 16 | D-Star (GPIO 6) |
| 17 | YSF (GPIO 12) | 18 | P25 (GPIO 13) |
| 19 | NXDN (GPIO 19) | 20 | M17 (GPIO 16) |

The I2C bus crosses the ribbon at 3.3 V, and a two-FET level shifter on the panel lifts it to 5 V
for the MCP23017s and the LCD backpack. So the LEDs and the LCD run from 5 V, as on the hand-wired
panel. The MCP23017 address pins are fixed by copper (no jumpers to bridge): U1 is 0x21, U2 is 0x22.

## Check before ordering boards

- **LCD:** the 2004A keeps its I2C backpack and is only bolted to the panel, so the footprint
  `lib/dtrx.pretty/LCD_2004A_98x60` is just the outline and four M3 holes (93 x 55 mm apart on the
  common 98 x 60 mm module). Measure yours; the numbers are `LCD_W`, `LCD_H` and `LCD_HOLES` in
  the script. Use standoffs tall enough for the backpack to clear the panel (about 15 mm), and a
  short 4-way female-female lead from the backpack's GND, VCC, SDA and SCL to J2 (marked LCD, just
  left of the display, same pin order). Set the contrast with the backpack's own trimmer.
- **DV3000:** J2 on the main board copies the Pi's pins 1-26, with pin 1 and the odd row in the
  same places as on a Pi. The board's outline and mounting holes aren't in the design, because I
  don't have its dimensions. Check it fits between the Pi and the ribbon header, and add standoff
  holes.
- **RGB LEDs:** footprint and symbol are common cathode, legs in the order red, cathode
  (longest), green, blue. Check against your LEDs.
- **Encoders:** footprint is the Alps EC11E with push switch, the same part as on the KY-040
  modules. The panel has its own 10k pull-ups, so bare encoders work.
- **Buttons:** 12 mm tactile switches (4 pins). Change the footprint in the script for other
  switches.
- **Pi Zero 2 W:** solder a normal male 2x20 header to the Pi and mount it face down onto a female
  2x20 socket on the main board. Its micro-USB OTG port takes the USB headset, through an OTG
  adapter. Power comes in at J1 (5 V, 2.5 A supply recommended), not the Pi's power USB socket.

## Software for the Pi Zero 2 W

Build the Zero 2 W image with `BOARD=zero2w ./build.sh` (see the main README). The app needs no
changes: it finds the GPIO chip by its `pinctrl-bcm2835` label, which the Zero 2 W uses.
