#!/usr/bin/env python3
"""gen-a740-tri.py: native Metal stage 2a for the OnePlus 12R's Adreno 740 - one flat command stream that clears a 64x64 RGBA8 LINEAR
target to blue and draws one orange triangle, with our own buffers, built from Turnip's own a7xx submission for the same smoke test
(UniXNU NativeTurnipSmoke: VS positions (-0.5,-0.5) (0.5,-0.5) (0,0.5) by gl_VertexIndex from a constant buffer, FS colour from
push constants (1, 0.5, 0, 1), clear (0, 0, 1, 1)) as recorded on UniXNU's host GPU simulation (12r/metal/capture/*submit00002.rd).

Stage 2a keeps Turnip's recorded register VALUES (they are a known-good a7xx draw: the same submission draws this triangle on the
phone through Turnip); what is ours is the stream itself and its buffers. It is flattened for one sysmem pass on the BR thread:
  - CP_COND_REG_EXEC RENDER_MODE blocks: the SYSMEM ones inline, the GMEM ones dropped; THREAD_MODE BR blocks inline
  - CP_SET_DRAW_STATE groups: the ones enabled for SYSMEM inlined, in the recorded order, right where Turnip set them
  - dropped: context-switch preambles (CP_SET_AMBLE, CP_BV_BR_COUNT_OPS, CP_REG_TEST), BV/BOTH thread switches (all BR),
    Turnip's trailing timestamp, the final UBWC->linear copy blit
  - the colour target is linear instead of UBWC-tiled: RB_MRT[0] and the 2D clear destination lose tile mode and flag buffers
  - the end is ours: CCU/cache flushes, then RB_DONE_TS writes seqno 1 to a control buffer, the CP waits for it, CACHE_FLUSH7
Every GPU address is a relocation onto our buffers (program, target, border colours, control).

usage: gen-a740-tri.py OUT.h      (reads the capture next to this script)
"""
import glob
import importlib.util
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
HDR = Path.home() / '12r-unixnu/ext/mesa-host-build/src/freedreno/registers/adreno'
spec = importlib.util.spec_from_file_location('rd', HERE / 'rd-decode.py')
rd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rd)

BASE = 0x100000000                      # the capture's buffer base (host GPU range)
PROG_VS, PROG_FS = 0x100006a00, 0x100007280
PROG_VS_BYTES, PROG_FS_BYTES = 0x100, 0x80   # VS code + its constant buffer at +0xc0; FS code
SIZE = 64

def regmap():
    text = (HDR / 'a6xx.xml.h').read_text()
    m = {}
    for name, v in re.findall(r'#define REG_(A[67]XX_\w+)\s+(0x[0-9a-fA-F]+)\s*$', text, re.M):
        m[name] = int(v, 16)
    return m
R = regmap()
OPS = {}
for name, v in re.findall(r'(CP_\w+)\s*=\s*(\d+)', re.search(r'enum adreno_pm4_type3_packets \{(.*?)\};',
                                                          (HDR / 'adreno_pm4.xml.h').read_text(), re.S).group(1)):
    OPS[name] = int(v)

NAME = {}
for k, v in OPS.items(): NAME[v] = k      # later (a7xx) names win where an opcode number is shared (92: CP_CONTEXT_REG_BUNCH)

def parity(v):
    v ^= v >> 16; v ^= v >> 8; v ^= v >> 4
    return (0x9669 >> (v & 0xf)) & 1         # freedreno_pm4.h pm4_odd_parity_bit: (~0x6996 >> v) & 1
def pkt4(reg, cnt): return 0x40000000 | cnt | parity(cnt) << 7 | (reg & 0x3ffff) << 8 | parity(reg) << 27
def pkt7(op, cnt): return 0x70000000 | cnt | parity(cnt) << 15 | (op & 0x7f) << 16 | parity(op) << 23


def packets(words):
    """Split a stream into [header, payload...] lists."""
    out, i = [], 0
    while i < len(words):
        h = words[i]
        n = (h & 0x7f) if h >> 28 == 4 else (h & 0x3fff) if h >> 28 == 7 else None
        if n is None: raise SystemExit(f'not a type 4/7 packet at {i}: {h:#x}')
        out.append(words[i:i + 1 + n]); i += 1 + n
    return out

def op(p): return (p[0] >> 16) & 0x7f if p[0] >> 28 == 7 else None


class Out:
    def __init__(self):
        self.words, self.relocs = [], []   # relocs: (dword, kind, offset, high)
    def add(self, p): self.words += p


def check_headers(words):
    for p in packets(words):
        h = p[0]
        if h >> 28 == 4: assert h == pkt4((h >> 8) & 0x3ffff, h & 0x7f), hex(h)
        else: assert h == pkt7((h >> 16) & 0x7f, h & 0x3fff), hex(h)


def main():
    rdfile = glob.glob(str(HERE / 'capture/*submit00002.rd'))[0]
    bufs, streams = rd.load(rdfile)
    prim_addr, prim_n = streams[0]
    prim = rd.read(bufs, prim_addr, prim_n)
    check_headers(prim)                      # our header formulas match Mesa's on every recorded packet
    def mem(addr, n): return rd.read(bufs, addr, n)

    o = Out()
    o.shared_consts = []
    for q in packets(prim):
        pass
    # Relocation kinds: 1 program, 2 target, 3 border colours, 4 control
    def reloc_pair(lo_index, kind, offset):
        o.relocs.append((lo_index, kind, offset, 0)); o.relocs.append((lo_index + 1, kind, offset, 1))

    def emit(p, depth=0):
        """Emit one packet of Turnip's stream, flattened and with our addresses."""
        code = op(p)
        name = NAME.get(code) if code is not None else None
        if p[0] >> 28 == 4:
            reg, n = (p[0] >> 8) & 0x3ffff, p[0] & 0x7f
            q = list(p)
            for k in range(n):
                r = reg + k
                base = len(o.words) + 1 + k       # q[1 + k] lands here; the high half is the next register in this packet
                if r in (R['A6XX_SP_TP_BORDER_COLOR_BASE_ADDR'], R['A6XX_SP_PS_TP_BORDER_COLOR_BASE_ADDR'], R['A6XX_RB_2D_DST']):
                    assert k + 1 < n, 'address high half not in the same packet'
                    q[1 + k] = q[2 + k] = 0
                    kind = 2 if r == R['A6XX_RB_2D_DST'] else 3
                    o.relocs.append((base, kind, 0, 0)); o.relocs.append((base + 1, kind, 0, 1))
                if r == R['A6XX_RB_2D_DST_INFO'] and q[1 + k] & 0x1000:        # UBWC + tiled -> linear
                    q[1 + k] &= 0xff
                if r == R['A6XX_RB_2D_DST_FLAGS']:
                    assert k + 1 < n
                    q[1 + k] = q[2 + k] = 0
                if r == R['A6XX_RB_2D_DST_FLAGS_PITCH']:
                    q[1 + k] = 0
            if 0xab40 <= reg < 0xab40 + 64:
                o.shared_consts = list(q[1:1 + n])
            o.add(q); return
        if name in ('CP_SET_AMBLE', 'CP_BV_BR_COUNT_OPS', 'CP_REG_TEST'):
            return
        if name == 'CP_THREAD_CONTROL':
            o.add([pkt7(code, 1), 0x08000001]); return   # BR, concurrent binning off (the native compute test's form)
        if name == 'CP_COND_REG_EXEC':
            mode, flags, n = p[1] >> 28, p[1] & 0x0f000000, p[2]
            return ('cond', mode, flags, n)
        if name == 'CP_SET_DRAW_STATE':
            if p[1] & (1 << 18):   # DISABLE_ALL_GROUPS: keep
                o.add(p); return
            for g in range(1, len(p), 3):
                w = p[g]; n = w & 0xffff
                sysmem, disable = (w >> 22) & 1, (w >> 17) & 1
                if not sysmem or disable or not n: continue
                group = packets(mem(p[g + 1] | p[g + 2] << 32, n))
                for q in group: emit(q, depth + 1)
            return
        if name == 'CP_CONTEXT_REG_BUNCH':
            q = list(p); start = len(o.words)
            for k in range(1, len(q) - 1, 2):
                r, v = q[k], q[k + 1]
                if r == R['A6XX_SP_VS_OBJ_START']: q[k + 1] = 0; reloc_pair_bunch(start, k, q, 1, 0)
                elif r == R['A6XX_SP_FS_OBJ_START']: q[k + 1] = 0; reloc_pair_bunch(start, k, q, 1, PROG_VS_BYTES)
                elif r == 0x8822: q[k + 1] = v & 0xff                    # RB_MRT[0].BUF_INFO: linear, no UBWC
                elif r == 0x8825: q[k + 1] = 0; reloc_pair_bunch(start, k, q, 2, 0)   # RB_MRT[0].BASE
                elif r in (0x8903, 0x8904, 0x8905): q[k + 1] = 0             # RB_MRT_FLAG_BUFFER[0]: none
            o.add(q); return
        if name == 'CP_LOAD_STATE6_GEOM' and len(p) == 6 and (p[1] >> 14) & 3 == 2:   # one UBO descriptor, direct
            q = list(p); lo, hi = q[4], q[5]
            addr = lo | (hi & 0x1ffff) << 32
            assert PROG_VS <= addr < PROG_VS + PROG_VS_BYTES, hex(addr)
            q[4] = 0; q[5] = hi & ~0x1ffff
            o.relocs.append((len(o.words) + 4, 1, addr - PROG_VS, 0))
            o.relocs.append((len(o.words) + 5, 1 | 0x80, addr - PROG_VS, 1))   # 0x80: OR the high bits into the size field
            o.add(q); return
        if name in ('CP_EVENT_WRITE7',) and len(p) > 2:
            return                                    # Turnip's own timestamp; ours comes at the end
        if name in ('CP_INDIRECT_BUFFER_PFE', 'CP_INDIRECT_BUFFER'):
            run(packets(mem(p[1] | p[2] << 32, p[3] & 0xfffff)), depth + 1); return
        if name == 'CP_DRAW_INDX_OFFSET' and o.shared_consts:
            # Diagnostic for boot 11 (512 pixels drawn, none orange): the FS preamble copies the shared immediate constants
            # (stsc c[0], 0, 4) and outputs c0; write the push-constant colour into HLSQ_SHARED_CONSTS_IMM again right before
            # the draw, in case the flattened group order lost it.
            o.add([pkt4(0xab40, len(o.shared_consts))] + o.shared_consts)
        o.add(p)

    def reloc_pair_bunch(start, k, q, kind, offset):
        o.relocs.append((start + k + 1, kind, offset, 0))
        # the high half is the next register pair's value in Turnip's bunches (OBJ_START+1, BASE+1)
        assert q[k + 2] == q[k] + 1, 'address high half not next in the bunch'
        q[k + 3] = 0
        o.relocs.append((start + k + 3, kind, offset, 1))

    def run(ps, depth=0):
        skip = 0
        i = 0
        while i < len(ps):
            p = ps[i]
            r = emit(p, depth)
            if isinstance(r, tuple):   # conditional block: the next r[3] dwords
                _, mode, flags, n = r
                block, taken = [], 0
                while taken < n:
                    i += 1; block.append(ps[i]); taken += len(ps[i])
                keep = (mode == 3 and flags == 0x08000000) or (mode == 5 and flags == 0x04000000)
                if keep:
                    run(block, depth)
            i += 1

    # The recorded primary stream ends with Turnip's resolve: the 2D blit that copies the UBWC target into the readback buffer.
    # Our target is already linear, so everything from the first 2D register write after the draw is replaced by our tail.
    ps = packets(prim)
    draw_at = next(i for i, p in enumerate(ps) if op(p) == OPS['CP_INDIRECT_BUFFER_PFE'])
    copy_at = next(i for i, p in enumerate(ps) if i > draw_at and p[0] >> 28 == 4 and ((p[0] >> 8) & 0x3ffff) == R['A6XX_RB_2D_UNKNOWN_8C01'])
    run(ps[:copy_at])
    # Our end: the recorded end-of-pass flushes are already in (CCU colour flush/invalidate, cache flush, wait for idle). Then the
    # completion handshake of the native compute test, word for word (Mesa-packed, verified on the phone 2026-11-21: v1024/1024 c1):
    # RB_DONE_TS writes seqno 1 into the control buffer, CP_WAIT_REG_MEM waits for it, CACHE_FLUSH7.
    fill = (HERE / 'out/A740FillProgram.h').read_text()
    fib = [int(x, 16) for x in re.search(r'kA740FillIb\[\d+\] = \{(.*?)\};', fill, re.S).group(1).replace(',', ' ').split()]
    frel = [tuple(int(x) for x in t) for t in re.findall(r'\{(\d+), (\d+), (\d+), (\d+)\}', re.search(r'kA740FillIbRelocs.*?\};', fill, re.S).group(0))]
    fps, at, starts = packets(fib), 0, []
    for q in fps: starts.append(at); at += len(q)
    ts = max(i for i, q in enumerate(fps) if op(q) == OPS['CP_EVENT_WRITE7'] and len(q) == 5)
    base = len(o.words)
    for i in range(ts, len(fps)): o.add(fps[i])
    for d, kind, off, hi in frel:
        if d >= starts[ts]:
            assert kind == 4, 'the compute tail only addresses the control buffer'
            o.relocs.append((base + d - starts[ts], 4, off, hi))
    check_headers(o.words)

    vs = struct.pack('<%dI' % (PROG_VS_BYTES // 4), *mem(PROG_VS, PROG_VS_BYTES // 4))
    fs = struct.pack('<%dI' % (PROG_FS_BYTES // 4), *mem(PROG_FS, PROG_FS_BYTES // 4))
    prog = list(struct.unpack('<%dI' % ((len(vs) + len(fs)) // 4), vs + fs))

    with open(sys.argv[1], 'w') as f:
        f.write('// GENERATED by 12r/metal/gen-a740-tri.py from Turnip\'s recorded a7xx submission (12r/metal/capture); do not edit.\n'
                '// SPDX-License-Identifier: MIT\n#pragma once\n#include <stdint.h>\n\n'
                '// Relocation kinds: 1 program (VS at 0, FS at 0x%x), 2 colour target (64x64 RGBA8 linear, pitch 256), 3 border colours,\n'
                '// 4 control (seqno). kind | 0x80: OR the address\'s high 17 bits into the existing word (a UBO descriptor\'s size field).\n'
                'struct A740TriReloc {uint16_t dword; uint8_t kind; uint32_t offset; uint8_t high;};\n'
                '#define A740_TRI_SIZE %d\n#define A740_TRI_CLEAR 0xffff0000u   /* ABGR8: blue */\n'
                '#define A740_TRI_COLOR 0xff0080ffu   /* ABGR8: (1, 0.5, 0, 1); 0.5 may round to 0x7f or 0x80 */\n\n'
                % (PROG_VS_BYTES, SIZE))
        def arr(name, w):
            f.write('static const uint32_t %s[%d] = {' % (name, len(w)))
            for i, x in enumerate(w): f.write(('\n    ' if i % 8 == 0 else ' ') + '0x%08x,' % x)
            f.write('\n};\n')
        arr('kA740TriIb', o.words)
        f.write('static const struct A740TriReloc kA740TriRelocs[%d] = {' % len(o.relocs))
        for i, (d, k, off, hi) in enumerate(o.relocs): f.write(('\n    ' if i % 4 == 0 else ' ') + '{%d, %d, %d, %d},' % (d, k, off, hi))
        f.write('\n};\n')
        arr('kA740TriProgram', prog)
    print(f'{sys.argv[1]}: IB {len(o.words)} dwords, {len(packets(o.words))} packets, {len(o.relocs)} relocations; program {len(prog)} dwords')


if __name__ == '__main__':
    main()
