#!/usr/bin/env python3
"""Prove a repacked data.win shows exactly the pixels the original did.

  tools/verify_repack.py ORIGINAL.win REPACKED.win [--max 1024] [--gutter 1]

Needs no renderer: every graphic is cropped from the original pages and from the repacked pages
and compared byte for byte (colour under fully transparent pixels included), and everything that
is not a texture must be untouched. Deliberately shares no code with repack_textures.py.
Exit status 0 means every check passed.
"""
import argparse
import io
import struct
import sys
from collections import defaultdict

import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None
TPAG_FMT = '<10Hh'  # src x,y,w,h  target x,y,w,h  bound w,h  page
MOVABLE = (0, 1, 10)  # the only TPAG fields a repack may change: src x, src y, page


class Win:
    def __init__(self, path):
        self.raw = open(path, 'rb').read()
        self.chunks = []
        pos = 8
        while pos < len(self.raw):
            name = self.raw[pos:pos + 4]
            size = self.word(pos + 4)
            self.chunks.append((name, pos + 8, size))
            pos += 8 + size
        self.end = pos

    def word(self, at):
        return int.from_bytes(self.raw[at:at + 4], 'little')

    def chunk(self, name):
        return next((start, size) for n, start, size in self.chunks if n == name)

    def table(self, name):
        start, _ = self.chunk(name)
        return [self.word(start + 4 + 4 * i) for i in range(self.word(start))]

    def page_blobs(self):
        start, size = self.chunk(b'TXTR')
        at = [self.word(e + 4) for e in self.table(b'TXTR')]
        return [self.raw[a:b] for a, b in zip(at, at[1:] + [start + size])]

    def items(self):
        return [struct.unpack_from(TPAG_FMT, self.raw, p) for p in self.table(b'TPAG')]

    def sounds(self):
        return [self.raw[p + 4:p + 4 + self.word(p)] for p in self.table(b'AUDO')]


class Checks:
    def __init__(self):
        self.failed = 0

    def __call__(self, ok, what, detail=''):
        self.failed += not ok
        print(('  ok    ' if ok else '  FAIL  ') + what + (f'  [{detail}]' if detail else ''))


def png_flavour(blob):
    """(width, height, bit depth, colour type, interlace) from IHDR, no decoder involved."""
    if blob[:8] != b'\x89PNG\r\n\x1a\n' or blob[12:16] != b'IHDR':
        return None
    return struct.unpack('>IIBBxxB', blob[16:29])


def ring(page, x, y, w, h, g):
    """The g-pixel frame around a rect, as four strips; None where it would leave the page."""
    ph, pw = page.shape[:2]
    if x - g < 0 or y - g < 0 or x + w + g > pw or y + h + g > ph:
        return None
    return [page[y - g:y, x - g:x + w + g], page[y + h:y + h + g, x - g:x + w + g],
            page[y:y + h, x - g:x], page[y:y + h, x + w:x + w + g]]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('original')
    ap.add_argument('repacked')
    ap.add_argument('--max', type=int, default=1024)
    ap.add_argument('--gutter', type=int, default=1)
    args = ap.parse_args()
    old, new = Win(args.original), Win(args.repacked)
    check = Checks()

    print('container')
    check(new.raw[:4] == b'FORM' and new.word(4) == len(new.raw) - 8, 'FORM size matches the file')
    check(new.end == len(new.raw), 'chunks tile the file exactly, no slack or overrun')
    check([c[0] for c in old.chunks] == [c[0] for c in new.chunks], 'same chunks in the same order')
    tpag_start, tpag_size = old.chunk(b'TPAG')
    same = [(n, s, z) for (n, s, z), (n2, s2, z2) in zip(old.chunks, new.chunks)
            if n not in (b'TPAG', b'TXTR', b'AUDO')
            and (s, z) == (s2, z2) and old.raw[s:s + z] == new.raw[s2:s2 + z2]]
    check(len(same) == len(old.chunks) - 3, 'every other chunk byte-identical at its old offset',
          f'{len(same)} of {len(old.chunks) - 3}')

    print('texture page entries (TPAG)')
    a, b = old.items(), new.items()
    check(len(a) == len(b), 'same number of entries', f'{len(a)}')
    check(old.table(b'TPAG') == new.table(b'TPAG'), 'entries did not move in the file')
    frozen = sum(all(p[i] == q[i] for i in range(11) if i not in MOVABLE) for p, q in zip(a, b))
    check(frozen == len(a), 'only source x / source y / page changed', f'{frozen} of {len(a)}')
    mask = bytearray(old.raw[tpag_start:tpag_start + tpag_size])
    other = bytearray(new.raw[tpag_start:tpag_start + tpag_size])
    for p in old.table(b'TPAG'):
        for lo, hi in ((0, 4), (20, 22)):
            mask[p - tpag_start + lo:p - tpag_start + hi] = other[p - tpag_start + lo:p - tpag_start + hi]
    check(mask == other, 'no byte of the TPAG chunk changed outside those fields')

    print('pages')
    old_pages = [np.array(Image.open(io.BytesIO(x)).convert('RGBA')) for x in old.page_blobs()]
    flavours = [png_flavour(x) for x in new.page_blobs()]
    check(all(f and f[2:] == (8, 6, 0) for f in flavours), 'every new page is 8-bit RGBA, non-interlaced',
          f'{len(flavours)} pages')
    check(all(f and f[0] <= args.max and f[1] <= args.max for f in flavours), f'no page edge over {args.max}',
          'largest %dx%d' % max((f[:2] for f in flavours if f), key=lambda s: s[0] * s[1]))
    new_pages = [np.array(Image.open(io.BytesIO(x))) for x in new.page_blobs()]
    check(all(0 <= q[10] < len(new_pages) for q in b), 'every entry points at a page that exists')

    print('pixels')
    bad_px = bad_ring = ring_seen = out_of_page = 0
    for p, q in zip(a, b):
        x, y, w, h, x2, y2 = p[0], p[1], p[2], p[3], q[0], q[1]
        src, dst = old_pages[p[10]], new_pages[q[10]]
        if y2 + h > dst.shape[0] or x2 + w > dst.shape[1]:
            out_of_page += 1
            continue
        bad_px += not np.array_equal(src[y:y + h, x:x + w], dst[y2:y2 + h, x2:x2 + w])
        before, after = ring(src, x, y, w, h, args.gutter), ring(dst, x2, y2, w, h, args.gutter)
        if before is not None:
            ring_seen += 1
            bad_ring += after is None or any(not np.array_equal(m, n) for m, n in zip(before, after))
    check(out_of_page == 0, 'every item lies inside its new page')
    check(bad_px == 0, 'every item byte-identical, old crop vs new crop', f'{len(a) - bad_px} of {len(a)}')
    check(bad_ring == 0, f'the {args.gutter}px ring around every item is identical too',
          f'{ring_seen - bad_ring} of {ring_seen}')

    print('layout')
    owners = defaultdict(set)
    for q in b:
        owners[q[10]].add((q[0], q[1], q[2], q[3]))
    clashes = 0
    for page, rects in owners.items():
        hgt, wid = new_pages[page].shape[:2]
        seen = np.zeros((hgt, wid), np.uint8)
        for x, y, w, h in rects:
            seen[y:y + h, x:x + w] += 1
        clashes += int((seen > 1).sum())
    check(clashes == 0, 'no two different items share a texel')
    old_rects = {(p[10], p[0], p[1], p[2], p[3]) for p in a}
    new_rects = {(q[10], q[0], q[1], q[2], q[3]) for q in b}
    check(len(old_rects) == len(new_rects), 'entries that shared a rect still share one',
          f'{len(new_rects)} unique')

    print('audio')
    s1, s2 = old.sounds(), new.sounds()
    check(len(s1) == len(s2) and all(m == n for m, n in zip(s1, s2)), 'every embedded sound byte-identical',
          f'{len(s1)} sounds')
    audo_old, audo_new = old.chunk(b'AUDO'), new.chunk(b'AUDO')
    shift = audo_new[0] - audo_old[0]
    check([p + shift for p in old.table(b'AUDO')] == new.table(b'AUDO'), 'sound pointers moved with the chunk',
          f'{shift:+d} bytes')

    print('stale pointers')
    lo = old.chunk(b'TXTR')[0]
    txtr_tables = set(old.table(b'TXTR')) | {old.word(e + 4) for e in old.table(b'TXTR')}
    suspects = [at for at in range(8, lo - 8, 4)
                if lo <= old.word(at) < len(old.raw) and old.word(at) in txtr_tables | set(old.table(b'AUDO'))]
    check(not suspects, 'nothing before TXTR holds the address of a page or a sound', f'{len(suspects)} hits')

    print('\nPASS - the repack is lossless' if not check.failed else f'\nFAIL - {check.failed} check(s) failed')
    sys.exit(1 if check.failed else 0)


if __name__ == '__main__':
    main()
