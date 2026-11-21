#!/usr/bin/env python3
"""Generates the machine's panel.svg.fragment, or a standalone review SVG.

  python3 tools/gen_panel.py
  python3 tools/gen_panel.py --state 0,7 --text "C5  G3" --out /path/to/review.svg

The page shows two lanes of 12 dots, one dot per pitch class (C to B): arp A's
lane on top, arp B's below, each lighting the note its arp plays. --state is
the two pitch classes (0 = C, 11 = B; -1 for none), as display() in player.cpp
shows them. Everything is drawn with straight-line <path> elements: the OP-XY
did not draw <circle>. Artwork stays below the header (y = 38).
"""
import argparse
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FOLDER = next(p.parent for p in (ROOT / 'machines').glob('*/definition.json'))
PREFIX = json.loads((FOLDER / 'definition.json').read_text(encoding='utf-8'))['id']
OUTPUT = FOLDER / 'panel.svg.fragment'
X0, DX = 136, 20          # first dot and spacing: 12 dots from x = 136 to 356
LANES = (84, 134)         # y of arp A's and arp B's lanes
LIT_R, MARK_R = 7, 2.5
SHARPS = {1, 3, 6, 8, 10}


def n(value):
    text = f'{value:.3f}'.rstrip('0').rstrip('.')
    return '0' if text in ('-0', '') else text


def disc(radius, sides=16):
    points = [(radius * math.sin(2 * math.pi * i / sides), -radius * math.cos(2 * math.pi * i / sides))
              for i in range(sides)]
    return 'M' + ' L'.join(f'{n(x)} {n(y)}' for x, y in points) + ' Z'


def at(x, y):
    return f'transform="matrix(1 0 0 1 {n(x)} {n(y)})"'


def paint(colour):
    return f'fill="{colour}" stroke="{colour}" stroke-width="1"'


def dots():
    """The 24 bound dots in dot order: arp A's C..B, then arp B's C..B."""
    return [(12 * lane + pc, f'<path id="{PREFIX}_dot_{12 * lane + pc}" {at(X0 + DX * pc, y)} '
             f'd="{disc(LIT_R)}" {paint("#FFFFFF")}/>')
            for lane, y in enumerate(LANES) for pc in range(12)]


def label(text, y):
    return (f'<text {at(X0 - 26, y + 5)} fill="#9A9A9A" font-family="UniversNextPro-Light" '
            f'font-size="14">{text}</text>')


def static_art():
    art = [label('a', LANES[0]), label('b', LANES[1])]
    for y in LANES:
        for pc in range(12):
            art.append(f'<path {at(X0 + DX * pc, y)} d="{disc(MARK_R, 8)}" '
                       f'{paint("#555555" if pc in SHARPS else "#999999")}/>')
    return art


def text_node(content):
    return (f'<text id="{PREFIX}_text" transform="matrix(1 0 0 1 240 190)" fill="#FFFFFF" '
            f'text-anchor="middle" font-family="UniversNextPro-Light" font-size="26">{content}</text>')


def fragment():
    body = static_art() + [svg for _, svg in dots()] + [text_node('ready')]
    return f'<g id="{PREFIX}_panel">\n' + '\n'.join(body) + '\n</g>\n'


def review(a, b, content):
    lit = {i for i in ((a if a >= 0 else None), (12 + b if b >= 0 else None)) if i is not None}
    body = '\n'.join(static_art() + [svg for i, svg in dots() if i in lit] + [text_node(content)])
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="960" height="444" viewBox="0 0 480 222">\n'
            '<rect width="480" height="222" fill="#000000"/>\n<rect width="480" height="38" fill="#1C1C1C"/>\n'
            + body + '\n</svg>\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--state', help='a,b: the pitch classes lit in each lane, -1 for none')
    parser.add_argument('--text', default='C5  G3')
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    if args.state:
        if not args.out:
            parser.error('--state needs --out')
        a, b = (int(part) for part in args.state.split(','))
        args.out.write_text(review(a, b, args.text), encoding='utf-8')
    else:
        (args.out or OUTPUT).write_text(fragment(), encoding='utf-8')


if __name__ == '__main__':
    main()
