"""Exploration helpers for Nitemare-3D data files."""
import struct, os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, 'original', 'data')

def rd(name):
    return open(os.path.join(DATA, name), 'rb').read()

def dat_entries(d):
    n = struct.unpack_from('<I', d, 2)[0] // 6
    return [struct.unpack_from('<HI', d, i * 6) for i in range(n)]

def dat_get(d, i):
    ln, pos = dat_entries(d)[i]
    # length field is 16-bit; use next entry offset for real size
    ents = dat_entries(d)
    end = ents[i + 1][1] if i + 1 < len(ents) else len(d)
    return d[pos:end]

def pcx_decode(p):
    w = struct.unpack_from('<H', p, 8)[0] - struct.unpack_from('<H', p, 4)[0] + 1
    h = struct.unpack_from('<H', p, 10)[0] - struct.unpack_from('<H', p, 6)[0] + 1
    bpl = struct.unpack_from('<H', p, 66)[0]
    out = bytearray(); i = 128
    while len(out) < bpl * h:
        c = p[i]; i += 1
        if c >= 0xC0:
            out += bytes([p[i]]) * (c & 0x3F); i += 1
        else:
            out.append(c)
    pix = b''.join(out[y * bpl:y * bpl + w] for y in range(h))
    pal = p[-768:] if p[-769] == 12 else None
    return w, h, bytes(pix), pal

def img_records(d, start=0xBC00):
    recs = []; i = start
    while i + 10 <= len(d):
        w, h = d[i], d[i + 1]
        meta = d[i + 2:i + 10]
        if i + 10 + w * h > len(d): break
        recs.append((i, w, h, meta, d[i + 10:i + 10 + w * h]))
        i += 10 + w * h
    return recs
