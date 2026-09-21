#!/usr/bin/env python3
"""Repack a GameMaker: Studio 1.x data.win's texture pages onto pages the Wii's GPU can bind.

  tools/repack_textures.py IN.win OUT.win [--max 1024] [--gutter 1] [--groups groups.json]

Every graphic is moved whole, pixels untouched, together with the ring of pixels GameMaker left
around it. TPAG entries are patched in place; TXTR is rebuilt; AUDO's pointers are shifted.
Check the result with tools/verify_repack.py, which shares no code with this file on purpose.
"""
import argparse
import io
import json
import struct
import sys
import zlib

import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None
BLOB_ALIGN = 128  # GameMaker aligns every PNG in TXTR to this


def u32(buf, off):
    return struct.unpack_from('<I', buf, off)[0]


def read_chunks(data):
    if data[:4] != b'FORM' or u32(data, 4) != len(data) - 8:
        sys.exit('not a data.win (bad FORM header)')
    chunks, order, off = {}, [], 8
    while off < len(data):
        name = data[off:off + 4].decode('ascii')
        size = u32(data, off + 4)
        chunks[name] = (off + 8, size)
        order.append(name)
        off += 8 + size
    return chunks, order


def pointer_list(data, off):
    return [u32(data, off + 4 + 4 * i) for i in range(u32(data, off))]


class MaxRects:
    """Best-short-side-fit MaxRects bin."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.free = [(0, 0, w, h)]
        self.placed = []

    def find(self, w, h):
        best = None
        for fx, fy, fw, fh in self.free:
            if w <= fw and h <= fh:
                score = (min(fw - w, fh - h), max(fw - w, fh - h), fy, fx)
                if best is None or score < best[0]:
                    best = (score, fx, fy)
        return best and best[1:]

    def place(self, key, x, y, w, h):
        self.placed.append((key, x, y, w, h))
        kept, fresh = [], []
        for f in self.free:
            fx, fy, fw, fh = f
            if x >= fx + fw or x + w <= fx or y >= fy + fh or y + h <= fy:
                kept.append(f)
                continue
            if x > fx:
                fresh.append((fx, fy, x - fx, fh))
            if x + w < fx + fw:
                fresh.append((x + w, fy, fx + fw - x - w, fh))
            if y > fy:
                fresh.append((fx, fy, fw, y - fy))
            if y + h < fy + fh:
                fresh.append((fx, y + h, fw, fy + fh - y - h))
        fresh = sorted(set(fresh), key=lambda r: -r[2] * r[3])
        for i, r in enumerate(fresh):
            if not any(contains(o, r) for o in kept) and not any(contains(o, r) for o in fresh[:i]):
                kept.append(r)
        self.free = kept


def contains(outer, inner):
    return (outer[0] <= inner[0] and outer[1] <= inner[1]
            and outer[0] + outer[2] >= inner[0] + inner[2]
            and outer[1] + outer[3] >= inner[1] + inner[3])


ORDERS = {
    'height': lambda c: (-c[2], -c[1]),
    'area': lambda c: (-c[1] * c[2], -c[2]),
    'maxside': lambda c: (-max(c[1], c[2]), -min(c[1], c[2])),
}


def pack_into(cells, w, h, order):
    """First-fit over as many w*h bins as it takes. cells: [(key, cw, ch)]."""
    bins = []
    for key, cw, ch in sorted(cells, key=ORDERS[order]):
        for b in bins:
            spot = b.find(cw, ch)
            if spot:
                break
        else:
            b = MaxRects(w, h)
            bins.append(b)
            spot = b.find(cw, ch)
            if not spot:
                sys.exit(f'item {key} ({cw}x{ch} with gutter) does not fit a {w}x{h} page')
        b.place(key, spot[0], spot[1], cw, ch)
    return bins


def pack_group(cells, size):
    best = min((pack_into(cells, size, size, o) for o in ORDERS), key=lambda bs: (len(bs), used_area(bs[-1])))
    # The group's last page is usually part-empty: squeeze it, so the Wii holds no blank texels.
    tail = [(k, w, h) for k, _, _, w, h in best[-1].placed]
    fits = [b for w in (size // 2, size) for b in [shortest_bin(tail, w, size)] if b]
    return best[:-1] + [min(fits + best[-1:], key=lambda b: b.w * b.h)]


def shortest_bin(cells, w, size):
    """Binary-search the least height (in 16s) at which the cells still pack at width w."""
    if any(cw > w for _, cw, _ in cells):
        return None
    lo, hi, found = -(-max(ch for _, _, ch in cells) // 16), size // 16, None
    while lo <= hi:
        mid = (lo + hi) // 2
        b = next((b for b in (pack_into_one(cells, w, mid * 16, o) for o in ORDERS) if b), None)
        if b:
            found, hi = b, mid - 1
        else:
            lo = mid + 1
    return found


def pack_into_one(cells, w, h, order):
    b = MaxRects(w, h)
    for key, cw, ch in sorted(cells, key=ORDERS[order]):
        spot = b.find(cw, ch)
        if not spot:
            return None
        b.place(key, spot[0], spot[1], cw, ch)
    return b


def trimmed(b):
    """Page size = the used extent, rounded up to GX's 4x4 texel blocks."""
    return (-(-max(x + w for _, x, _, w, _ in b.placed) // 4) * 4,
            -(-max(y + h for _, _, y, _, h in b.placed) // 4) * 4)


def used_area(b):
    return sum(w * h for _, _, _, w, h in b.placed)


def cut_cell(page, x, y, w, h, g):
    """The item plus its g-pixel ring, verbatim. Off-page ring pixels repeat the edge, like GX_CLAMP."""
    ph, pw = page.shape[:2]
    x0, y0, x1, y1 = max(x - g, 0), max(y - g, 0), min(x + w + g, pw), min(y + h + g, ph)
    cell = page[y0:y1, x0:x1]
    pad = ((y0 - (y - g), (y + h + g) - y1), (x0 - (x - g), (x + w + g) - x1), (0, 0))
    return np.pad(cell, pad, mode='edge') if any(p for side in pad for p in side) else cell


def png_chunk(kind, body):
    return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))


def encode_png(pixels):
    """Unfiltered rows, as GameMaker writes them: measured smaller on pixel art and half the decode work."""
    h, w = pixels.shape[:2]
    rows = np.zeros((h, 1 + w * 4), np.uint8)
    rows[:, 1:] = pixels.reshape(h, w * 4)
    return (b'\x89PNG\r\n\x1a\n' + png_chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
            + png_chunk(b'IDAT', zlib.compress(rows.tobytes(), 9)) + png_chunk(b'IEND', b''))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--max', type=int, default=1024, help='largest page edge (default 1024)')
    ap.add_argument('--gutter', type=int, default=1, help='ring of source pixels kept around each item')
    ap.add_argument('--groups', help='JSON {"<page>,<x>,<y>,<w>,<h>": group} - items sharing a group share pages')
    ap.add_argument('--report', help='write the old->new placement map here as JSON')
    args = ap.parse_args()

    data = open(args.src, 'rb').read()
    chunks, order = read_chunks(data)
    if order[-2:] != ['TXTR', 'AUDO']:
        sys.exit(f'expected the file to end TXTR, AUDO; got {order[-2:]} - offsets after TXTR would go stale')

    txtr_off, txtr_size = chunks['TXTR']
    entries = pointer_list(data, txtr_off)
    blob_offs = [u32(data, e + 4) for e in entries]
    blob_ends = blob_offs[1:] + [txtr_off + txtr_size]
    flag = u32(data, entries[0])
    pages = []
    for start, end in zip(blob_offs, blob_ends):
        im = Image.open(io.BytesIO(data[start:end]))
        if im.mode != 'RGBA':
            sys.exit(f'texture page is {im.mode}, expected RGBA')
        pages.append(np.array(im))

    tpag = pointer_list(data, chunks['TPAG'][0])
    rect_of = {}
    for p in tpag:
        x, y, w, h = struct.unpack_from('<4H', data, p)
        rect_of[p] = (struct.unpack_from('<h', data, p + 20)[0], x, y, w, h)
    rects = sorted(set(rect_of.values()))

    groups = {}
    if args.groups:
        raw = json.load(open(args.groups))
        groups = {tuple(int(v) for v in k.split(',')): g for k, g in raw.items()}
    by_group = {}
    for r in rects:
        by_group.setdefault(groups.get(r, f'page{r[0]:03d}'), []).append(r)

    g = args.gutter
    new_pages, where = [], {}
    for name in sorted(by_group):
        cells = [(r, r[3] + 2 * g, r[4] + 2 * g) for r in by_group[name]]
        for b in pack_group(cells, args.max):
            pw, ph = trimmed(b)
            canvas = np.zeros((ph, pw, 4), np.uint8)
            for r, cx, cy, cw, ch in b.placed:
                canvas[cy:cy + ch, cx:cx + cw] = cut_cell(pages[r[0]], r[1], r[2], r[3], r[4], g)
                where[r] = (len(new_pages), cx + g, cy + g)
            new_pages.append(canvas)
        print(f'  {name}: {len(by_group[name])} items -> {len(new_pages)} pages so far', file=sys.stderr)

    head = bytearray(data[:txtr_off - 8])
    for p in tpag:
        page, x, y = where[rect_of[p]]
        struct.pack_into('<2H', head, p, x, y)
        struct.pack_into('<h', head, p + 20, page)

    n = len(new_pages)
    body = bytearray(struct.pack('<I', n))
    first_entry = txtr_off + 4 + 4 * n
    body += b''.join(struct.pack('<I', first_entry + 8 * i) for i in range(n))
    entry_at = len(body)
    body += bytes(8 * n)
    for i, canvas in enumerate(new_pages):
        body += bytes(-(txtr_off + len(body)) % BLOB_ALIGN)
        struct.pack_into('<2I', body, entry_at + 8 * i, flag, txtr_off + len(body))
        body += encode_png(canvas)
    body += bytes(-len(body) % 16)

    audo_off, audo_size = chunks['AUDO']
    delta = len(body) - txtr_size
    audo = bytearray(data[audo_off:audo_off + audo_size])
    for i in range(u32(audo, 0)):
        struct.pack_into('<I', audo, 4 + 4 * i, u32(audo, 4 + 4 * i) + delta)

    out = head + b'TXTR' + struct.pack('<I', len(body)) + body + b'AUDO' + struct.pack('<I', audo_size) + audo
    struct.pack_into('<I', out, 4, len(out) - 8)
    open(args.dst, 'wb').write(out)

    px = sum(c.shape[0] * c.shape[1] for c in new_pages)
    print(f'{len(pages)} pages -> {n} pages, {px / 2**20:.1f} Mpx ({px * 2 / 2**20:.0f} MB as RGB5A3), '
          f'TXTR {txtr_size / 2**20:.1f} -> {len(body) / 2**20:.1f} MB, crc32 {zlib.crc32(out):08x}')
    if args.report:
        json.dump({'pages': [[c.shape[1], c.shape[0]] for c in new_pages],
                   'items': {','.join(map(str, r)): list(v) for r, v in where.items()}},
                  open(args.report, 'w'))


if __name__ == '__main__':
    main()
