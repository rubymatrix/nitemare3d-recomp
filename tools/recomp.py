#!/usr/bin/env python3
"""Static recompiler: Ghidra raw p-code (x86-16 protected mode) -> portable C.

Input : recomp/export/{functions,lang,memory}.json  (from ExportRecomp.java)
Output: src/gen/rc_<seg>.c, src/gen/rc_funcs.h, src/gen/rc_table.c

Each Ghidra function becomes `void f_SSSS_OOOO(void)` operating on the global
CPU state `R` (register file indexed by Ghidra register offsets) and the
segmented memory model in the runtime (see src/rt/rt.h). Every instruction is
translated from its raw p-code, so flag semantics are exact.

Hand-written replacements for any function can be registered in
src/rt/overrides.c; the generator emits calls through `RC_CALL(addr)` which
resolve to the override when one exists (see rc_table.c / overrides list).
"""
import json, os, sys, collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXP = os.path.join(ROOT, 'recomp', 'export')
OUT = os.path.join(ROOT, 'src', 'gen')

lang = json.load(open(os.path.join(EXP, 'lang.json')))
USEROPS = lang['userops']
REGNAME = {}
for n, (o, s) in lang['registers'].items():
    REGNAME.setdefault((o, s), n)
memory = json.load(open(os.path.join(EXP, 'memory.json')))
EXTERNAL = next(b for b in memory if b['name'] == 'EXTERNAL')
EXT_LO, EXT_HI = EXTERNAL['start'], EXTERNAL['start'] + EXTERNAL['size']

funcs = json.load(open(os.path.join(EXP, 'functions.json')))

# Imported KERNEL constants: Ghidra leaves the EXTERNAL-block offset as the
# immediate; substitute the value Windows would have patched in.
CONST_PLACEHOLDER = {'__AHSHIFT': 0x98, '__AHINCR': 0x9c, '__WINFLAGS': 0xb4, '__A000H': 0xbc}
CONST_VALUE = {'__AHSHIFT': 'RT_AHSHIFT', '__AHINCR': 'RT_AHINCR', '__WINFLAGS': 'RT_WINFLAGS', '__A000H': 'RT_SEL_A000'}
CONST_SITES = {}
_cr = os.path.join(EXP, 'const_relocs.json')
if os.path.exists(_cr):
    for r in json.load(open(_cr)):
        CONST_SITES[r['addr']] = r['name']
CODE_SEGS = {0x1000, 0x1008, 0x1010, 0x1018}
funcs = [f for f in funcs if (f['entry'] >> 16) in CODE_SEGS]
funcs.sort(key=lambda f: f['entry'])
ENTRIES = {f['entry']: f for f in funcs}

# Map every instruction address to its owning function(s) for cross-function jumps.
INSN_OWNER = {}
for f in funcs:
    for i in f['insns']:
        INSN_OWNER.setdefault(i['a'], f['entry'])

# Jumps/branches into the middle of another function: emit an alternate entry
# (a copy of the owning function that starts at the target label).
ALT_ENTRIES = collections.defaultdict(set)   # owner entry -> {target}
for f in funcs:
    own = {i['a'] for i in f['insns']}
    for i in f['insns']:
        for p in i['p']:
            if p[0] in ('BRANCH', 'CBRANCH') and p[2][0] == 'ram':
                t = p[2][1]
            elif p[0] == 'CALL' and p[2][0] == 'ram':
                t = p[2][1]
            else:
                continue
            if t in own or t in ENTRIES:
                continue
            owner = INSN_OWNER.get(t)
            if owner is not None:
                ALT_ENTRIES[owner].add(t)

def alt_name(owner, t):
    return 'f_%04x_%04x_at_%04x' % (owner >> 16, owner & 0xffff, t & 0xffff)

overrides_path = os.path.join(ROOT, 'src', 'rt', 'overrides.txt')
OVERRIDES = set()
WRAPPED = set()   # overrides that still get the original body, as f_SSSS_OOOO_orig
if os.path.exists(overrides_path):
    for line in open(overrides_path):
        toks = line.split('#')[0].split()
        if toks:
            seg, off = toks[0].split(':')
            a = (int(seg, 16) << 16) | int(off, 16)
            OVERRIDES.add(a)
            if 'wrap' in toks[1:]:
                WRAPPED.add(a)

def fname(a):
    return 'f_%04x_%04x' % (a >> 16, a & 0xffff)

def cty(size):
    return {1: 'uint8_t', 2: 'uint16_t', 4: 'uint32_t', 8: 'uint64_t'}.get(size, 'uint64_t')

MASK = {1: '0xffu', 2: '0xffffu', 4: '0xffffffffu', 8: '0xffffffffffffffffull'}

warnings = collections.Counter()

class FnGen:
    def __init__(self, f):
        self.f = f
        self.entry = f['entry']
        self.addrs = {i['a'] for i in f['insns']}
        self.uniques = {}
        self.lines = []
        self.labels_used = set()
        self.extra_entries = set()

    # ---- varnodes -------------------------------------------------------
    def rd(self, v):
        sp, off, size = v
        if sp == 'const':
            if size == 2 and off in getattr(self, 'cur_consts', {}):
                return '((uint16_t)%s)' % self.cur_consts[off]
            return '((%s)0x%xull)' % (cty(size), off & ((1 << (8 * size)) - 1) if size <= 8 else off)
        if sp == 'register':
            return 'RG%d(0x%x)' % (size * 8, off) if size in (1, 2, 4, 8) else self.bad('regsize %d' % size)
        if sp == 'unique':
            return '((%s)%s)' % (cty(size), self.uvar(off, size))
        if sp == 'ram':
            return 'MEMR%d(0x%xu)' % (size * 8, off)
        return self.bad('space ' + sp)

    def wr(self, v, expr):
        sp, off, size = v
        if sp == 'register':
            return 'RG%d(0x%x) = (%s)(%s);' % (size * 8, off, cty(size), expr)
        if sp == 'unique':
            return '%s = (%s)(%s);' % (self.uvar(off, size), cty(size), expr)
        if sp == 'ram':
            return 'MEMW%d(0x%xu, %s);' % (size * 8, off, expr)
        return self.bad('write space ' + sp)

    def uvar(self, off, size):
        # Unique space is byte addressed; Ghidra may write N bytes and read fewer
        # (little-endian low part) at the same offset. Model each offset as a
        # zero-extended 64-bit slot and truncate on read.
        name = 'u%x' % off
        self.uniques[name] = 8
        return name

    def bad(self, what):
        warnings[what] += 1
        return '0 /* UNSUPPORTED %s */' % what

    # ---- flow targets ---------------------------------------------------
    def label(self, a, idx=0):
        return 'L_%x_%d' % (a & 0xffff, idx)

    def goto_ram(self, target):
        if target in self.addrs:
            self.labels_used.add((target, 0))
            return 'goto %s;' % self.label(target)
        return self.tailcall(target)

    def tailcall(self, target):
        if target in ENTRIES:
            return '{ %s(); return; }' % call_name(target)
        owner = INSN_OWNER.get(target)
        if owner is not None:
            return '{ %s(); return; }' % alt_name(owner, target)
        warnings['jump into non-entry %x' % target] += 1
        return '{ rt_jump_unknown(0x%xu); return; }' % target

    # ---- main -----------------------------------------------------------
    def gen(self):
        out = self.lines
        insns = self.f['insns']
        for i in insns:
            a = i['a']
            out.append('%s: /* %04x:%04x  %s */' % (self.label(a), a >> 16, a & 0xffff, i['t'].replace('*/', '* /')))
            ops = i['p']
            if i['t'].startswith('XLAT'):
                # Ghidra's 16-bit XLAT p-code drops the segment; AL = [seg:BX+AL].
                seg = {'ES': 0x100, 'CS': 0x102, 'SS': 0x104}.get(i['t'].split()[-1].split(':')[0], 0x106)
                out.append('  RG8(0x0) = MEMR8(SEGLIN(RG16(0x%x), (uint16_t)(RG16(0xc) + RG8(0x0))));' % seg)
                ops = []
            needs_idx = any(o[0] in ('BRANCH', 'CBRANCH') and o[2][0] == 'const' for o in ops)
            for idx, op in enumerate(ops):
                if needs_idx and idx > 0:
                    out.append('%s:;' % self.label(a, idx))
                out.append('  ' + self.op(i, idx, op))
            if needs_idx:
                out.append('%s:;' % self.label(a, len(ops)))
            # Fall-through into the next instruction when it is not contiguous.
            nxt = i['ft']
            last = ops[-1][0] if ops else None
            if nxt is not None and last not in ('BRANCH', 'RETURN', 'BRANCHIND'):
                pos = insns.index(i)
                if pos + 1 >= len(insns) or insns[pos + 1]['a'] != nxt:
                    out.append('  ' + self.goto_ram(nxt))
            if nxt is None and last not in ('BRANCH', 'RETURN', 'BRANCHIND', 'CALLIND', 'CALL'):
                pass
        return out

    def op(self, ins, idx, p):
        m, outv, ins_ = p[0], p[1], p[2:]
        a = ins['a']
        self.cur_consts = {}
        for site in range(a, a + ins['n']):
            if site in CONST_SITES:
                nm = CONST_SITES[site]
                self.cur_consts[CONST_PLACEHOLDER[nm]] = CONST_VALUE[nm]
        R = self.rd
        def sz(v): return v[2]
        def S(v):  # signed read
            return 'SX%d(%s)' % (sz(v) * 8, R(v))
        if m == 'COPY':
            return self.wr(outv, R(ins_[0]))
        if m == 'LOAD':
            size = outv[2]
            return self.wr(outv, 'MEMR%d(%s)' % (size * 8, R(ins_[1])))
        if m == 'STORE':
            size = ins_[2][2]
            return 'MEMW%d(%s, %s);' % (size * 8, R(ins_[1]), R(ins_[2]))
        bin_ops = {
            'INT_ADD': '+', 'INT_SUB': '-', 'INT_MULT': '*', 'INT_AND': '&', 'INT_OR': '|', 'INT_XOR': '^',
            'BOOL_AND': '&', 'BOOL_OR': '|', 'BOOL_XOR': '^',
        }
        if m in bin_ops:
            return self.wr(outv, '(uint64_t)%s %s %s' % (R(ins_[0]), bin_ops[m], R(ins_[1])))
        if m == 'INT_EQUAL':
            return self.wr(outv, '%s == %s' % (R(ins_[0]), R(ins_[1])))
        if m == 'INT_NOTEQUAL':
            return self.wr(outv, '%s != %s' % (R(ins_[0]), R(ins_[1])))
        if m == 'INT_LESS':
            return self.wr(outv, '%s < %s' % (R(ins_[0]), R(ins_[1])))
        if m == 'INT_LESSEQUAL':
            return self.wr(outv, '%s <= %s' % (R(ins_[0]), R(ins_[1])))
        if m == 'INT_SLESS':
            return self.wr(outv, '%s < %s' % (S(ins_[0]), S(ins_[1])))
        if m == 'INT_SLESSEQUAL':
            return self.wr(outv, '%s <= %s' % (S(ins_[0]), S(ins_[1])))
        if m == 'INT_CARRY':
            return self.wr(outv, 'CARRY%d(%s, %s)' % (sz(ins_[0]) * 8, R(ins_[0]), R(ins_[1])))
        if m == 'INT_SCARRY':
            return self.wr(outv, 'SCARRY%d(%s, %s)' % (sz(ins_[0]) * 8, R(ins_[0]), R(ins_[1])))
        if m == 'INT_SBORROW':
            return self.wr(outv, 'SBORROW%d(%s, %s)' % (sz(ins_[0]) * 8, R(ins_[0]), R(ins_[1])))
        if m == 'INT_2COMP':
            return self.wr(outv, '-(uint64_t)%s' % R(ins_[0]))
        if m == 'INT_NEGATE':
            return self.wr(outv, '~(uint64_t)%s' % R(ins_[0]))
        if m == 'BOOL_NEGATE':
            return self.wr(outv, '!%s' % R(ins_[0]))
        if m == 'INT_ZEXT':
            return self.wr(outv, R(ins_[0]))
        if m == 'INT_SEXT':
            return self.wr(outv, '(int64_t)%s' % S(ins_[0]))
        if m == 'INT_LEFT':
            return self.wr(outv, 'SHL%d(%s, %s)' % (sz(outv) * 8, R(ins_[0]), R(ins_[1])))
        if m == 'INT_RIGHT':
            return self.wr(outv, 'SHR%d(%s, %s)' % (sz(outv) * 8, R(ins_[0]), R(ins_[1])))
        if m == 'INT_SRIGHT':
            return self.wr(outv, 'SAR%d(%s, %s)' % (sz(outv) * 8, R(ins_[0]), R(ins_[1])))
        if m == 'INT_DIV':
            return self.wr(outv, 'DIVU(%s, %s)' % (R(ins_[0]), R(ins_[1])))
        if m == 'INT_REM':
            return self.wr(outv, 'REMU(%s, %s)' % (R(ins_[0]), R(ins_[1])))
        if m == 'INT_SDIV':
            return self.wr(outv, 'DIVS(%s, %s)' % (S(ins_[0]), S(ins_[1])))
        if m == 'INT_SREM':
            return self.wr(outv, 'REMS(%s, %s)' % (S(ins_[0]), S(ins_[1])))
        if m == 'POPCOUNT':
            return self.wr(outv, 'POPCNT(%s)' % R(ins_[0]))
        if m == 'SUBPIECE':
            return self.wr(outv, '(%s >> %d)' % (R(ins_[0]), 8 * ins_[1][1]))
        if m == 'PIECE':
            return self.wr(outv, '((uint64_t)%s << %d) | %s' % (R(ins_[0]), 8 * sz(ins_[1]), R(ins_[1])))
        if m == 'CALLOTHER':
            name = USEROPS[ins_[0][1]]
            args = [R(v) for v in ins_[1:]]
            if name == 'segment':
                return self.wr(outv, 'SEGLIN(%s, %s)' % tuple(args))
            if name in ('LOCK', 'UNLOCK'):
                return ';'
            if name == 'in':
                return self.wr(outv, 'rt_in%d(%s)' % (sz(outv) * 8, args[0]))
            if name == 'out':
                return 'rt_out%d(%s, %s);' % (sz(ins_[2]) * 8, args[0], args[1])
            if name == 'swi':
                return 'rt_int(%s);' % args[0]
            return self.bad('userop ' + name)
        if m == 'BRANCH':
            t = ins_[0]
            if t[0] == 'const':
                rel = t[1] if t[1] < (1 << 31) else t[1] - (1 << 64)
                self.labels_used.add((a, idx + rel))
                return 'goto %s;' % self.label(a, idx + rel)
            return self.goto_ram(t[1])
        if m == 'CBRANCH':
            t, cond = ins_[0], ins_[1]
            if t[0] == 'const':
                rel = t[1] if t[1] < (1 << 31) else t[1] - (1 << 64)
                self.labels_used.add((a, idx + rel))
                return 'if (%s) goto %s;' % (R(cond), self.label(a, idx + rel))
            return 'if (%s) %s' % (R(cond), self.goto_ram(t[1]))
        if m == 'CALL':
            t = ins_[0][1]
            if EXT_LO <= t < EXT_HI:
                return 'rt_api(0x%xu);' % t
            if t in ENTRIES:
                return '%s();' % call_name(t)
            if INSN_OWNER.get(t) is not None:
                return '%s();' % alt_name(INSN_OWNER[t], t)
            warnings['call non-entry %x' % t] += 1
            return 'rt_call_unknown(0x%xu);' % t
        if m == 'CALLIND':
            if ins['t'].startswith('INT '):
                return ';  /* INT handled by swi */'
            t = ins_[0]
            if t[2] == 4 and not (t[0] == 'register' and t[1] == 644):
                return 'rt_call_ind(%s);' % self.rd(t)   # far call: already linear
            return 'rt_call_ind(%s);' % self.linear_target(t)
        if m == 'BRANCHIND':
            targets = [r for r in ins['refs'] if r in self.addrs]
            cases = ' '.join('case 0x%xu: goto %s;' % (t, self.label(t)) for t in targets)
            for t in targets:
                self.labels_used.add((t, 0))
            return 'switch (%s) { %s default: rt_jump_ind(%s); return; }' % (
                self.linear_target(ins_[0]), cases, self.linear_target(ins_[0]))
        if m == 'RETURN':
            return 'return;'
        return self.bad('op ' + m)

    def linear_target(self, v):
        # Indirect targets are IP-style offsets; combine with CS to get linear address.
        if v[2] == 4 and v[0] == 'register' and v[1] == 644:
            return '(((uint32_t)RG16(0x102) << 16) | RG16(0x284))'
        if v[2] == 2:
            return '(((uint32_t)RG16(0x102) << 16) | %s)' % self.rd(v)
        return '(((uint32_t)RG16(0x102) << 16) | (%s & 0xffffu))' % self.rd(v)

    def emit(self, w, primary=None):
        body = self.gen()
        variants = [(primary or fname(self.entry), None)] + [(alt_name(self.entry, t), t) for t in sorted(ALT_ENTRIES.get(self.entry, ()))]
        for name, start in variants:
            w.write('void %s(void) {\n' % name)
            for n, s in sorted(self.uniques.items()):
                w.write('  %s %s = 0;\n' % (cty(s), n))
            w.write('  RT_ENTER(0x%xu);\n' % (start or self.entry))
            if start is not None:
                w.write('  goto %s;\n' % self.label(start))
            for line in body:
                w.write(line + '\n')
            w.write('  return;\n}\n\n')


def call_name(t):
    return fname(t)


def main():
    os.makedirs(OUT, exist_ok=True)
    by_seg = collections.defaultdict(list)
    for f in funcs:
        by_seg[f['entry'] >> 16].append(f)
    with open(os.path.join(OUT, 'rc_funcs.h'), 'w') as h:
        h.write('/* Generated by tools/recomp.py - do not edit. */\n#pragma once\n')
        for f in funcs:
            h.write('void %s(void); /* %s */\n' % (fname(f['entry']), f['name']))
            if f['entry'] in WRAPPED:
                h.write('void %s_orig(void);\n' % fname(f['entry']))
            for t in sorted(ALT_ENTRIES.get(f['entry'], ())):
                h.write('void %s(void);\n' % alt_name(f['entry'], t))
        h.write('\ntypedef struct { uint32_t addr; void (*fn)(void); const char *name; } rc_func_t;\n')
        h.write('extern const rc_func_t rc_func_table[];\nextern const int rc_func_count;\n')
    for seg, fl in by_seg.items():
        path = os.path.join(OUT, 'rc_%04x.c' % seg)
        with open(path, 'w') as w:
            w.write('/* Generated by tools/recomp.py from NITE3W.EXE segment %04x - do not edit. */\n' % seg)
            w.write('#include "rt/rt.h"\n#include "gen/rc_funcs.h"\n\n')
            w.write('#if defined(__GNUC__)\n#pragma GCC diagnostic ignored "-Wunused-label"\n'
                    '#pragma GCC diagnostic ignored "-Wunused-but-set-variable"\n'
                    '#pragma GCC diagnostic ignored "-Wunused-variable"\n#endif\n\n')
            for f in fl:
                if f['entry'] in WRAPPED:
                    w.write('/* %s: overridden in src/rt; original body follows */\n' % fname(f['entry']))
                    FnGen(f).emit(w, fname(f['entry']) + '_orig')
                    continue
                if f['entry'] in OVERRIDES:
                    if ALT_ENTRIES.get(f['entry']):
                        sys.exit('error: %s has alternate entry points; use "wrap"' % fname(f['entry']))
                    w.write('/* %s: overridden in src/rt */\n' % fname(f['entry']))
                    continue
                FnGen(f).emit(w)
    with open(os.path.join(OUT, 'rc_table.c'), 'w') as w:
        w.write('/* Generated by tools/recomp.py - do not edit. */\n#include "rt/rt.h"\n#include "gen/rc_funcs.h"\n\n')
        w.write('const rc_func_t rc_func_table[] = {\n')
        for f in funcs:
            w.write('  {0x%xu, %s, "%s"},\n' % (f['entry'], fname(f['entry']), f['name']))
        w.write('};\nconst int rc_func_count = %d;\n' % len(funcs))
    for k, v in sorted(warnings.items(), key=lambda x: -x[1]):
        print('warning: %s (x%d)' % (k, v), file=sys.stderr)
    print('generated %d functions' % len(funcs))


if __name__ == '__main__':
    main()
