#!/usr/bin/env python3
"""Turn repack_textures.py's --report into the plan the console repacker follows.

  tools/repack_plan.py ORIGINAL.win report.json plan.bin

Little-endian: 'AM2RPLAN', u32 crc32 of the original data.win's TPAG chunk (the console refuses a
plan made for another file), u32 pages, u32 items, then pages × (u16 w, u16 h) and, for every TPAG
entry in order, u16 page, u16 x, u16 y (x, y already include the 1-pixel gutter).
"""
import json
import struct
import sys
import zlib


def main():
    src, report, dst = sys.argv[1:4]
    data = open(src, 'rb').read()
    off = 8
    chunks = {}
    while off < len(data):
        name = data[off:off + 4].decode('ascii')
        size = struct.unpack_from('<I', data, off + 4)[0]
        chunks[name] = (off + 8, size)
        off += 8 + size
    toff, tsize = chunks['TPAG']
    rep = json.load(open(report))
    where = {tuple(int(v) for v in k.split(',')): tuple(v) for k, v in rep['items'].items()}
    n = struct.unpack_from('<I', data, toff)[0]
    out = bytearray(b'AM2RPLAN')
    out += struct.pack('<III', zlib.crc32(data[toff:toff + tsize]), len(rep['pages']), n)
    for w, h in rep['pages']:
        out += struct.pack('<HH', w, h)
    for i in range(n):
        p = struct.unpack_from('<I', data, toff + 4 + 4 * i)[0]
        x, y, w, h = struct.unpack_from('<4H', data, p)
        page = struct.unpack_from('<h', data, p + 20)[0]
        dpage, dx, dy = where[(page, x, y, w, h)]
        out += struct.pack('<HHH', dpage, dx, dy)
    open(dst, 'wb').write(out)
    print(f'{n} items, {len(rep["pages"])} pages, {len(out)} bytes -> {dst}')


if __name__ == '__main__':
    main()
