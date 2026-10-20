#!/usr/bin/env python3
"""List NE relocation sites that import KERNEL constants (__AHINCR etc.).

Writes recomp/export/const_relocs.json: [{"addr": linear, "ord": n, "name": s}]
where addr is the linear address (sel<<16|off) of the patched 16-bit word.
"""
import struct, json, os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
d = open(os.path.join(ROOT, 'original', 'NITE3W.EXE'), 'rb').read()
ne = struct.unpack_from('<I', d, 0x3c)[0]
nseg = struct.unpack_from('<H', d, ne + 0x1c)[0]
segtab = struct.unpack_from('<H', d, ne + 0x22)[0] + ne
align = struct.unpack_from('<H', d, ne + 0x32)[0]
modtab = struct.unpack_from('<H', d, ne + 0x28)[0] + ne
imptab = struct.unpack_from('<H', d, ne + 0x2a)[0] + ne
nmod = struct.unpack_from('<H', d, ne + 0x1e)[0]
mods = []
for i in range(nmod):
    o = struct.unpack_from('<H', d, modtab + i * 2)[0]
    l = d[imptab + o]
    mods.append(d[imptab + o + 1:imptab + o + 1 + l].decode())
NAMES = {113: '__AHSHIFT', 114: '__AHINCR', 174: '__A000H', 178: '__WINFLAGS', 193: '__0000H',
         194: '__0040H', 173: '__ROMBIOS', 179: '__D000H', 181: '__F000H', 182: '__E000H', 190: '__E000H',
         183: '__C000H', 175: '__B000H', 176: '__B800H'}
out = []
for si in range(nseg):
    off, ln, fl, mn = struct.unpack_from('<HHHH', d, segtab + si * 8)
    if not fl & 0x100 or off == 0:
        continue
    base = off << align
    size = ln or 0x10000
    seg = bytearray(d[base:base + size])
    rel = base + size
    n = struct.unpack_from('<H', d, rel)[0]
    for k in range(n):
        st, ft, o, a, b = struct.unpack_from('<BBHHH', d, rel + 2 + k * 8)
        if (ft & 3) == 1 and mods[a - 1] == 'KERNEL' and b in NAMES:
            name = NAMES[b]
        elif (ft & 3) == 2 and mods[a - 1] == 'KERNEL':
            l = d[imptab + b]
            name = d[imptab + b + 1:imptab + b + 1 + l].decode()
            if not name.startswith('__'):
                continue
        else:
            continue
        additive = bool(ft & 4)
        sites = []
        while True:
            sites.append(o)
            if additive:
                break
            nxt = struct.unpack_from('<H', seg, o)[0]
            if nxt == 0xffff or nxt >= size:
                break
            o = nxt
        sel = 0x1000 + si * 8
        for s in sites:
            out.append({'addr': (sel << 16) | s, 'ord': b, 'name': name, 'src_type': st, 'additive': additive})
json.dump(out, open(os.path.join(ROOT, 'recomp', 'export', 'const_relocs.json'), 'w'), indent=1)
for r in out:
    print('%04x:%04x %s type=%d%s' % (r['addr'] >> 16, r['addr'] & 0xffff, r['name'], r['src_type'], ' additive' if r['additive'] else ''))
