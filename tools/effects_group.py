#!/usr/bin/env python3
"""Put the effect sprites (explosions, smoke, splashes, sparks, impacts) into one "effects" packing group.

  tools/effects_group.py ORIGINAL.win groups.json OUT-groups.json

They are drawn in every area but rarely in a sweep, so the usage record scatters them over area and unseen pages
and the first explosion of a room pulls a whole page mid-play. On one page the game can keep them resident
(wii/src/gx_renderer.c pins the pages of the same names). The words must match GX_EFFECT_WORDS there.
"""
import json
import re
import sys

from texture_groups import chunks, cstr, owners, plist, tpag_rects

EFFECT_WORDS = ('expl', 'smoke', 'splash', 'spark', 'debris', 'impact', 'blast')
NOT_EFFECTS = 'door'  # area 4's blast door is scenery


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    d = open(sys.argv[1], 'rb').read()
    groups = json.load(open(sys.argv[2]))
    ch = chunks(d)
    rects, ptrs, own = tpag_rects(d, ch), plist(d, ch[b'TPAG']), owners(d, ch)
    pattern = re.compile('|'.join(EFFECT_WORDS), re.I)
    moved = px = 0
    for idx, rect in enumerate(rects):
        name = own.get(ptrs[idx], '')
        key = ','.join(map(str, rect))
        if name.startswith('s') and pattern.search(name) and NOT_EFFECTS not in name.lower() and groups.get(key) != 'effects':
            groups[key] = 'effects'
            moved += 1
            px += rect[3] * rect[4]
    json.dump(groups, open(sys.argv[3], 'w'))
    print(f'{moved} page items into "effects", {px / 2**20:.2f} Mpx')


if __name__ == '__main__':
    main()
