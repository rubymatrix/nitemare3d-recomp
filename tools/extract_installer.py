#!/usr/bin/env python3
"""Extract Nitemare-3D for Windows v1.6 from its Wise installer.

Usage: extract_installer.py <win3_Nite3d3x.zip | folder with INSTALL.EXE+INSTALL.W02> [outdir]

The 1995 Wise installer stores each file as a raw deflate stream, back to back,
spanning INSTALL.EXE and INSTALL.W02. File names live in the (unparsed) Wise
script, so files are identified by content. Output:

  outdir/NITE3W.EXE         the game executable (input to the recompiler)
  outdir/data/              game data files with the names the game opens
  outdir/extra/             manual, map viewer, WinG redistributables, ...

GAME.PAL is not part of this distribution; the game only reads its last 768
bytes (a PCX palette trailer), so it is synthesized from UIF.DAT, whose PCX
screens all carry the game palette.
"""
import io, os, struct, sys, zipfile, zlib


def load_volumes(src):
    if os.path.isdir(src):
        parts = [open(os.path.join(src, n), 'rb').read() for n in ('INSTALL.EXE', 'INSTALL.W02')]
    else:
        z = zipfile.ZipFile(src)
        names = {n.upper(): n for n in z.namelist()}
        parts = [z.read(names[n]) for n in ('INSTALL.EXE', 'INSTALL.W02')]
    return b''.join(parts)


def find_first_stream(data):
    """The first file stream follows the installer's own (large) deflated DLL.
    Walk candidate offsets and return the start of the longest run of
    back-to-back streams that reaches the end of the archive."""
    best = None
    i = 0
    n = len(data)
    while i < n - 16:
        d = zlib.decompressobj(-15)
        try:
            out = d.decompress(data[i:i + 4_000_000])
        except zlib.error:
            i += 1
            continue
        if d.eof and len(out) > 4096:
            # try chaining from here
            pos, count = i, 0
            while pos < n - 8:
                d2 = zlib.decompressobj(-15)
                try:
                    d2.decompress(data[pos:])
                except zlib.error:
                    break
                if not d2.eof:
                    break
                pos = n - len(d2.unused_data)
                count += 1
            if pos >= n - 8 and count >= 20:
                return i
            i += 1
            continue
        i += 1
    return best


def streams(data, start):
    pos = start
    while pos < len(data) - 8:
        d = zlib.decompressobj(-15)
        out = d.decompress(data[pos:])
        yield pos, out
        pos = len(data) - len(d.unused_data)


def dat_entries(d):
    n = struct.unpack_from('<I', d, 2)[0] // 6
    return [struct.unpack_from('<HI', d, i * 6) for i in range(n)]


def classify(blob):
    """Return (subdir, filename) for an extracted blob, or None."""
    if blob[:2] == b'MZ':
        if b'NITE3W' in blob[:0x4000] or b'CNite3wView' in blob:
            return ('.', 'NITE3W.EXE')
        if b'MAPS.EXE' in blob or b'Nitemare-3D Map' in blob:
            return ('extra', 'MAPS.EXE')
        if b'UNWISE' in blob.upper():
            return ('extra', 'UNWISE.EXE')
        return ('extra', None)
    if blob[:4] == b'\x3f\x5f\x03\x00':
        return ('extra', 'MAPS.HLP')
    if blob[:2] == b'\x31\xbe':
        return ('extra', 'NITE3W.WRI')
    if len(blob) > 6 and blob[4:6] == b'\x11\xaf':
        return ('data', 'ENDING.FLI')
    if blob[:1] == b'\x0a' and (len(blob) - 514) % 8192 == 0 and len(blob) > 8192:
        return ('data', 'MAP.%d')
    if len(blob) > 0xbc00 and struct.unpack_from('<I', blob, 4)[0] == 0xbc00:
        return ('data', 'IMG.%d')
    if blob[:4] == b'0000' and b'TILES' in blob[:400]:
        return ('extra', 'TILES.%d.TXT')
    if blob[:4] == b'0000' and b'START' in blob[:200]:
        return ('extra', 'OBJS.%d.TXT')
    if blob.startswith(b'; Names of'):
        return ('extra', 'SETUP.INF')
    if len(blob) > 12:
        try:
            ents = dat_entries(blob)
            first = blob[ents[0][1]:ents[0][1] + 4]
            if first == b'IBK\x1a':
                return ('data', 'SND.DAT')
            if any(blob[p:p + 2] == b'\x0a\x05' for _, p in ents if 0 < p < len(blob)):
                return ('data', 'UIF.DAT')
        except Exception:
            pass
    # Encrypted instruction/scroll text (XOR with the copyright banner).
    key = b'Copyright 1992, David P Gray, Gray Design Associates'
    if b'David P Gray' in bytes(c ^ key[i % len(key)] for i, c in enumerate(blob[:64])):
        return ('data', 'NITE3D.BSF')
    return ('extra', None)


def make_game_pal(uif):
    ents = dat_entries(uif)
    for _, pos in ents:
        if uif[pos:pos + 2] == b'\x0a\x05':
            end = min(p for _, p in ents if p > pos) if any(p > pos for _, p in ents) else len(uif)
            pcx = uif[pos:end]
            if pcx[-769] == 12:
                return b'\x0c' + pcx[-768:]
    raise SystemExit('could not find a PCX palette in UIF.DAT')


GAME_FILES = {'DEMO.1', 'GAME.PAL', 'MAP.1', 'MAP.2', 'MAP.3', 'IMG.1', 'IMG.2', 'IMG.3', 'NITE3D.BSF',
              'ENDING.FLI', 'SND.DAT', 'UIF.DAT'}


def script_names(blobs):
    """File names, in archive order, from the Wise installer script."""
    import re
    for blob in blobs:
        if b'%MAINDIR%' in blob:
            seen, out = set(), []
            for m in re.finditer(rb'%(MAINDIR|SYS)%\\([A-Za-z0-9_.]+)', blob):
                n = m.group(2).decode().upper()
                if n != 'BACKUP' and n not in seen:
                    seen.add(n)
                    out.append(n)
            return out
    return []


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'original')
    data = load_volumes(sys.argv[1])
    start = find_first_stream(data)
    if start is None:
        raise SystemExit('no Wise file streams found')
    blobs = [b for _, b in streams(data, start)]
    names = script_names(blobs)
    # Align the script's install list with the streams using the executable as anchor.
    exe_idx = next(i for i, b in enumerate(blobs) if classify(b) == ('.', 'NITE3W.EXE'))
    base = exe_idx - names.index('NITE3W.EXE') if 'NITE3W.EXE' in names else None
    os.makedirs(os.path.join(out, 'data'), exist_ok=True)
    os.makedirs(os.path.join(out, 'extra'), exist_ok=True)
    have = set()
    for k, blob in enumerate(blobs):
        name = None
        if base is not None and 0 <= k - base < len(names):
            name = names[k - base].upper()
        if name in have:
            name = None
        if name is None:
            sub, guess = classify(blob)
            name = guess if guess and '%d' not in guess else 'FILE%02d.BIN' % k
        if name == 'NITE3W.EXE':
            sub = '.'
        elif name in GAME_FILES:
            sub = 'data'
        else:
            sub = 'extra'
        with open(os.path.join(out, sub, name), 'wb') as f:
            f.write(blob)
        have.add(name)
        print('%-6s %-14s %8d bytes' % (sub, name, len(blob)))
    if 'GAME.PAL' not in have and 'UIF.DAT' in have:
        pal = make_game_pal(open(os.path.join(out, 'data', 'UIF.DAT'), 'rb').read())
        with open(os.path.join(out, 'data', 'GAME.PAL'), 'wb') as f:
            f.write(pal)
        print('data   GAME.PAL       synthesized from UIF.DAT')


if __name__ == '__main__':
    main()
