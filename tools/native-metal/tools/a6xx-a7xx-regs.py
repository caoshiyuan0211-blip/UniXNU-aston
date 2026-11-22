#!/usr/bin/env python3
"""a6xx-a7xx-regs.py A6XX_XML [HAKIOS_DUMP.bin] -- from Mesa's register XML: every A6XX register's fate on A7XX.
keep: the same offset is valid on A7XX; move: A6XX-only, the same name has an A7XX offset (same field layout, or flagged);
drop: A6XX-only with no A7XX register of that name. With a hakios dump: the verdict for each register it writes."""
import re, struct, sys, xml.etree.ElementTree as ET
xml = re.sub(r'\sxmlns="[^"]+"', '', open(sys.argv[1]).read(), count=1)
root = ET.fromstring(xml)
def chips(v):
    if v is None: return {'A6XX', 'A7XX'}
    out = set()
    for part in v.split(','):
        part = part.strip()
        if part.endswith('-'): out |= {'A6XX', 'A7XX'} if part[:-1] <= 'A6XX' else ({'A7XX'} if part[:-1] == 'A7XX' else set())
        elif part: out.add(part)
    return out
regs = []   # (offset, name, chips, layout key)
def walk(node, base=0, prefix='', inherited=None):
    for c in node:
        if c.tag in ('reg32', 'reg64'):
            v = chips(c.get('variants')) if c.get('variants') else inherited or chips(None)
            layout = c.get('type') or ''.join(ET.tostring(b).decode() for b in c if b.tag == 'bitfield')
            regs.append((base + int(c.get('offset'), 0), prefix + c.get('name'), v, layout, c.tag))
        elif c.tag == 'array':
            v = chips(c.get('variants')) if c.get('variants') else inherited
            off, stride, n = int(c.get('offset'), 0), int(c.get('stride'), 0), int(c.get('length'), 0)
            for i in range(n): walk(c, base + off + i * stride, prefix + c.get('name', '') + f'[{i}].', v)
        elif c.tag in ('domain', 'stripe'):
            if c.tag == 'domain' and c.get('name') != 'A6XX': continue
            walk(c, base, prefix, chips(c.get('variants')) if c.get('variants') else inherited)
walk(root)
by_off, by_name = {}, {}
for off, name, v, lay, tag in regs:
    words = 2 if tag == 'reg64' else 1
    for k in range(words):
        by_off.setdefault(off + k, []).append((name + ('.hi' if k else ''), v, lay))
    by_name.setdefault(name, []).append((off, v, lay))
def verdict(off):
    e = by_off.get(off)
    if not e: return ('unknown', '', None, None)
    if any('A7XX' in v for _, v, _ in e): return ('keep', e[0][0], None, None)
    name, _, lay = e[0]
    base = name[:-3] if name.endswith('.hi') else name
    for o7, v7, l7 in by_name.get(base, []):
        if 'A7XX' in v7:
            return ('move' if l7 == lay else 'move-layout-differs', name, o7 + (1 if name.endswith('.hi') else 0), l7 == lay)
    return ('drop', name, None, None)
if len(sys.argv) > 2:
    data = open(sys.argv[2], 'rb').read(); w = struct.unpack('<%dI' % (len(data) // 4), data)
    written, i = {}, 0
    while i < len(w):
        h = w[i]
        if h >> 28 == 4:
            n, r = h & 0x7f, (h >> 8) & 0x3ffff
            for k in range(n): written[r + k] = w[i + 1 + k]
            i += 1 + n
        elif h >> 28 == 7: i += 1 + (h & 0x3fff)
        else: i += 1
    cls = {}
    for r in sorted(written): cls.setdefault(verdict(r)[0], []).append(r)
    for k in ('move', 'move-layout-differs', 'drop', 'unknown'):
        print(f'== {k} ({len(cls.get(k, []))})')
        for r in cls.get(k, []):
            vd = verdict(r); print(f'  {r:05x} {vd[1]:40s} -> {vd[2] if vd[2] is None else hex(vd[2])}  value {written[r]:08x}')
    print(f'== keep ({len(cls.get("keep", []))})')
