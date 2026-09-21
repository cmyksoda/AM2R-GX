#!/usr/bin/env python3
"""Turn a room sweep's drawing record into packing groups for repack_textures.py.

  tools/texture_groups.py ORIGINAL.win sweep-gecko.log groups.json [--also more-gecko.log ...] [--score REPACKED.win ...]

The sweep logs, per room, which texture page items were drawn ("SWEEP tpag room=R off=N <hex>").
Graphics seen in most rooms go together ("common": Samus, HUD, fonts, shots); graphics seen in one
area go with that area; the never-seen rest stays grouped by sprite so an animation's frames share
pages. --score prints pages-touched-per-room for any repacked file, using the same record.
"""
import argparse
import json
import re
import struct
import sys
from collections import Counter, defaultdict

COMMON_SHARE = 0.5  # drawn in at least this share of rooms => always-resident material
CHUNK_PIXELS = 900 * 900  # never-seen sprites are grouped about a page at a time, in name order
BIG_SPRITE = 2 * 1024 * 1024  # a never-seen sprite this large (the animated title) gets pages of its own
TPAG_LINE = re.compile(r'SWEEP tpag room=(\d+) off=(\d+) ([0-9a-f]+)')


def u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


def chunks(d):
    out, off = {}, 8
    while off < len(d):
        out[d[off:off + 4]] = off + 8
        off += 8 + u32(d, off + 4)
    return out


def plist(d, off):
    return [u32(d, off + 4 + 4 * i) for i in range(u32(d, off))]


def cstr(d, ptr):
    return d[ptr:ptr + u32(d, ptr - 4)].decode('utf-8', 'replace')


def tpag_rects(d, ch):
    """TPAG index -> (page, x, y, w, h)."""
    return [struct.unpack_from('<h', d, p + 20) + struct.unpack_from('<4H', d, p) for p in plist(d, ch[b'TPAG'])]


def owners(d, ch):
    """TPAG pointer -> resource name, so unseen frames of one sprite can stay together."""
    out = {}
    for p in plist(d, ch[b'SPRT']):
        for k in range(u32(d, p + 56)):
            out.setdefault(u32(d, p + 60 + 4 * k), cstr(d, u32(d, p)))
    for p in plist(d, ch[b'BGND']):
        out.setdefault(u32(d, p + 16), cstr(d, u32(d, p)))
    for p in plist(d, ch[b'FONT']):
        out.setdefault(u32(d, p + 28), cstr(d, u32(d, p)))
    return out


def read_usage(log):
    """room index -> set of TPAG indices drawn there."""
    rooms = defaultdict(set)
    for m in map(TPAG_LINE.search, open(log, errors='replace')):
        if m:
            room, off, bits = int(m.group(1)), int(m.group(2)), bytes.fromhex(m.group(3))
            rooms[room].update((off + i) * 8 + b for i, byte in enumerate(bits) for b in range(8) if byte >> b & 1)
    return rooms


def area_of(name):
    m = re.match(r'rm_a(\d)', name)
    return 'area' + m.group(1) if m else 'misc'


def score(path, usage, label):
    """Working set per room: how many pages, and how many MB of RGB5A3, a room's graphics sit on."""
    d = open(path, 'rb').read()
    ch = chunks(d)
    rects = tpag_rects(d, ch)
    size = [struct.unpack('>II', d[u32(d, e + 4) + 16:u32(d, e + 4) + 24]) for e in plist(d, ch[b'TXTR'])]
    mb = lambda pages: sum(size[p][0] * size[p][1] for p in pages) * 2 / 2**20
    rooms = [{rects[i][0] for i in items if i < len(rects)} for items in usage.values()]
    count, weight = sorted(map(len, rooms)), sorted(map(mb, rooms))
    n = len(rooms)
    print(f'{label}\n  {len(size)} pages, {mb(range(len(size))):.0f} MB in all; every recorded room together: '
          f'{mb(set().union(*rooms)):.1f} MB\n'
          f'  per room: pages median {count[n // 2]} / p90 {count[n * 9 // 10]} / max {count[-1]};  '
          f'MB median {weight[n // 2]:.1f} / p90 {weight[n * 9 // 10]:.1f} / max {weight[-1]:.1f}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('win'); ap.add_argument('log'); ap.add_argument('out')
    ap.add_argument('--also', nargs='*', default=[], help='more sweep logs of the same data file; their records are merged room by room')
    ap.add_argument('--score', nargs='*', default=[])
    args = ap.parse_args()
    d = open(args.win, 'rb').read()
    ch = chunks(d)
    rects = tpag_rects(d, ch)
    ptrs = plist(d, ch[b'TPAG'])
    own = owners(d, ch)
    room_names = [cstr(d, u32(d, p)) for p in plist(d, ch[b'ROOM'])]
    usage = read_usage(args.log)
    for extra in args.also:
        for room, items in read_usage(extra).items():
            usage[room] |= items
    if not usage:
        sys.exit('no "SWEEP tpag" lines in the log')

    seen_in = defaultdict(set)
    for room, items in usage.items():
        for i in items:
            if i < len(rects):
                seen_in[rects[i]].add(room)
    groups, stats, unseen = {}, Counter(), defaultdict(list)
    for idx, rect in enumerate(rects):
        if rect in groups or any(rect in frames for frames in unseen.values()):
            continue
        rooms = seen_in.get(rect)
        if not rooms:
            unseen[own.get(ptrs[idx], 'zz')].append(rect)
            continue
        if len(rooms) >= COMMON_SHARE * len(usage):
            groups[rect] = 'common'
        else:
            areas = {area_of(room_names[r]) for r in rooms}
            groups[rect] = areas.pop() if len(areas) == 1 else 'shared'
    # Never drawn in the sweep (bosses, cutscenes, most of Samus): walk the sprites in name order and
    # cut a new group about every page, so an animation's frames and a family of sprites share pages.
    chunk, filled = 0, 0
    for name in sorted(unseen, key=str.lower):
        area = sum(r[3] * r[4] for r in unseen[name])
        if area >= BIG_SPRITE:
            for r in unseen[name]:
                groups[r] = 'x-' + name
            continue
        if filled + area > CHUNK_PIXELS:
            chunk, filled = chunk + 1, 0
        filled += area
        for r in unseen[name]:
            groups[r] = f'unseen{chunk:03d}'
    for rect, group in groups.items():
        stats['x (big unseen sprites)' if group.startswith('x-') else group[:6] if group.startswith('unseen') else group] += rect[3] * rect[4]
    json.dump({','.join(map(str, r)): g for r, g in groups.items()}, open(args.out, 'w'))
    print(f'{len(usage)} rooms recorded, {len(seen_in)} of {len(set(rects))} graphics seen')
    for g, px in sorted(stats.items()):
        print(f'  {g:24s} {px / 2**20:6.2f} Mpx')
    for path in args.score:
        score(path, usage, path)


if __name__ == '__main__':
    main()
