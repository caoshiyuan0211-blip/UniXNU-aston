#!/usr/bin/env python3
"""gen-hakios-a7xx-render.py A6XX_XML A740_3A_HEADER OUT.hpp -- tables for hakios-metal's a7xx draw rewrite (OnePlus 12R, A740).

From Mesa 25.0.7's register XML (MIT): registers that moved (A6XX-only offset -> the A7XX one of the same name and layout), that
have no A7XX register (dropped), and those whose fields changed (re-encoded field by field, by name). From the 12R's stage 3a
stream (Turnip's a7xx submission, flattened; it drew right on the A740): the hardware state Turnip writes before the render pass,
split into device constants and pass-dependent registers. Do not edit the output."""
import os, re, struct, sys, xml.etree.ElementTree as ET
X, HDR, OUT = sys.argv[1:4]
here = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(here, 'a6xx-a7xx-regs.py')).read().split('if len(sys.argv) > 2:')[0]
sys.argv = ['x', X]; g = {}; exec(src, g)
regs, by_off, by_name, verdict = g['regs'], g['by_off'], g['by_name'], g['verdict']
xml = re.sub(r'\sxmlns="[^"]+"', '', open(X).read(), count=1); root = ET.fromstring(xml)
bitsets = {b.get('name'): b for b in root.iter('bitset')}
def fields(off, chip):
    """[(name, low, high)] of the register at OFF on CHIP"""
    def walk(node, base=0, inh=None):
        for c in node:
            if c.tag in ('reg32', 'reg64'):
                v = g['chips'](c.get('variants')) if c.get('variants') else inh or g['chips'](None)
                if base + int(c.get('offset'), 0) == off and chip in v:
                    src = bitsets.get(c.get('type')) if c.get('type') in bitsets else c
                    out = []
                    for b in src.iter('bitfield'):
                        if b.get('variants') and chip not in g['chips'](b.get('variants')): continue
                        lo = int(b.get('low', b.get('pos', 0))); hi = int(b.get('high', b.get('pos', lo)))
                        out.append((b.get('name'), lo, hi))
                    return out
            elif c.tag == 'array':
                v = g['chips'](c.get('variants')) if c.get('variants') else inh
                o, s, n = int(c.get('offset'), 0), int(c.get('stride'), 0), int(c.get('length'), 0)
                for i in range(n):
                    r = walk(c, base + o + i * s, v)
                    if r is not None: return r
            elif c.tag in ('domain', 'stripe'):
                if c.tag == 'domain' and c.get('name') != 'A6XX': continue
                r = walk(c, base, g['chips'](c.get('variants')) if c.get('variants') else inh)
                if r is not None: return r
        return None
    return walk(root) or []
# 1. moves and drops for every A6XX-only offset; re-encodes for same-offset registers whose fields differ
moves, drops, reencode = [], [], []
offs = sorted(by_off)
for off in offs:
    e = by_off[off]
    a6 = [x for x in e if 'A6XX' in x[1]]; a7 = [x for x in e if 'A7XX' in x[1]]
    if a6 and not a7:
        k, name, to, same = verdict(off)
        if k == 'move': moves.append((off, to, name))
        elif k == 'move-layout-differs': reencode.append((off, to, name))
        elif k == 'drop': drops.append((off, name))
    elif a6 and a7 and a6[0][2] != a7[0][2]:
        reencode.append((off, off, a6[0][0]))
# 2. Turnip's a7xx hardware state before the pass, from the stage 3a stream
hdr = open(HDR).read()
ib = [int(x, 16) for x in re.search(r'kA740TriIb\[\d+\] = \{(.*?)\};', hdr, re.S).group(1).replace(',', ' ').split()]
reloc = set(int(m.group(1)) for m in re.finditer(r'\{(\d+), \d+, \d+, \d+\}', re.search(r'kA740TriRelocs\[\d+\] = \{(.*?)\};', hdr, re.S).group(1)))
init, i = [], 0
while i < len(ib):
    h = ib[i]
    if h >> 28 == 4:
        n, r = h & 0x7f, (h >> 8) & 0x3ffff
        init += [(r + k, ib[i + 1 + k], i + 1 + k) for k in range(n)]; i += 1 + n
    elif h >> 28 == 7:
        n, op = h & 0x3fff, (h >> 16) & 0x7f
        if op == 101: break                                   # CP_SET_MARKER: the pass begins
        if op == 92: init += [(ib[i + k], ib[i + k + 1], i + k + 1) for k in range(1, n, 2)]
        i += 1 + n
    else: i += 1
PASS = re.compile(r'WINDOW|SCISSOR|RESOLVE|BIN_CONTROL|LRZ|BLIT_CLEAR_MODE|SP_FS_CTRL_REG0|HLSQ_VS_CNTL|HLSQ_CONTROL_5|UNKNOWN_8812|UNKNOWN_8007')
nm = lambda r: (by_off.get(r) or [('0x%x' % r,)])[0][0]
const, passdep = [], []
for r, v, d in init:
    if d in reloc: continue
    (passdep if PASS.search(nm(r)) else const).append((r, v))
const = list({r: v for r, v in const}.items())
# a7xx-only registers (no A6XX entry at that offset): those the pass setup writes go into the device block too (sysmem, no LRZ:
# constants), as do those the draw writes as 0 outside the 2D engine; hakios never writes any of them
a7only = lambda r: by_off.get(r) and not any('A6XX' in v for _, v, _ in by_off[r])
for r, v, d in init:
    if d not in reloc and PASS.search(nm(r)) and a7only(r) and not re.search(r'HLSQ_|SP_FS_', nm(r)) and r not in dict(const): const.append((r, v))   # not a program's registers
after, i, seen = {}, 0, False
while i < len(ib):
    h = ib[i]
    if h >> 28 == 4:
        n, r = h & 0x7f, (h >> 8) & 0x3ffff
        if seen: after.update({r + k: ib[i + 1 + k] for k in range(n)})
        i += 1 + n
    elif h >> 28 == 7:
        n, op = h & 0x3fff, (h >> 16) & 0x7f
        if op == 101: seen = True
        if seen and op == 92: after.update({ib[i + k]: ib[i + k + 1] for k in range(1, n, 2)})
        i += 1 + n
    else: i += 1
# every register Turnip's draw writes as 0, outside the 2D engine and its shared constants (0xab40..): hakios's own writes follow
zeros = [r for r, v in sorted(after.items()) if v == 0 and '2D' not in nm(r) and not 0xab40 <= r < 0xab80 and r not in dict(const)]
def a7reg(name):
    for o, v, l in by_name.get(name, []):
        if 'A7XX' in v: return o
    raise SystemExit('no A7XX ' + name)
mirrors = [(a7reg(a), a7reg(b), a, b) for a, b in [('PC_POLYGON_MODE', 'VPC_POLYGON_MODE2'), ('PC_RASTER_CNTL', 'PC_RASTER_CNTL_V2'),
           ('PC_MULTIVIEW_CNTL', 'VPC_MULTIVIEW_CNTL'), ('PC_PRIMITIVE_CNTL_0', 'VPC_PRIMITIVE_CNTL_0')]]
def bit(reg, field, chip='A7XX'):
    for name, lo, hi in fields(a7reg(reg), chip):
        if name == field: return lo, hi
    raise SystemExit(f'no {reg}.{field}')
zc = bit('GRAS_CL_CNTL', 'Z_CLAMP_ENABLE'); cs1 = bit('SP_FS_PREFETCH_CNTL', 'CONSTSLOTID'); cs2 = bit('SP_FS_PREFETCH_CNTL', 'CONSTSLOTID4COORD')
with open(OUT, 'w') as f:
    f.write('// GENERATED by 12r/metal/tools/gen-hakios-a7xx-render.py from Mesa 25.0.7\'s a6xx.xml (MIT) and the 12R\'s stage 3a A740 stream\n'
            '// (Turnip\'s a7xx state, which drew right on the phone). Do not edit.\n#pragma once\n#include <cstdint>\n\nnamespace hakios::metal::a7xx {\n\n')
    f.write('struct RegMove { uint32_t a6, a7; };\nstruct RegValue { uint32_t reg, value; };\n')
    f.write('struct FieldMove { uint8_t a6lo, a6hi, a7lo; };\nstruct Reencode { uint32_t a6, a7; uint8_t n; FieldMove f[24]; uint32_t lost; };\n\n')
    f.write('// A6XX-only registers and the A7XX register of the same name and field layout\nconstexpr RegMove kMoves[] = {\n')
    for a, b, n in moves: f.write(f'    {{0x{a:05x}, 0x{b:05x}}},   // {n}\n')
    f.write('};\n// A6XX-only registers with no A7XX register of that name: not written on the A740\nconstexpr uint32_t kDrops[] = {\n')
    for a, n in drops: f.write(f'    0x{a:05x},   // {n}\n')
    f.write('};\n// registers whose fields differ on A7XX: each A6XX field moved to the A7XX field of the same name; LOST: A6XX bits with none\n')
    f.write('constexpr Reencode kReencode[] = {\n')
    for a, b, n in reencode:
        f6, f7 = fields(a, 'A6XX'), {x[0]: x for x in fields(b, 'A7XX')}
        moved, lost = [], 0
        for name, lo, hi in f6:
            if name in f7 and f7[name][2] - f7[name][1] == hi - lo: moved.append((lo, hi, f7[name][1]))
            else: lost |= ((1 << (hi - lo + 1)) - 1) << lo
        if len(moved) > 24: raise SystemExit(f'{n}: {len(moved)} fields')
        f.write(f'    {{0x{a:05x}, 0x{b:05x}, {len(moved)}, {{' + ', '.join(f'{{{lo}, {hi}, {l7}}}' for lo, hi, l7 in moved) + f'}}, 0x{lost & 0xffffffff:08x}u}},   // {n}\n')
    f.write('};\n// Turnip\'s a7xx hardware state before a pass that is the same for every pass (tu6_init_hw and the sysmem setup)\nconstexpr RegValue kInitConst[] = {\n')
    for r, v in const: f.write(f'    {{0x{r:05x}, 0x{v:08x}}},   // {nm(r)}\n')
    f.write('};\n// a7xx-only registers Turnip\'s draw writes as 0 (no FSR, no aliased components, no dither, VGPR config 0, ...)\nconstexpr uint32_t kDrawZeros[] = {\n')
    for r in zeros: f.write(f'    0x{r:05x},   // {nm(r)}\n')
    f.write('};\n// every register Turnip\'s a7xx stream writes (a hakios device constant outside it is not written on the A740)\nconstexpr uint32_t kTurnipRegs[] = {')
    every = sorted(set(r for r, v, d in init) | set(after))
    f.write(', '.join(f'0x{r:05x}' for r in every))
    f.write('};\n// a7xx registers that copy another (Turnip writes both with one value: tu_pipeline.cc, tu_cmd_buffer.cc)\nconstexpr RegMove kMirrors[] = {\n')
    for a, b, na, nb in mirrors: f.write(f'    {{0x{a:05x}, 0x{b:05x}}},   // {na} -> {nb}\n')
    f.write('};\n')
    f.write(f'constexpr uint32_t kRegGrasClCntl = 0x{a7reg("GRAS_CL_CNTL"):05x}, kGrasClZClampEnable = 1u << {zc[0]};   // a7xx: always (tu_pipeline.cc)\n')
    f.write(f'constexpr uint32_t kRegSpFsPrefetchCntl = 0x{a7reg("SP_FS_PREFETCH_CNTL"):05x};\n')
    f.write(f'constexpr uint32_t kPrefetchConstSlotsNone = (0x1ffu << {cs1[0]}) | (0x1ffu << {cs2[0]});   // CONSTSLOTID, CONSTSLOTID4COORD 0x1ff (fd6_program.cc, A7XX)\n')
    f.write(f'constexpr uint32_t kRegRbCcuCntl = 0x{a7reg("RB_CCU_CNTL"):05x}, kRegRbBinControl = 0x{a7reg("RB_BIN_CONTROL"):05x}, kRegGrasBinControl = 0x{a7reg("GRAS_BIN_CONTROL"):05x};\n')
    f.write(f'constexpr uint32_t kRegHlsqControl5 = 0x{a7reg("HLSQ_CONTROL_5_REG"):05x}, kRegHlsqControl2 = 0x{a7reg("HLSQ_CONTROL_2_REG"):05x}, kRegHlsqUnknownA9ae = 0x{a7reg("HLSQ_UNKNOWN_A9AE"):05x};\n')
    f.write(f'constexpr uint32_t kRegA6xxVpcUnknown9107 = 0x09107;   // A6XX VPC_UNKNOWN_9107: A7XX PC_RASTER_CNTL sits there\n')
    f.write(f'constexpr uint32_t kBinControlSysmem = 0x{after.get(a7reg("RB_BIN_CONTROL"), dict(init and [(r, v) for r, v, d in init]).get(a7reg("RB_BIN_CONTROL"), 0)):08x};   // Turnip\'s sysmem pass\n')
    f.write('\n}  // namespace hakios::metal::a7xx\n')
print(f'{OUT}: {len(zeros)} draw zeros, {len(mirrors)} mirrors, zclamp bit {zc[0]}; {len(moves)} moves, {len(drops)} drops, {len(reencode)} re-encodes, {len(const)} device constants ({len(passdep)} pass-dependent left out: {", ".join(nm(r) for r, v in passdep)})')
