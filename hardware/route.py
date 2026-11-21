#!/usr/bin/env python3
"""Autoroute the boards with Freerouting (ground included), then fill the ground pours.

    python3 hardware/route.py [dtrx-main] [dtrx-panel]

Needs Java and the Freerouting jar (https://github.com/freerouting/freerouting/releases),
found at $FREEROUTING_JAR or ~/.local/share/freerouting/freerouting-<version>.jar.
It writes the routed tracks into each .kicad_pcb, so run it after gen_boards.py.
"""
import glob
import json
import os
import re
import subprocess
import sys
import tempfile

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))


def jar():
    j = os.environ.get('FREEROUTING_JAR')
    if j:
        return j
    found = sorted(glob.glob(os.path.expanduser('~/.local/share/freerouting/freerouting-*.jar')))
    if not found:
        sys.exit('Freerouting jar not found: set FREEROUTING_JAR')
    return found[-1]


def route(name, passes=100):
    pcb = os.path.join(HERE, name, name + '.kicad_pcb')
    with tempfile.TemporaryDirectory() as tmp:
        dsn, ses = os.path.join(tmp, name + '.dsn'), os.path.join(tmp, name + '.ses')
        board = pcbnew.LoadBoard(pcb)
        if not pcbnew.ExportSpecctraDSN(board, dsn):
            sys.exit(f'{name}: DSN export failed')
        # Route ground as tracks too, so no pin relies on the pour reaching it (tracks between
        # header pins can cut a pin off). The pours are filled afterwards as extra ground.
        with open(dsn) as f:
            text = re.sub(r'\(plane [^()]*\(polygon [^()]*\)\)\s*', '', f.read())
        with open(dsn, 'w') as f:
            f.write(text)
        subprocess.run(['java', '-jar', jar(), '-de', dsn, '-do', ses, '-mp', str(passes),
                        '--gui.enabled=false'], check=True)
        board = pcbnew.LoadBoard(pcb)
        if not pcbnew.ImportSpecctraSES(board, ses):
            sys.exit(f'{name}: SES import failed')
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(pcb)
    solid_starved_pads(pcb)
    print('routed', name)


def solid_starved_pads(pcb):
    """A ground pin with tracks on every side has no room for thermal spokes. Those pins are
    already joined by tracks, so connect just them solidly to the pour and fill again."""
    for _ in range(3):
        with tempfile.TemporaryDirectory() as tmp:
            rpt = os.path.join(tmp, 'drc.json')
            subprocess.run(['kicad-cli', 'pcb', 'drc', '--format', 'json', '-o', rpt, pcb],
                           check=True, stdout=subprocess.DEVNULL)
            with open(rpt) as f:
                report = json.load(f)
        starved = set()
        for v in report.get('violations', []):
            if v.get('type') != 'starved_thermal':
                continue
            for item in v.get('items', []):
                m = re.search(r'pad (\S+) \[.*\] of (\S+)', item.get('description', ''))
                if m:
                    starved.add((m.group(2), m.group(1)))
        if not starved:
            return
        board = pcbnew.LoadBoard(pcb)
        for ref, num in sorted(starved):
            for pad in board.FindFootprintByReference(ref).Pads():
                if pad.GetNumber() == num:
                    pad.SetLocalZoneConnection(pcbnew.ZONE_CONNECTION_FULL)
            print(f'  {ref} pin {num}: solid to the ground pour')
        pcbnew.ZONE_FILLER(board).Fill(board.Zones())
        board.Save(pcb)


if __name__ == '__main__':
    for n in sys.argv[1:] or ['dtrx-main', 'dtrx-panel']:
        route(n)
