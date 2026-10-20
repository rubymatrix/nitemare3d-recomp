#!/usr/bin/env python3
"""Summarize annotated decomp: per function size, callees, callers, strings."""
import re, sys, collections
src = open(sys.argv[1]).read()
funcs = re.split(r'^// ==== ', src, flags=re.M)[1:]
info = {}
for f in funcs:
    head, body = f.split('\n', 1)
    name, addr = head.split(' @ ')
    calls = sorted(set(re.findall(r'\b((?:FUN_[0-9a-f]{4}_[0-9a-f]{4})|[A-Za-z_][A-Za-z0-9_]*)\s*\(', body)) - {'if','while','for','switch','return','sizeof','CONCAT22','CONCAT11','CONCAT31','CONCAT13','SUB42','SUB41','ZEXT24','SEXT24','ZEXT12','SEXT12','CARRY2','SBORROW2','POPCOUNT','SUB21'} - {name})
    strs = re.findall(r'/\*"(.*?)"\*/', body) + re.findall(r's_(\w+?)_1048_[0-9a-f]{4}', body)
    info[name] = (addr, body.count('\n'), calls, strs)
callers = collections.defaultdict(set)
for n, (a, l, c, s) in info.items():
    for x in c: callers[x].add(n)
segs = sys.argv[2].split(',') if len(sys.argv) > 2 else None
for n, (a, l, c, s) in info.items():
    if segs and a.split(':')[0] not in segs: continue
    print(f"{a} {n} L{l} <- {len(callers[n])} [{','.join(sorted(callers[n]))[:80]}]")
    if c: print("   calls:", ' '.join(c)[:300])
    if s: print("   strs:", ' | '.join(dict.fromkeys(s))[:300])
