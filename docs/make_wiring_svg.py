#!/usr/bin/env python3
"""Draws docs/wiring.svg: the rpi-Digital-TRX front panel, with net labels."""
import sys

W, H = 1700, 1240
out = []
def add(s): out.append(s)

C = {  # net colours
    '3V3': '#d93025', '5V': '#b00020', 'GND': '#202124', 'SDA': '#1a73e8', 'SCL': '#0b57d0',
    'SDA5': '#00838f', 'SCL5': '#006064',
    'ENC': '#8430ce', 'BTN': '#188038', 'PTT': '#e8710a', 'DV': '#9aa0a6', 'LED': '#5f6368',
}

def text(x, y, s, size=13, anchor='start', weight='normal', fill='#202124', italic=False):
    st = ' font-style="italic"' if italic else ''
    s = s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
    add(f'<text x="{x}" y="{y}" font-size="{size}" text-anchor="{anchor}" font-weight="{weight}" fill="{fill}"{st}>{s}</text>')

def rect(x, y, w, h, fill='#fff', stroke='#202124', rx=6, sw=1.5):
    add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"/>')

def line(x1, y1, x2, y2, col='#202124', sw=2):
    add(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{col}" stroke-width="{sw}" stroke-linecap="round"/>')

def net(x, y, name, col, anchor='start'):
    """A net label flag: wires with the same label are connected."""
    w = 8 * len(name) + 16
    x0 = x if anchor == 'start' else x - w
    add(f'<rect x="{x0}" y="{y-10}" width="{w}" height="20" rx="10" fill="{col}"/>')
    text(x0 + w / 2, y + 4.5, name, 12, 'middle', 'bold', '#fff')

def resistor(x, y, horiz=True, value='470Ω'):
    if horiz:
        rect(x, y - 6, 34, 12, '#fff', '#202124', 2, 1.5)
        text(x + 17, y - 9, value, 10, 'middle')
    else:
        rect(x - 6, y, 12, 34, '#fff', '#202124', 2, 1.5)

def button(x, y, label, gpio, col):
    """GPIO net — button — GND."""
    net(x, y, gpio, col, 'end')
    line(x, y, x + 30, y, col)
    # switch symbol
    add(f'<circle cx="{x+33}" cy="{y}" r="3" fill="#fff" stroke="#202124" stroke-width="1.5"/>')
    add(f'<circle cx="{x+73}" cy="{y}" r="3" fill="#fff" stroke="#202124" stroke-width="1.5"/>')
    line(x + 33, y - 9, x + 73, y - 9, '#202124', 2)
    line(x + 53, y - 9, x + 53, y - 17, '#202124', 2)
    line(x + 46, y - 17, x + 60, y - 17, '#202124', 2)
    line(x + 76, y, x + 104, y, C['GND'])
    net(x + 104, y, 'GND', C['GND'])
    text(x + 53, y + 20, label, 12, 'middle', 'bold')

# ------------------------------------------------------------------ page
add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
    'font-family="DejaVu Sans, Arial, sans-serif">')
rect(0, 0, W, H, '#ffffff', '#ffffff', 0)
text(30, 42, 'rpi-Digital-TRX front panel — wiring', 26, weight='bold')
text(30, 66, 'Wires with the same coloured label are connected. Pin numbers are physical header pins; '
     'GPIO numbers are BCM. See wiring.md for details and tests.', 14, fill='#5f6368')

# ------------------------------------------------------------------ Pi header
hx, hy = 40, 110
rect(hx - 15, hy - 30, 400, 20 * 26 + 60, '#f8f9fa', '#5f6368', 10)
text(hx + 185, hy - 8, 'Raspberry Pi 4 — 40-pin header', 15, 'middle', 'bold')
pins = {
    1: ('3.3 V', '3V3'), 2: ('5 V', '5V'), 3: ('GPIO 2 SDA', 'SDA'), 4: ('5 V', '5V'), 5: ('GPIO 3 SCL', 'SCL'),
    6: ('GND', 'GND'), 7: ('GPIO 4 — DV3000 reset', 'DV'), 8: ('GPIO 14 — DV3000 TX', 'DV'), 9: ('GND', 'GND'),
    10: ('GPIO 15 — DV3000 RX', 'DV'), 11: ('GPIO 17 — DV3000 uses', 'DV'), 12: ('GPIO 18 VOL-SW', 'ENC'),
    13: ('GPIO 27 CH-DT', 'ENC'), 14: ('GND', 'GND'), 15: ('GPIO 22 CH-SW', 'ENC'), 16: ('GPIO 23 PTT', 'PTT'),
    17: ('3.3 V', '3V3'), 18: ('GPIO 24 CH-CLK', 'ENC'), 19: ('GPIO 10', None), 20: ('GND', 'GND'),
    21: ('GPIO 9', None), 22: ('GPIO 25 spare', None), 23: ('GPIO 11', None), 24: ('GPIO 8', None),
    25: ('GND', 'GND'), 26: ('GPIO 7', None), 27: ('ID_SD', None), 28: ('ID_SC', None),
    29: ('GPIO 5 DMR', 'BTN'), 30: ('GND', 'GND'), 31: ('GPIO 6 D-STAR', 'BTN'), 32: ('GPIO 12 YSF', 'BTN'),
    33: ('GPIO 13 P25', 'BTN'), 34: ('GND', 'GND'), 35: ('GPIO 19 NXDN', 'BTN'), 36: ('GPIO 16 M17', 'BTN'),
    37: ('GPIO 26 MENU', 'BTN'), 38: ('GPIO 20 VOL-CLK', 'ENC'), 39: ('GND', 'GND'), 40: ('GPIO 21 VOL-DT', 'ENC'),
}
for n in range(1, 41):
    row, odd = (n - 1) // 2, n % 2 == 1
    y = hy + 18 + row * 26
    cx = hx + 170 if odd else hx + 200
    label, kind = pins[n]
    col = C.get(kind, '#dadce0') if kind else '#dadce0'
    add(f'<circle cx="{cx}" cy="{y}" r="9" fill="{col}" stroke="#202124" stroke-width="1"/>')
    text(cx, y + 4, str(n), 9, 'middle', 'bold', '#fff' if kind else '#5f6368')
    if odd:
        text(cx - 16, y + 4, label, 12, 'end', fill='#202124' if kind else '#9aa0a6')
    else:
        text(cx + 16, y + 4, label, 12, 'start', fill='#202124' if kind else '#9aa0a6')
add(f'<rect x="{hx+155}" y="{hy+5}" width="60" height="{13*26}" rx="6" fill="none" stroke="#9aa0a6" stroke-dasharray="5 4" stroke-width="1.5"/>')
text(hx - 5, hy + 20 * 26 + 126, 'Dashed box: pins 1–26 are under the DV3000 — reach them', 11, fill='#5f6368', italic=True)
text(hx - 5, hy + 20 * 26 + 140, 'through a 40-pin GPIO splitter or a tall stacking header.', 11, fill='#5f6368', italic=True)
# legend
ly = hy + 20 * 26 + 50
for i, (k, name) in enumerate([('3V3', '3.3 V'), ('5V', '5 V'), ('GND', 'GND'), ('SDA', 'I2C 3.3 V'), ('SDA5', 'I2C 5 V'),
                                ('ENC', 'Encoders'), ('BTN', 'Buttons'), ('PTT', 'PTT'), ('DV', 'DV3000 — leave alone')]):
    x = hx - 5 + (i % 4) * 95
    y = ly + (i // 4) * 22
    add(f'<circle cx="{x+6}" cy="{y}" r="6" fill="{C[k]}"/>')
    text(x + 16, y + 4, name, 11)

# ------------------------------------------------------------------ display + level shifter
dx, dy = 470, 100
rect(dx, dy, 300, 150, '#f8f9fa', '#5f6368', 10)
text(dx + 150, dy + 22, 'I2C level shifter (BSS138)', 14, 'middle', 'bold')
ls = [('LV', '3V3', 'HV', '5V'), ('GND', 'GND', 'GND', 'GND'), ('LV1', 'SDA', 'HV1', 'D-SDA'), ('LV2', 'SCL', 'HV2', 'D-SCL')]
lcd_pins = []
for i, (l, ln, r, rn) in enumerate(ls):
    y = dy + 50 + i * 26
    text(dx + 14, y + 4, l, 12, weight='bold')
    text(dx + 286, y + 4, r, 12, 'end', 'bold')
    line(dx - 10, y, dx, y, C.get(ln, C['SDA']))
    net(dx - 12, y, {'3V3': '3.3V'}.get(ln, ln), C.get(ln, C['SDA']), 'end')
    lcd_pins.append((y, rn))


lx, ly2 = 900, 100
rect(lx, ly2, 360, 150, '#e8f0fe', '#1a73e8', 10)
text(lx + 180, ly2 + 20, '20×4 LCD + PCF8574 backpack', 14, 'middle', 'bold')
text(lx + 180, ly2 + 37, 'I2C address 0x27 (or 0x3F)', 12, 'middle', fill='#5f6368')
for (y, rn), p in zip(lcd_pins, ['VCC', 'GND', 'SDA', 'SCL']):
    col = {'5V': C['5V'], 'GND': C['GND']}.get(rn, '#4285f4')
    text(lx + 14, y + 4, p, 12, weight='bold')
    line(dx + 300, y, lx, y, col)                    # straight across: HV pins -> display pins
    if rn == '5V':
        net(dx + 335, y, '5V', col)                  # this wire is also the 5 V supply
    elif rn in ('D-SDA', 'D-SCL'):                   # the 5 V I2C bus: display and both LED modules
        k = 'SDA5' if rn == 'D-SDA' else 'SCL5'
        line(dx + 300, y, lx, y, C[k])
        net(dx + 335, y, 'SDA 5V' if k == 'SDA5' else 'SCL 5V', C[k])
add(f'<rect x="{lx+70}" y="{ly2+48}" width="270" height="90" rx="4" fill="#2d5a27" stroke="#1e3d1a"/>')
for i, s in enumerate(['DSTR REF001 E  12:41', 'M6OZE  Ozzy', '>CQCQCQ        0:07', 'RX  192.168.101.75']):
    add(f'<text x="{lx+82}" y="{ly2+70+i*19}" font-size="14" font-family="DejaVu Sans Mono, monospace" fill="#c8f7c5">{s}</text>')

# ------------------------------------------------------------------ MCP23017 modules + LEDs
def module(x, y, title, addr, a0, a1, rows):
    """rows: list of (pin, led name, colour letter) or None"""
    h = max(64 + 8 * 22, 64 + 26 * len(rows)) + 10
    rect(x, y, 230, h, '#fef7e0', '#e37400', 10)
    text(x + 115, y + 22, title, 14, 'middle', 'bold')
    text(x + 115, y + 40, f'MCP23017 — I2C {addr}, on 5 V', 12, 'middle', fill='#5f6368')
    # power / bus pins on the left
    for i, (p, n) in enumerate([('VCC', '5V'), ('GND', 'GND'), ('SDA', 'SDA 5V'), ('SCL', 'SCL 5V'),
                                ('A0', a0), ('A1', a1), ('A2', 'GND'), ('RST', '5V')]):
        yy = y + 64 + i * 22
        k = {'5V': '5V', 'GND': 'GND', 'SDA 5V': 'SDA5', 'SCL 5V': 'SCL5'}[n]
        text(x + 12, yy + 4, p, 12, weight='bold')
        line(x - 10, yy, x, yy, C[k])
        net(x - 12, yy, n, C[k], 'end')
    # LED outputs on the right
    for i, r in enumerate(rows):
        yy = y + 64 + i * 26
        if r is None:
            continue
        pin, led, colr = r
        text(x + 218, yy + 4, pin, 12, 'end', 'bold')
        if led is None:
            text(x + 250, yy + 4, 'spare', 11, fill='#9aa0a6', italic=True)
            continue
        cc = {'R': '#d93025', 'G': '#188038', 'B': '#1a73e8'}[colr]
        line(x + 230, yy, x + 262, yy, cc)
        resistor(x + 262, yy, value='470Ω' if colr == 'R' else '330Ω')
        line(x + 296, yy, x + 330, yy, cc)
    return h

def led_group(x, y, name, rows_y, colours='BGR'):
    """An RGB LED (common cathode) whose three anode legs (colours, in order) arrive at rows_y; cathode to GND."""
    top, bot = min(rows_y) - 16, max(rows_y) + 16
    rect(x, top, 70, bot - top, '#ffffff', '#5f6368', 8)
    for yy, colr in zip(rows_y, colours):
        cc = {'R': '#d93025', 'G': '#188038', 'B': '#1a73e8'}[colr]
        # diode pointing right (current flows from the module pin through the LED to GND)
        add(f'<polygon points="{x+16},{yy-7} {x+16},{yy+7} {x+30},{yy}" fill="{cc}"/>')
        line(x + 32, yy - 7, x + 32, yy + 7, cc, 2)
        line(x, yy, x + 16, yy, cc)
        line(x + 32, yy, x + 52, yy, '#5f6368', 1.5)
    line(x + 52, rows_y[0], x + 52, rows_y[-1], '#5f6368', 1.5)
    mid = (rows_y[0] + rows_y[-1]) / 2
    line(x + 52, mid, x + 84, mid, C['GND'])
    net(x + 86, mid, 'GND', C['GND'])
    text(x + 136, mid + 5, name, 13, 'start', 'bold')

ax, ay = 520, 330
rowsA = [('GPA0', 'DMR', 'B'), ('GPA1', 'DMR', 'G'), ('GPA2', 'DMR', 'R'),
         ('GPA3', 'D-Star', 'B'), ('GPA4', 'D-Star', 'G'), ('GPA5', 'D-Star', 'R'),
         ('GPA6', 'YSF', 'B'), ('GPA7', 'YSF', 'G'), ('GPB0', 'YSF', 'R'),
         ('GPB1', 'P25', 'R'), ('GPB2', 'P25', 'B'), ('GPB3', 'P25', 'G'),
         ('GPB4', 'NXDN', 'R'), ('GPB5', 'NXDN', 'B'), ('GPB6', 'NXDN', 'G'), ('GPB7', None, None)]
module(ax, ay, 'Module A', '0x21', '5V', 'GND', rowsA)
for g, name in enumerate(['DMR', 'D-Star', 'YSF', 'P25', 'NXDN']):
    ys = [ay + 64 + (g * 3 + k) * 26 for k in range(3)]
    led_group(ax + 330, ys, name, ys, ''.join(r[2] for r in rowsA[g * 3:g * 3 + 3]))

bx, by = 1130, 330
rowsB = [('GPA0', 'M17', 'B'), ('GPA1', 'M17', 'G'), ('GPA2', 'M17', 'R'),
         ('GPA3', 'Status', 'B'), ('GPA4', 'Status', 'G'), ('GPA5', 'Status', 'R'),
         ('GPA6', None, None), ('GPA7', None, None), ('GPB0–7', None, None)]
module(bx, by, 'Module B', '0x22', 'GND', '5V', rowsB)
for name, first in (('M17', 0), ('Status', 3)):
    ys = [by + 64 + (first + k) * 26 for k in range(3)]
    led_group(bx + 330, ys, name, ys)
text(bx + 250, by + 330, 'Status LED: red = TX, green = RX,', 12, fill='#5f6368')
text(bx + 250, by + 346, 'blue = network fault / new page', 12, fill='#5f6368')
text(bx + 250, by + 372, 'Mode LEDs: green = RX on, red = TX mode,', 12, fill='#5f6368')
text(bx + 250, by + 388, 'yellow = both, blue = call on air', 12, fill='#5f6368')
text(bx + 250, by + 414, 'Common-cathode LEDs (long leg to GND).', 12, fill='#5f6368', italic=True)
text(bx + 250, by + 430, 'Resistors: see wiring.md.', 12, fill='#5f6368', italic=True)

# ------------------------------------------------------------------ encoders
ey = 960
def encoder(x, y, title, nets):
    rect(x, y, 200, 160, '#f3e8fd', '#8430ce', 10)
    text(x + 100, y + 22, title, 14, 'middle', 'bold')
    add(f'<circle cx="{x+150}" cy="{y+95}" r="30" fill="#e0c7f7" stroke="#8430ce" stroke-width="2"/>')
    add(f'<circle cx="{x+150}" cy="{y+95}" r="12" fill="#8430ce"/>')
    for i, (p, n, k) in enumerate(nets):
        yy = y + 44 + i * 24
        text(x + 12, yy + 4, p, 12, weight='bold')
        line(x - 10, yy, x, yy, C[k])
        net(x - 12, yy, n, C[k], 'end')

encoder(530, ey, 'KY-040: CHANNEL', [('CLK', 'GPIO24', 'ENC'), ('DT', 'GPIO27', 'ENC'), ('SW', 'GPIO22', 'ENC'),
                                     ('+', '3.3V', '3V3'), ('GND', 'GND', 'GND')])
encoder(850, ey, 'KY-040: VOLUME', [('CLK', 'GPIO20', 'ENC'), ('DT', 'GPIO21', 'ENC'), ('SW', 'GPIO18', 'ENC'),
                                    ('+', '3.3V', '3V3'), ('GND', 'GND', 'GND')])
text(630, ey + 182, 'turn = reflector / talkgroup, push = link', 12, 'middle', fill='#5f6368')
text(950, ey + 182, 'turn = volume, push = mute', 12, 'middle', fill='#5f6368')

# ------------------------------------------------------------------ buttons
bx0, by0 = 1200, 930
rect(bx0 - 110, by0 - 40, 480, 300, '#e6f4ea', '#188038', 10)
text(bx0 + 130, by0 - 16, 'Push buttons (to GND, no resistors)', 14, 'middle', 'bold')

for i, (lab, g) in enumerate([('DMR', 'GPIO5'), ('D-Star', 'GPIO6'), ('YSF', 'GPIO12'), ('P25', 'GPIO13'),
                              ('NXDN', 'GPIO19'), ('M17', 'GPIO16'), ('Menu', 'GPIO26')]):
    col, row = i % 2, i // 2
    button(bx0 - 20 + col * 240, by0 + 20 + row * 56, lab, g, C['BTN'])
button(bx0 - 20 + 240, by0 + 20 + 3 * 56, 'PTT', 'GPIO23', C['PTT'])
text(bx0 + 130, by0 + 250, 'Optional PTT jack: tip → GPIO23, sleeve → GND', 12, 'middle', fill='#5f6368')

add('</svg>')
open(sys.argv[1], 'w').write('\n'.join(out))
