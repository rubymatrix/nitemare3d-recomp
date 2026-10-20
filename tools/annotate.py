#!/usr/bin/env python3
"""Annotate Ghidra decompiler output for NITE3W.EXE.

- Replaces FUN_xxxx_yyyy with names from re/names.txt ("seg:off name" per line).
- Appends /*"..."*/ after hex immediates that point at a string in the
  automatic data segment (DS = 0x1048).
"""
import re, struct, sys, os

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
exe = open(os.path.join(root, 'original/NITE3W.EXE'), 'rb').read()
DS_FILE_OFF, DS_LEN = 0x2bf80, 0x8e9e
ds = exe[DS_FILE_OFF:DS_FILE_OFF + DS_LEN]

names = {}
np = os.path.join(root, 're/names.txt')
if os.path.exists(np):
    for line in open(np):
        line = line.split('#')[0].strip()
        if not line:
            continue
        addr, name = line.split()[:2]
        seg, off = addr.split(':')
        names['FUN_%s_%s' % (seg.lower(), off.lower())] = name

def ds_string(off):
    if off < 4 or off >= DS_LEN or ds[off - 1] != 0:
        return None
    end = ds.find(b'\0', off)
    s = ds[off:end]
    if len(s) < 2 or any(c < 0x20 and c not in (9, 10, 13) or c > 0x7e for c in s):
        return None
    return s.decode('latin-1').replace('\n', '\\n').replace('*/', '*_/')[:60]

def fix_imm(m):
    v = int(m.group(0), 16)
    s = ds_string(v)
    return m.group(0) + ('/*"%s"*/' % s if s else '')

src = open(sys.argv[1]).read()
src = re.sub(r'FUN_[0-9a-f]{4}_[0-9a-f]{4}', lambda m: names.get(m.group(0), m.group(0)), src)
def fix_seg(m):
    v = int(m.group(1), 16) + (int(m.group(2), 16) if m.group(2) else 0)
    if 0x1000 <= v <= 0x1200 and v % 8 == 0:
        return 'SEG_%04x' % v
    return m.group(0)
src = re.sub(r'\(char \*\)s_\w+?_1048_([0-9a-f]{4})(?: \+ (0x[0-9a-f]+|\d+))?', fix_seg, src)
src = re.sub(r'\b0x[0-9a-f]{3,4}\b', fix_imm, src)
open(sys.argv[2], 'w').write(src)
