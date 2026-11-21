#!/usr/bin/env python3
"""Generate the two KiCad 9 projects for the Digital TRX hardware.

  dtrx-main   Raspberry Pi Zero 2 W (face down on a socket) and the DV3000 AMBE-3000 board,
              5 V input, and the ribbon to the panel.
  dtrx-panel  The front panel: 20x4 LCD in the middle (2004A with its I2C backpack), the
              six mode buttons with their RGB LEDs split left and right, volume encoder and
              Menu bottom left, PTT and channel encoder bottom right, the status LED, both
              MCP23017 LED drivers (0x21, 0x22) and the I2C level shifter.

The two boards are joined by a 20-way IDC ribbon (pin 1 to pin 1). Everything matches the
app as it stands (app/src/GpioPanel.cpp, LedPanel.cpp, Lcd2004.cpp), so no software changes
are needed apart from building the image for the Pi Zero 2 W.

Run with the system python3 (it needs KiCad's pcbnew module):   python3 hardware/gen_boards.py
Re-running rewrites both projects. Routing done in KiCad is lost, so route once the design
is settled.
"""
import json
import os
import re
import uuid

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
KI = '/usr/share/kicad'
NS = uuid.UUID('6c3f0b9e-2d0a-4b1e-9a57-0d7a2f1c9e11')

# 20x4 HD44780 module (the common "2004A", 98 x 60 mm) with a PCF8574 I2C backpack soldered
# to its back. Check these against your display.
LCD_W, LCD_H = 98.0, 60.0
LCD_HOLES = [(2.5, 2.5), (95.5, 2.5), (2.5, 57.5), (95.5, 57.5)]   # M3 mounting holes
LCD_PIN1 = (10.0, 2.5)                                           # pin 1 of the 16-pin row
BACKPACK = (6.0, 0.0, 48.0, 20.0)   # area the backpack covers (x0, y0, x1, y1), seen from the front

# The ribbon between the boards: IDC pin -> net (same on both boards)
IDC = {1: '+5V', 2: 'GND', 3: '+3V3', 4: 'SDA', 5: 'SCL', 6: 'GND',
       7: 'BTN_PTT', 8: 'BTN_MENU', 9: 'ENC_CH_A', 10: 'ENC_CH_B', 11: 'BTN_LINK',
       12: 'ENC_VOL_A', 13: 'ENC_VOL_B', 14: 'BTN_MUTE', 15: 'BTN_DMR', 16: 'BTN_DSTAR',
       17: 'BTN_YSF', 18: 'BTN_P25', 19: 'BTN_NXDN', 20: 'BTN_M17'}


def uid(*parts):
    return str(uuid.uuid5(NS, '/'.join(str(p) for p in parts)))


# ------------------------------------------------------------------ s-expressions
class Q(str):
    """A quoted string in an s-expression."""


_TOK = re.compile(r'\(|\)|"(?:\\.|[^"\\])*"|[^\s()"]+')


def parse(text):
    stack = [[]]
    for t in _TOK.findall(text):
        if t == '(':
            stack.append([])
        elif t == ')':
            n = stack.pop()
            stack[-1].append(n)
        elif t.startswith('"'):
            stack[-1].append(Q(t[1:-1].replace('\\"', '"').replace('\\\\', '\\')))
        else:
            stack[-1].append(t)
    return stack[0][0]


def _atom(a):
    if isinstance(a, Q):
        return '"' + a.replace('\\', '\\\\').replace('"', '\\"') + '"'
    if isinstance(a, float):
        s = f'{a:.4f}'.rstrip('0').rstrip('.')
        return '0' if s in ('-0', '') else s
    return str(a)


def dump(n, ind=0):
    if not isinstance(n, list):
        return _atom(n)
    head = [x for x in n if not isinstance(x, list)]
    kids = [x for x in n if isinstance(x, list)]
    s = '(' + ' '.join(_atom(x) for x in head)
    if not kids:
        return s + ')'
    # keep order: atoms may follow lists in a few places; KiCad doesn't mind order of atoms
    out = '(' + ' '.join(_atom(x) for x in n if not isinstance(x, list))
    for x in n:
        if isinstance(x, list):
            out += '\n' + '\t' * (ind + 1) + dump(x, ind + 1)
    return out + '\n' + '\t' * ind + ')'


def find(n, key):
    for x in n:
        if isinstance(x, list) and x and x[0] == key:
            return x
    return None


# ------------------------------------------------------------------ symbol libraries
_libtext = {}
_custom = {}


def _block(lib, name):
    if (lib, name) in _custom:
        return parse(dump(_custom[(lib, name)]))
    if lib not in _libtext:
        _libtext[lib] = open(f'{KI}/symbols/{lib}.kicad_sym').read()
    t = _libtext[lib]
    i = t.find(f'\n\t(symbol "{name}"\n')
    if i < 0:
        raise KeyError(f'{lib}:{name}')
    i += 2
    depth = 0
    j = i
    in_str = False
    while True:
        c = t[j]
        if in_str:
            if c == '\\':
                j += 1
            elif c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                break
        j += 1
    return parse(t[i:j + 1])


def flat_symbol(lib, name):
    """The symbol with any 'extends' resolved, named plainly (no library prefix)."""
    node = _block(lib, name)
    ext = find(node, 'extends')
    if not ext:
        return node
    parent = flat_symbol(lib, ext[1])
    props = [x for x in node if isinstance(x, list) and x[0] == 'property']
    out = ['symbol', Q(name)]
    inserted = False
    for x in parent[2:]:
        if isinstance(x, list) and x[0] == 'property':
            if not inserted:
                out += props
                inserted = True
            continue
        if isinstance(x, list) and x[0] == 'symbol':
            x = list(x)
            x[1] = Q(str(x[1]).replace(str(ext[1]) + '_', name + '_', 1))
        out.append(x)
    return out


def sym_pins(node):
    """[(number, name, x, y, angle, hidden)] for units 0 and 1."""
    pins = []
    nm = str(node[1]).split(':')[-1]
    for sub in node:
        if not (isinstance(sub, list) and sub[0] == 'symbol'):
            continue
        m = re.match(re.escape(nm) + r'_(\d+)_(\d+)$', str(sub[1]))
        if not m or m.group(1) not in ('0', '1'):
            continue
        for p in sub:
            if isinstance(p, list) and p[0] == 'pin':
                at = find(p, 'at')
                hidden = 'hide' in p or find(p, 'hide') is not None
                pins.append((str(find(p, 'number')[1]), str(find(p, 'name')[1]),
                             float(at[1]), float(at[2]), int(float(at[3])), hidden))
    return pins


def set_prop(node, key, value):
    for x in node:
        if isinstance(x, list) and x[0] == 'property' and x[1] == key:
            x[2] = Q(value)


def make_custom_lib():
    """PCF8574 in DIP-16 (KiCad only has the SSOP-20 version)."""
    dip = {'~{INT}': '13', 'SCL': '14', 'SDA': '15', 'VDD': '16', 'A0': '1', 'A1': '2', 'A2': '3',
           'P0': '4', 'P1': '5', 'P2': '6', 'P3': '7', 'VSS': '8', 'P4': '9', 'P5': '10',
           'P6': '11', 'P7': '12'}
    s = flat_symbol('Interface_Expansion', 'PCF8574TS')
    s = parse(dump(s).replace('PCF8574TS_', 'PCF8574_DIP16_'))
    s[1] = Q('PCF8574_DIP16')
    set_prop(s, 'Value', 'PCF8574')
    set_prop(s, 'Footprint', 'Package_DIP:DIP-16_W7.62mm')
    set_prop(s, 'Description', 'Remote 8-bit I/O expander for I2C, DIP-16 (PCF8574P / PCF8574AN)')
    for sub in s:
        if isinstance(sub, list) and sub[0] == 'symbol':
            sub[:] = [p for p in sub if not (isinstance(p, list) and p[0] == 'pin'
                                             and str(find(p, 'name')[1]) == 'NC')]
            for p in sub:
                if isinstance(p, list) and p[0] == 'pin':
                    find(p, 'number')[1] = Q(dip[str(find(p, 'name')[1])])
    _custom[('dtrx', 'PCF8574_DIP16')] = s
    lib = ['kicad_symbol_lib', ['version', '20241209'], ['generator', Q('kicad_symbol_editor')],
           ['generator_version', Q('9.0')], s]
    with open(os.path.join(HERE, 'lib', 'dtrx.kicad_sym'), 'w') as f:
        f.write(dump(lib) + '\n')


def make_lcd_footprint():
    p = []
    p.append(f'(footprint "LCD_2004A_98x60"\n\t(version 20241229)\n\t(generator "pcbnew")\n'
             f'\t(generator_version "9.0")\n\t(layer "F.Cu")\n'
             f'\t(descr "20x4 character LCD module (2004A, HD44780) with I2C backpack, 98 x 60 mm, '
             f'4 x M3 holes 93 x 55 mm. Mounting only: the backpack is wired to J2. '
             f'Check against your module.")\n'
             f'\t(tags "LCD 2004 HD44780")\n')
    p.append(f'\t(property "Reference" "REF**" (at 0 -2.5 0) (layer "F.SilkS") '
             f'(uuid "{uid("lcdfp", "ref")}") (effects (font (size 1 1) (thickness 0.15))))\n')
    p.append(f'\t(property "Value" "LCD_2004A" (at {LCD_W/2} {LCD_H/2} 0) (layer "F.Fab") '
             f'(uuid "{uid("lcdfp", "val")}") (effects (font (size 1 1) (thickness 0.15))))\n')
    for layer, inset, w in (('F.Fab', 0, 0.1), ('F.SilkS', -0.12, 0.12), ('F.CrtYd', -0.5, 0.05)):
        p.append(f'\t(fp_rect (start {inset} {inset}) (end {LCD_W - inset} {LCD_H - inset}) '
                 f'(stroke (width {w}) (type default)) (fill no) (layer "{layer}") '
                 f'(uuid "{uid("lcdfp", layer)}"))\n')
    p.append(f'\t(fp_rect (start 10 11) (end 88 49) (stroke (width 0.1) (type dash)) (fill no) '
             f'(layer "F.Fab") (uuid "{uid("lcdfp", "view")}"))\n')
    x0, y0, x1, y1 = BACKPACK
    p.append(f'\t(fp_rect (start {x0} {y0}) (end {x1} {y1}) (stroke (width 0.1) (type dash)) '
             f'(fill no) (layer "F.Fab") (uuid "{uid("lcdfp", "backpack")}"))\n')
    p.append(f'\t(fp_text user "I2C backpack" (at {(x0 + x1) / 2} {(y0 + y1) / 2} 0) (layer "F.Fab") '
             f'(uuid "{uid("lcdfp", "bptext")}") (effects (font (size 1 1) (thickness 0.15))))\n')
    for k, (x, y) in enumerate(LCD_HOLES):
        p.append(f'\t(pad "" np_thru_hole circle (at {x} {y}) (size 3.2 3.2) (drill 3.2) '
                 f'(layers "*.Cu" "*.Mask") (uuid "{uid("lcdfp", "hole", k)}"))\n')
    p.append('\t(embedded_fonts no)\n)\n')
    with open(os.path.join(HERE, 'lib', 'dtrx.pretty', 'LCD_2004A_98x60.kicad_mod'), 'w') as f:
        f.write(''.join(p))


# ------------------------------------------------------------------ design description
class Part:
    def __init__(self, ref, sym, value, fp, nets=None, pos=None, rot=0, back=False, silk=None,
                 on_board=True):
        self.ref, self.sym, self.value, self.fp = ref, sym, value, fp
        self.nets = {str(k): v for k, v in (nets or {}).items()}
        self.pos, self.rot, self.back, self.silk, self.on_board = pos, rot, back, silk, on_board


class Board:
    def __init__(self, name, title, size):
        self.name, self.title, self.size = name, title, size
        self.parts = []
        self.notes = []          # (x, y, text, layer)
        self.pack = []           # (region (x0, y0, x1, y1), [refs]) for automatic placement

    def add(self, *a, **k):
        p = Part(*a, **k)
        self.parts.append(p)
        return p


R_FP = 'Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P2.54mm_Vertical'
C_FP = 'Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm'
H_FP = 'MountingHole:MountingHole_3.2mm_M3'


def main_board():
    b = Board('dtrx-main', 'Digital TRX: Pi Zero 2 W and AMBE board', (150, 85))
    pi = {1: '+3V3', 17: '+3V3', 2: '+5V', 4: '+5V', 3: 'SDA', 5: 'SCL', 7: 'AMBE_RESET',
          8: 'UART_TXD', 10: 'UART_RXD', 11: 'ENC_CH_A', 12: 'BTN_MUTE', 13: 'ENC_CH_B',
          15: 'BTN_LINK', 16: 'BTN_PTT', 18: 'GPIO24', 19: 'GPIO10', 21: 'GPIO9', 22: 'GPIO25',
          23: 'GPIO11', 24: 'GPIO8', 26: 'GPIO7', 29: 'BTN_DMR', 31: 'BTN_DSTAR', 32: 'BTN_YSF',
          33: 'BTN_P25', 35: 'BTN_NXDN', 36: 'BTN_M17', 37: 'BTN_MENU', 38: 'ENC_VOL_A',
          40: 'ENC_VOL_B'}
    for g in (6, 9, 14, 20, 25, 30, 34, 39):
        pi[g] = 'GND'
    b.add('U1', 'Connector:Raspberry_Pi_2_3', 'Raspberry Pi Zero 2 W',
          'Module:Raspberry_Pi_Zero_Socketed_THT_FaceDown_MountingHoles', pi, pos=('b', 1.5, 9),
          rot=180)   # turned so its USB and HDMI ports face the left edge of the board
    # The DV3000 plugs on as it would on a Pi: header pins 1-26, pin 1 left, odd row nearer the
    # board it sits over.
    b.add('J2', 'Connector_Generic:Conn_02x13_Odd_Even', 'DV3000 (AMBE-3000R)',
          'Connector_PinHeader_2.54mm:PinHeader_2x13_P2.54mm_Vertical',
          {n: pi[n] for n in range(1, 27) if n in pi}, pos=('o', 62, 12), rot=90)
    b.add('J3', 'Connector_Generic:Conn_02x10_Odd_Even', 'To panel (20-way IDC)',
          'Connector_IDC:IDC-Header_2x10_P2.54mm_Vertical', IDC, pos=('c', 138, 40))
    b.add('J1', 'Connector_Generic:Conn_01x02', '5V in',
          'TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-1,5-2_1x02_P5.00mm_Horizontal',
          {1: 'VIN', 2: 'GND'}, pos=('c', 60, 78))
    b.add('F1', 'Device:Polyfuse', 'PTC 2A hold', 'Fuse:Fuse_Bourns_MF-RG300',
          {1: 'VIN', 2: '+5V'}, pos=('c', 80, 78))
    b.add('C1', 'Device:C_Polarized', '470uF 10V', 'Capacitor_THT:CP_Radial_D8.0mm_P3.50mm',
          {1: '+5V', 2: 'GND'}, pos=('c', 98, 78))
    b.add('C2', 'Device:C', '100nF', C_FP, {1: '+5V', 2: 'GND'}, pos=('c', 112, 78))
    for i, (x, y) in enumerate([(4, 4), (146, 4), (4, 81), (146, 81)]):
        b.add(f'H{i + 1}', 'Mechanical:MountingHole', 'M3', H_FP, pos=('c', x, y))
    b.notes += [(84, 47, 'DV3000 board sits here\n(check its size and holes)', 'F.Fab'),
                (24, 79, 'Pi Zero 2 W face down\n<- USB / HDMI', 'F.SilkS'),
                (138, 24, 'PANEL', 'F.SilkS'),
                (60, 72, '5V IN  + -', 'F.SilkS')]
    return b


def panel_board():
    W, H = 190, 100
    b = Board('dtrx-panel', 'Digital TRX: front panel', (W, H))
    # ribbon from the main board
    b.add('J1', 'Connector_Generic:Conn_02x10_Odd_Even', 'From main board (20-way IDC)',
          'Connector_IDC:IDC-Header_2x10_P2.54mm_Vertical', IDC, pos=('c', 95, 89), rot=90,
          back=True)

    # I2C level shifter: Pi side 3.3 V, panel side 5 V
    b.add('Q1', 'Transistor_FET:BSS138', 'BSS138', 'Package_TO_SOT_SMD:SOT-23',
          {1: '+3V3', 2: 'SDA', 3: 'SDA5'}, back=True)
    b.add('Q2', 'Transistor_FET:BSS138', 'BSS138', 'Package_TO_SOT_SMD:SOT-23',
          {1: '+3V3', 2: 'SCL', 3: 'SCL5'}, back=True)
    b.add('R1', 'Device:R', '4k7', R_FP, {1: 'SDA5', 2: '+5V'}, back=True)
    b.add('R2', 'Device:R', '4k7', R_FP, {1: 'SCL5', 2: '+5V'}, back=True)

    # LED drivers: module A at 0x21, module B at 0x22 (app/src/LedPanel.cpp)
    a_pins = {'GPA0': 'DMR_R', 'GPA1': 'DMR_G', 'GPA2': 'DMR_B', 'GPA3': 'DSTAR_R',
              'GPA4': 'DSTAR_G', 'GPA5': 'DSTAR_B', 'GPA6': 'YSF_R', 'GPA7': 'YSF_G',
              'GPB0': 'YSF_B', 'GPB1': 'P25_R', 'GPB2': 'P25_G', 'GPB3': 'P25_B',
              'GPB4': 'NXDN_R', 'GPB5': 'NXDN_G', 'GPB6': 'NXDN_B'}
    b_pins = {'GPA0': 'M17_R', 'GPA1': 'M17_G', 'GPA2': 'M17_B', 'GPA3': 'STATUS_R',
              'GPA4': 'STATUS_G', 'GPA5': 'STATUS_B'}
    mcp = flat_symbol('Interface_Expansion', 'MCP23017_SP')
    num = {name: n for n, name, *_ in sym_pins(mcp)}

    def mcp_nets(gp, a0, a1):
        n = {num['VDD']: '+5V', num['VSS']: 'GND', num['SDA']: 'SDA5', num['SCK']: 'SCL5',
             num['~{RESET}']: '+5V', num['A0']: a0, num['A1']: a1, num['A2']: 'GND'}
        n.update({num[k]: v for k, v in gp.items()})
        return n
    b.add('U1', 'Interface_Expansion:MCP23017_SP', 'MCP23017 (0x21)', 'Package_DIP:DIP-28_W7.62mm',
          mcp_nets(a_pins, '+5V', 'GND'), back=True, pos=('b', 52, 12))
    b.add('U2', 'Interface_Expansion:MCP23017_SP', 'MCP23017 (0x22)', 'Package_DIP:DIP-28_W7.62mm',
          mcp_nets(b_pins, 'GND', '+5V'), back=True, pos=('b', 64, 12))

    # The 2004A LCD with its I2C backpack (0x27 or 0x3F, app/src/Lcd2004.cpp). The LCD is only
    # bolted on (four M3 standoffs, tall enough to clear the backpack); a short 4-way lead joins
    # the backpack's header to J2, just left of the display.
    b.add('DS1', 'Mechanical:MountingHole', 'LCD 20x4 (2004A) with I2C backpack',
          'dtrx:LCD_2004A_98x60', {}, pos=('o', (W - LCD_W) / 2, 5))
    b.add('J2', 'Connector_Generic:Conn_01x04', 'LCD I2C backpack (GND VCC SDA SCL)',
          'Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical',
          {1: 'GND', 2: '+5V', 3: 'SDA5', 4: 'SCL5'}, pos=('c', 40, 12), silk='LCD')

    # RGB LEDs (common cathode, R K G B) and their resistors
    leds = [('D1', 'DMR', 'L', 0), ('D2', 'DSTAR', 'L', 1), ('D3', 'YSF', 'L', 2),
            ('D4', 'P25', 'R', 0), ('D5', 'NXDN', 'R', 1), ('D6', 'M17', 'R', 2),
            ('D7', 'STATUS', None, None)]
    rows = [22, 42, 62]
    rn = 10
    for ref, m, side, row in leds:
        if side == 'L':
            lpos = ('c', 9, rows[row])
        elif side == 'R':
            lpos = ('c', W - 9, rows[row])
        else:
            lpos = ('c', W / 2, 74)
        b.add(ref, 'Device:LED_RKGB', f'RGB LED {m}', 'LED_THT:LED_D5.0mm-4_RGB_Staggered_Pins',
              {1: f'{m}_RA', 2: 'GND', 3: f'{m}_GA', 4: f'{m}_BA'}, pos=lpos)
        for c, val in (('R', '470R'), ('G', '330R'), ('B', '330R')):
            b.add(f'R{rn}', 'Device:R', val, R_FP, {1: f'{m}_{c}', 2: f'{m}_{c}A'}, back=True)
            rn += 1

    # buttons: one side to the GPIO line, the other to GND
    btns = [('SW1', 'BTN_DMR', 'DMR', ('c', 27, rows[0])),
            ('SW2', 'BTN_DSTAR', 'D-STAR', ('c', 27, rows[1])),
            ('SW3', 'BTN_YSF', 'YSF', ('c', 27, rows[2])),
            ('SW4', 'BTN_P25', 'P25', ('c', W - 27, rows[0])),
            ('SW5', 'BTN_NXDN', 'NXDN', ('c', W - 27, rows[1])),
            ('SW6', 'BTN_M17', 'M17', ('c', W - 27, rows[2])),
            ('SW7', 'BTN_MENU', 'MENU', ('c', 52, 86)),
            ('SW8', 'BTN_PTT', 'PTT', ('c', W - 52, 86))]
    for ref, net, text, pos in btns:
        b.add(ref, 'Switch:SW_Push', text, 'Button_Switch_THT:SW_PUSH-12mm',
              {1: net, 2: 'GND'}, pos=pos, silk=text)
    b.add('ENC1', 'Device:RotaryEncoder_Switch', 'Volume (EC11 with switch)',
          'Rotary_Encoder:RotaryEncoder_Alps_EC11E-Switch_Vertical_H20mm',
          {'A': 'ENC_VOL_A', 'B': 'ENC_VOL_B', 'C': 'GND', 'S1': 'BTN_MUTE', 'S2': 'GND'},
          pos=('c', 22, 85), silk='VOLUME / MUTE')
    b.add('ENC2', 'Device:RotaryEncoder_Switch', 'Channel (EC11 with switch)',
          'Rotary_Encoder:RotaryEncoder_Alps_EC11E-Switch_Vertical_H20mm',
          {'A': 'ENC_CH_A', 'B': 'ENC_CH_B', 'C': 'GND', 'S1': 'BTN_LINK', 'S2': 'GND'},
          pos=('c', W - 22, 85), silk='CHANNEL / LINK')

    # 10k pull-ups on the panel for the 14 input lines (the ribbon is long for the Pi's own)
    b.add('RN1', 'Device:R_Network08', '10k x8', 'Resistor_THT:R_Array_SIP9',
          {1: '+3V3', 2: 'BTN_PTT', 3: 'BTN_MENU', 4: 'ENC_CH_A', 5: 'ENC_CH_B', 6: 'BTN_LINK',
           7: 'ENC_VOL_A', 8: 'ENC_VOL_B', 9: 'BTN_MUTE'}, back=True, pos=('b', 52, 51))
    b.add('RN2', 'Device:R_Network08', '10k x8', 'Resistor_THT:R_Array_SIP9',
          {1: '+3V3', 2: 'BTN_DMR', 3: 'BTN_DSTAR', 4: 'BTN_YSF', 5: 'BTN_P25', 6: 'BTN_NXDN',
           7: 'BTN_M17'}, back=True, pos=('b', 52, 56))

    # decoupling
    for ref, val, fp, n in (('C1', '100nF', C_FP, '+5V'), ('C2', '100nF', C_FP, '+5V'),
                            ('C3', '100nF', C_FP, '+5V'), ('C5', '100nF', C_FP, '+3V3'),
                            ('C4', '10uF', 'Capacitor_THT:CP_Radial_D5.0mm_P2.00mm', '+5V')):
        sym = 'Device:C_Polarized' if ref == 'C4' else 'Device:C'
        b.add(ref, sym, val, fp, {1: n, 2: 'GND'}, back=True)

    for i, (x, y) in enumerate([(4, 4), (W - 4, 4), (4, H - 4), (W - 4, H - 4)]):
        b.add(f'H{i + 1}', 'Mechanical:MountingHole', 'M3', H_FP, pos=('c', x, y))

    # Everything on the back packs in behind the LCD and between the bottom controls.
    back = [p.ref for p in b.parts if p.back and p.pos is None]
    # inside the LCD outline, clear of its pin row and mounting holes
    b.pack.append(([(88, 12, 138, 61), (77, 35, 88, 61)], back))
    b.notes += [(W / 2, 70, 'STATUS', 'F.SilkS'),
                (W / 2, 97, 'Digital TRX panel', 'B.SilkS')]
    return b


# ------------------------------------------------------------------ schematic
def write_schematic(b, sym_uuid):
    sheet = uid(b.name, 'sheet')
    embedded = {}
    insts, labels, ncs = [], [], []
    flags = []
    for p in b.parts:
        lib, nm = p.sym.split(':')
        node = flat_symbol(lib, nm)
        if p.sym not in embedded:
            e = parse(dump(node))
            e[1] = Q(p.sym)
            embedded[p.sym] = e
    power_nets = sorted({n for p in b.parts for n in p.nets.values()
                         if n in ('+5V', '+3V3', 'GND')})
    flag = flat_symbol('power', 'PWR_FLAG')
    e = parse(dump(flag))
    e[1] = Q('power:PWR_FLAG')
    embedded['power:PWR_FLAG'] = e

    # simple flow layout on an A2 sheet
    x0, y0, xmax = 25.4, 25.4, 560.0
    cx, cy, rowh = x0, y0, 0.0

    def g(v):
        return round(v / 2.54) * 2.54

    items = [(p, flat_symbol(*p.sym.split(':'))) for p in b.parts]
    items += [(Part(f'#FLG0{i + 1}', 'power:PWR_FLAG', 'PWR_FLAG', '', {1: n}, on_board=False), flag)
              for i, n in enumerate(power_nets)]
    for p, node in items:
        pins = sym_pins(node)
        xs = [q[2] for q in pins] or [0]
        ys = [-q[3] for q in pins] or [0]
        minx, maxx, miny, maxy = min(xs), max(xs), min(ys), max(ys)
        # room for the labels on whichever sides have pins
        left = 22 if any(q[4] == 0 for q in pins) else 6
        right = 22 if any(q[4] == 180 for q in pins) else 6
        top = 12 if any(q[4] == 270 for q in pins) else 6
        bot = 12 if any(q[4] == 90 for q in pins) else 8
        w = (maxx - minx) + left + right
        h = (maxy - miny) + top + bot
        if cx + w > xmax:
            cx, cy, rowh = x0, cy + rowh, 0.0
        X, Y = g(cx + left - minx), g(cy + top - miny)
        cx += w
        rowh = max(rowh, h)
        u = sym_uuid[p.ref] if p.ref in sym_uuid else uid(b.name, 'sym', p.ref)
        sym_uuid[p.ref] = u
        hide = ['hide', 'yes']
        props = [
            ['property', Q('Reference'), Q(p.ref), ['at', X, g(Y + miny - 3), 0],
             ['effects', ['font', ['size', 1.27, 1.27]]] + ([hide] if p.ref.startswith('#') else [])],
            ['property', Q('Value'), Q(p.value), ['at', X, g(Y + maxy + 3), 0],
             ['effects', ['font', ['size', 1.27, 1.27]]]],
            ['property', Q('Footprint'), Q(p.fp), ['at', X, Y, 0],
             ['effects', ['font', ['size', 1.27, 1.27]], hide]],
            ['property', Q('Datasheet'), Q('~'), ['at', X, Y, 0],
             ['effects', ['font', ['size', 1.27, 1.27]], hide]],
        ]
        in_bom = p.on_board and not p.ref.startswith('H')
        inst = ['symbol', ['lib_id', Q(p.sym)], ['at', X, Y, 0], ['unit', 1],
                ['exclude_from_sim', 'no'], ['in_bom', 'yes' if in_bom else 'no'],
                ['on_board', 'yes' if p.on_board else 'no'], ['dnp', 'no'], ['uuid', Q(u)]] + props
        done = set()
        for num, name, px, py, ang, hidden in pins:
            inst.append(['pin', Q(num), ['uuid', Q(uid(b.name, p.ref, 'pin', num))]])
            ax, ay = round(X + px, 3), round(Y - py, 3)
            if (ax, ay) in done:
                continue
            net = p.nets.get(num)
            if hidden and net is None:
                continue
            done.add((ax, ay))
            if net is None:
                ncs.append(['no_connect', ['at', ax, ay], ['uuid', Q(uid(b.name, p.ref, 'nc', num))]])
                continue
            lang, just = {0: (180, 'right'), 180: (0, 'left'), 90: (270, 'right'),
                          270: (90, 'left')}[ang]
            labels.append(['label', Q(net), ['at', ax, ay, lang],
                           ['effects', ['font', ['size', 1.27, 1.27]], ['justify', just, 'bottom']],
                           ['uuid', Q(uid(b.name, p.ref, 'lbl', num))]])
        inst.append(['instances', ['project', Q(b.name),
                                   ['path', Q('/' + sheet), ['reference', Q(p.ref)], ['unit', 1]]]])
        insts.append(inst)
        unknown = set(p.nets) - {q[0] for q in pins}
        if unknown:
            raise SystemExit(f'{b.name} {p.ref}: no pins {sorted(unknown)} on {p.sym}')

    sch = ['kicad_sch', ['version', '20250114'], ['generator', Q('eeschema')],
           ['generator_version', Q('9.0')], ['uuid', Q(sheet)], ['paper', Q('A2')],
           ['title_block', ['title', Q(b.title)], ['date', Q('2026-11-21')], ['rev', Q('1')],
            ['comment', 1, Q('Generated by hardware/gen_boards.py. Nets are joined by labels.')],
            ['comment', 2, Q('Ribbon: 20-way IDC, pin 1 to pin 1 between the two boards.')]],
           ['lib_symbols'] + list(embedded.values())] + labels + ncs + insts + \
          [['sheet_instances', ['path', Q('/'), ['page', Q('1')]]], ['embedded_fonts', 'no']]
    d = os.path.join(HERE, b.name)
    with open(os.path.join(d, b.name + '.kicad_sch'), 'w') as f:
        f.write(dump(sch) + '\n')
    return sheet


# ------------------------------------------------------------------ PCB
def mm(v):
    return pcbnew.FromMM(v)


def pads_centre(fp):
    xs, ys = [], []
    for pad in fp.Pads():
        if pad.GetNumber():
            xs.append(pad.GetPosition().x)
            ys.append(pad.GetPosition().y)
    if not xs:
        return fp.GetPosition()
    return pcbnew.VECTOR2I(int((min(xs) + max(xs)) / 2), int((min(ys) + max(ys)) / 2))


def write_pcb(b, sym_uuid, sheet):
    path = os.path.join(HERE, b.name, b.name + '.kicad_pcb')
    if os.path.exists(path):
        os.remove(path)
    board = pcbnew.NewBoard(path)
    OX, OY = 50.0, 50.0                  # board origin on the page
    W, H = b.size
    # Unused pins: the schematic gives each one a net of its own; the PCB must match.
    for p in b.parts:
        pins = sym_pins(flat_symbol(*p.sym.split(':')))
        shown = {(q[2], q[3]) for q in pins if not q[5]}
        for num, name, x, y, ang, hidden in pins:
            if num not in p.nets and not (hidden and (x, y) in shown):
                p.nets[num] = 'unconnected-(%s-%s-Pad%s)' % (p.ref, name.replace('/', '{slash}'), num)
    nets = {}
    for n in sorted({n for p in b.parts for n in p.nets.values()}):
        ni = pcbnew.NETINFO_ITEM(board, n if n.startswith('unconnected-') else '/' + n)
        board.Add(ni)
        nets[n] = ni

    fps = {}
    for p in b.parts:
        if not p.on_board:
            continue
        lib, name = p.fp.split(':')
        libdir = os.path.join(HERE, 'lib', 'dtrx.pretty') if lib == 'dtrx' else \
            f'{KI}/footprints/{lib}.pretty'
        fp = pcbnew.FootprintLoad(libdir, name)
        if fp is None:
            raise SystemExit(f'footprint {p.fp} not found')
        fp.SetFPID(pcbnew.LIB_ID(lib, name))
        fp.SetReference(p.ref)
        fp.SetValue(p.value)
        fp.SetPath(pcbnew.KIID_PATH('/' + sym_uuid[p.ref]))
        try:
            fp.SetSheetname('Root')
            fp.SetSheetfile(b.name + '.kicad_sch')
        except AttributeError:
            pass
        for pad in fp.Pads():
            net = p.nets.get(pad.GetNumber())
            if net:
                pad.SetNet(nets[net])
        if p.ref.startswith('H'):
            fp.Reference().SetVisible(False)
        board.Add(fp)
        fp.SetPosition(pcbnew.VECTOR2I(0, 0))
        fp.SetOrientationDegrees(p.rot)
        if p.back:
            fp.Flip(fp.GetPosition(), pcbnew.FLIP_DIRECTION_TOP_BOTTOM)
        fps[p.ref] = fp
        if p.pos:
            kind, x, y = p.pos
            target = pcbnew.VECTOR2I(mm(OX + x), mm(OY + y))
            if kind == 'o':
                fp.SetPosition(target)
            elif kind == 'b':                       # top-left corner of the footprint's extent
                bb = fp.GetBoundingBox(False)
                fp.Move(pcbnew.VECTOR2I(target.x - bb.GetLeft(), target.y - bb.GetTop()))
            else:
                c = pads_centre(fp)
                fp.Move(target - c)
        if p.silk:
            t = pcbnew.PCB_TEXT(board)
            t.SetText(p.silk)
            t.SetLayer(pcbnew.F_SilkS)
            t.SetTextSize(pcbnew.VECTOR2I(mm(1.5), mm(1.5)))
            t.SetTextThickness(mm(0.25))
            bb = fp.GetCourtyard(pcbnew.F_CrtYd).BBox() if hasattr(fp, 'GetCourtyard') else None
            c = pads_centre(fp)
            top = fp.GetBoundingBox(False).GetTop() if bb is None else bb.GetTop()
            t.SetPosition(pcbnew.VECTOR2I(c.x, top - mm(1.6)))
            board.Add(t)

    # automatic packing of the remaining back-side parts
    for regions, refs in b.pack:
        order = sorted(refs, key=lambda r: (-fps[r].GetBoundingBox(False).GetHeight(), r))
        regions = list(regions)
        x0, y0, x1, y1 = regions.pop(0)
        cx, cy, rowh = x0, y0, 0.0
        gap = 1.2
        for r in order:
            fp = fps[r]
            bb = fp.GetBoundingBox(False)
            w, h = pcbnew.ToMM(bb.GetWidth()), pcbnew.ToMM(bb.GetHeight())
            if cx + w > x1:
                cx, cy, rowh = x0, cy + rowh + gap, 0.0
            while cy + h > y1:
                if not regions:
                    raise SystemExit(f'{b.name}: no room to place {r}')
                x0, y0, x1, y1 = regions.pop(0)
                cx, cy, rowh = x0, y0, 0.0
            dx = mm(OX + cx) - bb.GetLeft()
            dy = mm(OY + cy) - bb.GetTop()
            fp.Move(pcbnew.VECTOR2I(dx, dy))
            cx += w + gap
            rowh = max(rowh, h)

    # outline
    rect = pcbnew.PCB_SHAPE(board)
    rect.SetShape(pcbnew.SHAPE_T_RECT)
    rect.SetStart(pcbnew.VECTOR2I(mm(OX), mm(OY)))
    rect.SetEnd(pcbnew.VECTOR2I(mm(OX + W), mm(OY + H)))
    rect.SetLayer(pcbnew.Edge_Cuts)
    rect.SetWidth(mm(0.1))
    board.Add(rect)

    for x, y, text, layer in b.notes:
        t = pcbnew.PCB_TEXT(board)
        t.SetText(text)
        t.SetLayer({'F.SilkS': pcbnew.F_SilkS, 'B.SilkS': pcbnew.B_SilkS,
                    'F.Fab': pcbnew.F_Fab, 'B.Fab': pcbnew.B_Fab}[layer])
        t.SetTextSize(pcbnew.VECTOR2I(mm(1.5), mm(1.5)))
        t.SetTextThickness(mm(0.25))
        t.SetPosition(pcbnew.VECTOR2I(mm(OX + x), mm(OY + y)))
        if layer.startswith('B.'):
            t.SetMirrored(True)
        board.Add(t)

    # ground pour on both sides
    for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
        z = pcbnew.ZONE(board)
        z.SetLayer(layer)
        z.SetNet(nets['GND'])
        z.SetLocalClearance(mm(0.4))
        z.SetMinThickness(mm(0.25))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
        ol = z.Outline()
        ol.NewOutline()
        for x, y in ((0.5, 0.5), (W - 0.5, 0.5), (W - 0.5, H - 0.5), (0.5, H - 0.5)):
            ol.Append(mm(OX + x), mm(OY + y))
        board.Add(z)

    ds = board.GetDesignSettings()
    ds.SetCopperLayerCount(2)
    nc = ds.m_NetSettings.GetDefaultNetclass()
    nc.SetTrackWidth(mm(0.3))
    nc.SetClearance(mm(0.2))
    # Zones are left unfilled: press B in KiCad to fill them with the board's own rules.
    tb = board.GetTitleBlock()
    tb.SetTitle(b.title)
    tb.SetRevision('1')
    tb.SetDate('2026-11-21')
    board.Save(path)


def write_project(b):
    d = os.path.join(HERE, b.name)
    os.makedirs(d, exist_ok=True)
    pro = os.path.join(d, b.name + '.kicad_pro')
    data = {}
    if os.path.exists(pro):
        try:
            data = json.load(open(pro))
        except ValueError:
            data = {}
    data.setdefault('meta', {'filename': b.name + '.kicad_pro', 'version': 3})
    data['meta']['filename'] = b.name + '.kicad_pro'
    # power nets get wider tracks
    data['net_settings'] = {
        'classes': [
            {'name': 'Default', 'track_width': 0.3, 'clearance': 0.2, 'via_diameter': 0.8,
             'via_drill': 0.4, 'priority': 2147483647},
            {'name': 'Power', 'track_width': 0.8, 'clearance': 0.2, 'via_diameter': 1.0,
             'via_drill': 0.5, 'priority': 0}],
        'meta': {'version': 4},
        'netclass_assignments': None,
        'netclass_patterns': [{'netclass': 'Power', 'pattern': p}
                              for p in ('+5V', '+3V3', 'GND', 'VIN')]}
    json.dump(data, open(pro, 'w'), indent=2)
    with open(os.path.join(d, 'sym-lib-table'), 'w') as f:
        f.write('(sym_lib_table\n\t(version 7)\n\t(lib (name "dtrx") (type "KiCad") '
                '(uri "${KIPRJMOD}/../lib/dtrx.kicad_sym") (options "") (descr "Digital TRX parts"))\n)\n')
    with open(os.path.join(d, 'fp-lib-table'), 'w') as f:
        f.write('(fp_lib_table\n\t(version 7)\n\t(lib (name "dtrx") (type "KiCad") '
                '(uri "${KIPRJMOD}/../lib/dtrx.pretty") (options "") (descr "Digital TRX footprints"))\n)\n')


def main():
    os.makedirs(os.path.join(HERE, 'lib', 'dtrx.pretty'), exist_ok=True)
    make_custom_lib()
    make_lcd_footprint()
    for b in (main_board(), panel_board()):
        write_project(b)
        sym_uuid = {}
        sheet = write_schematic(b, sym_uuid)
        write_pcb(b, sym_uuid, sheet)
        write_project(b)
        print('wrote', b.name)


if __name__ == '__main__':
    main()
