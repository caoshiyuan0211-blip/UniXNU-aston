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


# ---- Stage 2b: the values that depend on the target and the colours, computed here (Turnip's formulas) ----
def f32(x): return struct.unpack('<I', struct.pack('<f', x))[0]

def guardband(offset, scale):
    """Mesa common/freedreno_guardband.h fd_calc_guardband (a4xx+), in float32 steps."""
    import math
    gb_min, gb_max = -32768.0, 32767.0
    sc = abs(scale)
    gb_adj = min(-((gb_min - offset) / sc), (gb_max - offset) / sc)
    if gb_adj < 1.0 or math.isinf(gb_adj): return 0x1ff
    m, e = math.frexp(gb_adj)
    if e <= 0 or e > 8: return 0x1ff
    return ((e - 1) << 6) | (int(m * (1 << 7)) - (1 << 6))

def params(W, H, clear, color):
    """Register -> value for a W x H RGBA8 linear target, cleared to `clear` and drawn in `color` (RGBA floats). Viewport and
    scissors as Turnip's tu6_emit_viewport and sysmem pass; pitches as a linear fdl layout (64-byte pitch alignment)."""
    br = (W - 1) | (H - 1) << 16
    pitch = (W * 4 + 63) // 64 * 64
    gb = min(0x1ff, guardband(W / 2, W / 2)) | min(0x1ff, guardband(H / 2, H / 2)) << 10
    v = {
        R['A6XX_GRAS_SC_WINDOW_SCISSOR_BR']: br, R['A6XX_GRAS_2D_RESOLVE_CNTL_2']: br, R['A6XX_GRAS_2D_DST_BR']: br,
        R['A6XX_GRAS_2D_SRC_BR_X']: (W - 1) << 8, R['A6XX_GRAS_2D_SRC_BR_Y']: (H - 1) << 8,
        R['A6XX_RB_2D_DST_PITCH']: pitch // 64,
        0x8823: pitch // 64, 0x8824: pitch * H // 64,                       # RB_MRT[0].PITCH, ARRAY_PITCH (64-byte units)
        0x8010: f32(W / 2), 0x8011: f32(W / 2), 0x8012: f32(H / 2), 0x8013: f32(H / 2),   # GRAS_CL_VPORT X/Y offset, scale
        0x80d1: br,                                                          # GRAS_SC_VIEWPORT_SCISSOR_BR(0)
        0x80b1: br,                                                          # GRAS_SC_SCREEN_SCISSOR_BR(0)
        R['A6XX_GRAS_CL_GUARDBAND_CLIP_ADJ']: gb,
    }
    for i, c in enumerate(clear): v[R['A6XX_RB_2D_SRC_SOLID_C0'] + i] = round(c * 255)   # RGBA8 UNORM solid fill, per channel
    for i, c in enumerate(color): v[0xab40 + i] = f32(c)                    # HLSQ_SHARED_CONSTS_IMM: the FS colour (push constant)
    return v

def apply(words, values, verify, start=0):
    """Write `values` into every pkt4 / CP_CONTEXT_REG_BUNCH write of those registers. With verify, require the stream to hold
    exactly those values already (the self-check against Turnip's recording). Returns {register: occurrences}."""
    seen = {}
    i = start
    while i < len(words):
        h = words[i]
        if h >> 28 == 4:
            n, reg = h & 0x7f, (h >> 8) & 0x3ffff
            for k in range(n):
                r = reg + k
                if r in values:
                    if verify: assert words[i + 1 + k] == values[r], f'reg {r:#x}: recorded {words[i + 1 + k]:#x}, computed {values[r]:#x}'
                    words[i + 1 + k] = values[r]; seen[r] = seen.get(r, 0) + 1
            i += 1 + n
        else:
            n = h & 0x3fff
            if (h >> 16) & 0x7f == OPS['CP_CONTEXT_REG_BUNCH']:
                for k in range(1, n, 2):
                    r = words[i + k]
                    if r in values:
                        if verify: assert words[i + k + 1] == values[r], f'reg {r:#x}: recorded {words[i + k + 1]:#x}, computed {values[r]:#x}'
                        words[i + k + 1] = values[r]; seen[r] = seen.get(r, 0) + 1
            i += 1 + n
    return seen


# ---- Stage 2c: our own shader binaries (12r/metal/a740-compile.c) and the registers computed from their layout ----
def insert(o, at, words, relocs=()):
    """Insert whole packets at dword `at`, moving the relocations behind it."""
    n = len(words)
    o.words[at:at] = words
    o.relocs = [(d + n if d >= at else d, k, off, h) for d, k, off, h in o.relocs] + [(at + d, k, off, h) for d, k, off, h in relocs]

def find_packets(words, pred):
    i, out = 0, []
    while i < len(words):
        h = words[i]; n = (h & 0x7f) if h >> 28 == 4 else (h & 0x3fff)
        if pred(words[i:i + 1 + n]): out.append(i)
        i += 1 + n
    return out

def reg_value(words, reg):
    """The last value the stream writes to `reg` (pkt4 or CP_CONTEXT_REG_BUNCH)."""
    val, i = None, 0
    while i < len(words):
        h = words[i]
        if h >> 28 == 4:
            n, r = h & 0x7f, (h >> 8) & 0x3ffff
            if r <= reg < r + n: val = words[i + 1 + reg - r]
            i += 1 + n
        else:
            n = h & 0x3fff
            if (h >> 16) & 0x7f == OPS['CP_CONTEXT_REG_BUNCH']:
                for k in range(1, n, 2):
                    if words[i + k] == reg: val = words[i + k + 1]
            i += 1 + n
    return val

def load_state0(dst_off, state_type, state_src, block, units):
    # CP_LOAD_STATE6_0: DST_OFF[13:0] STATE_TYPE[15:14] STATE_SRC[17:16] STATE_BLOCK[21:18] NUM_UNIT[31:22]
    return dst_off | state_type << 14 | state_src << 16 | block << 18 | units << 22

ST6_SHADER, ST6_CONSTANTS, ST6_UBO, SS6_DIRECT, SS6_INDIRECT, SB6_FS_TEX, SB6_VS_SHADER, SB6_FS_SHADER = 0, 1, 2, 0, 2, 4, 8, 12
# Stage 3b texture buffer (relocation kind 5): the texels at 0, the texture descriptor and the sampler after them (64-byte aligned).
TEX_KIND = 5

def stage2c(o, d, color):
    import json, math
    vs, fs = json.load(open(d / 'vs.json')), json.load(open(d / 'fs.json'))
    linkfile = json.load(open(d / 'link.json')) if (d / 'link.json').exists() else None
    link = linkfile['regs'] if linkfile else None
    tex = linkfile.get('tex') if linkfile else None
    if link is None:
        # Stage 2c: what the stream already says, and our layout must agree with (the registers this stage does not compute).
        assert vs['outputs'] == [[0, reg_value(o.words, 0xa803) & 0xff]], 'VS position output regid (SP_VS_OUT_REG[0])'
        assert (reg_value(o.words, 0xa803) >> 8) & 0xf == 0xf, 'position written as xyzw'
        assert fs['outputs'] == [[4, reg_value(o.words, 0xa98e) & 0xff]], 'FS colour output regid (SP_FS_OUTPUT_REG[0])'
        assert fs['total_in'] == 0
    assert [i[1] for i in vs['inputs'] if i[0] == 9] == [reg_value(o.words, R['A6XX_VFD_CONTROL_1']) & 0xff], 'vertex id regid (VFD_CONTROL_1)'
    assert vs['consts_ubo'] in (1, -1) and fs['consts_ubo'] == -1, 'constant-data UBO slots as in the stream'
    # tu6_emit_xs (Turnip): SP_xS_CTRL_REG0 from the variant; tu6_emit_xs_config: HLSQ_xS_CNTL constlen (>> 2) | enabled
    def ctrl0(v, fs_stage):
        w = (v['max_half_reg'] + 1) << 1 | (v['max_reg'] + 1) << 7 | v['branchstack_hw'] << 14
        if v['mergedregs']: w |= 0x80000000 if fs_stage else 0x00100000
        if v['early_preamble']: w |= 0x10000000 if fs_stage else 0x00200000
        if fs_stage:
            w |= (1 if v['double_threadsize'] else 0) << 20 | (0x00400000 if v['total_in'] else 0) | (0x00800000 if v['need_full_quad'] else 0)
            w |= 0x01000000 | (0x04000000 if v['need_pixlod'] else 0)   # UNK24: Turnip always sets it
        return w
    values = {
        R['A6XX_SP_VS_CTRL_REG0']: ctrl0(vs, False), R['A6XX_SP_FS_CTRL_REG0']: ctrl0(fs, True),
        R['A6XX_SP_VS_INSTRLEN']: vs['instrlen'], R['A6XX_SP_FS_INSTRLEN']: fs['instrlen'],
        R['A7XX_HLSQ_VS_CNTL']: (vs['constlen'] >> 2) | 0x100, R['A7XX_HLSQ_FS_CNTL']: (fs['constlen'] >> 2) | 0x100,
        R['A7XX_HLSQ_FS_CNTL_0']: 1 if fs['double_threadsize'] else 0,
    }
    # Turnip's FS read the colour from HLSQ_SHARED_CONSTS_IMM; ours reads c0 (loaded below). Zero the shared constants, so a right
    # colour on the phone can only have come through our own const load.
    for i in range(4): values[0xab40 + i] = 0
    before = {r: reg_value(o.words, r) for r in values}
    apply(o.words, values, verify=False)
    if link is not None:
        # Stage 3: the linkage registers a740-compile computed with Turnip's tu6_emit_vpc / tu6_emit_fs_inputs / tu6_emit_fs_outputs
        # (self-checked: for the smoke pair all equal Turnip's recorded values). Registers the stream never writes go in before the draw.
        lv = dict((r, v) for r, v in link)
        # Only the draw's own state: from the render pass marker on (Turnip's hardware init writes some of these too, e.g.
        # HLSQ_CONTROL_5_REG, and is left as recorded).
        marker = find_packets(o.words, lambda p: op(p) == OPS['CP_SET_MARKER'])[0]
        seen = apply(o.words, lv, verify=False, start=marker)
        missing = [r for r in lv if r not in seen]
        if missing:
            draw = find_packets(o.words, lambda p: op(p) == OPS['CP_DRAW_INDX_OFFSET'])[0]
            add = []
            for r in sorted(missing): add += [pkt4(r, 1), lv[r]]
            insert(o, draw, add)
        print(f'stage 3: {len(lv)} linkage registers ({len(missing)} added)')
    # Program buffer: our VS (code + constant data) at 0, our FS at the next 256-byte boundary (instrlen units are 128 bytes).
    vsw, fsw = vs['words'], fs['words']
    fs_offset = (len(vsw) * 4 + 255) // 256 * 256
    prog = vsw + [0] * (fs_offset // 4 - len(vsw)) + fsw
    # Code addresses: the FS reloc pointed at Turnip's layout; the VS one already at 0.
    o.relocs = [(dd, k, fs_offset if (k == 1 and off == PROG_VS_BYTES) else off, h) for dd, k, off, h in o.relocs]
    # The constant-data UBO descriptor (Turnip loads it from its preamble; we keep it) now points at our constant data ...
    ubo = find_packets(o.words, lambda p: op(p) == OPS['CP_LOAD_STATE6_GEOM'] and len(p) == 6 and (p[1] >> 14) & 3 == ST6_UBO)
    assert len(ubo) == 1 and vs['consts_ubo'] == 1, 'the VS keeps its constant data in UBO 1, as the recorded stream'
    u = ubo[0]
    o.words[u + 5] = math.ceil(vs['constant_data_size'] / 16) << 17          # A6XX_UBO_1_SIZE (vec4), the address high bits ORed in
    o.relocs = [(dd, k, vs['constant_data_offset'] if dd in (u + 4, u + 5) else off, h) for dd, k, off, h in o.relocs]
    # ... and, as Turnip's non-preamble path (tu6_emit_xs, !load_shader_consts_via_preamble), the used range goes into the const file.
    loads, lrel = [], []
    for r in vs['ubo_ranges']:
        assert r['block'] == vs['consts_ubo'] and not r['bindless']
        size = min(r['end'] - r['start'], 16 * vs['constlen'] - r['offset'])
        at = len(loads)
        loads += [pkt7(OPS['CP_LOAD_STATE6_GEOM'], 3), load_state0(r['offset'] // 16, ST6_CONSTANTS, SS6_INDIRECT, SB6_VS_SHADER, size // 16), 0, 0]
        lrel += [(at + 2, 1, vs['constant_data_offset'] + r['start'], 0), (at + 3, 1, vs['constant_data_offset'] + r['start'], 1)]
    insert(o, u + 6, loads, lrel)
    # The FS push constants (the colour) into its const file, direct, right before the draw.
    draw = find_packets(o.words, lambda p: op(p) == OPS['CP_DRAW_INDX_OFFSET'])
    assert len(draw) == 1
    if fs['push_consts_dwords']:
        assert fs['push_consts_dwords'] == 4
        push = [pkt7(OPS['CP_LOAD_STATE6_FRAG'], 3 + 4), load_state0(fs['push_consts_vec4'], ST6_CONSTANTS, SS6_DIRECT, SB6_FS_SHADER, 1), 0, 0] + [f32(c) for c in color]
        insert(o, draw[0], push)
    if tex:
        # Stage 3b: one texture and one sampler in the FS's (non-bindless) tables, as Gallium's fd6_emit_textures: CP_LOAD_STATE6
        # of the sampler (ST6_SHADER) and descriptor (ST6_CONSTANTS) from memory, and the table registers the TP reads them from.
        assert tex['num_samp'] == 1 and tex['address_dwords'][0][0] == 4 and tex['address_dwords'][1][0] == 5
        draw = find_packets(o.words, lambda p: op(p) == OPS['CP_DRAW_INDX_OFFSET'])[0]
        desc_off = (tex['pitch'] * tex['height'] + 63) // 64 * 64
        samp_off = desc_off + 64
        w = [pkt7(OPS['CP_LOAD_STATE6_FRAG'], 3), load_state0(0, ST6_SHADER, SS6_INDIRECT, SB6_FS_TEX, 1), 0, 0,
             pkt7(OPS['CP_LOAD_STATE6_FRAG'], 3), load_state0(0, ST6_CONSTANTS, SS6_INDIRECT, SB6_FS_TEX, 1), 0, 0,
             pkt4(R['A6XX_SP_FS_TEX_SAMP'], 2), 0, 0, pkt4(R['A6XX_SP_FS_TEX_CONST'], 2), 0, 0, pkt4(R['A6XX_SP_FS_TEX_COUNT'], 1), 1]
        rel = []
        for at, off in ((2, samp_off), (6, desc_off), (9, samp_off), (12, desc_off)):
            rel += [(at, TEX_KIND, off, 0), (at + 1, TEX_KIND, off, 1)]
        insert(o, draw, w, rel)
        tex['desc_off'], tex['samp_off'] = desc_off, samp_off
        # Two triangles: the draw's vertex count (CP_DRAW_INDX_OFFSET_2 NUM_INDICES) 3 -> 6.
        draw = find_packets(o.words, lambda p: op(p) == OPS['CP_DRAW_INDX_OFFSET'])[0]
        assert o.words[draw + 3] == 3, 'the recorded draw is one triangle'
        o.words[draw + 3] = 6
        o.tex = tex
        print(f"stage 3b: texture {tex['width']}x{tex['height']} pitch {tex['pitch']}, descriptor at {desc_off:#x}, sampler at {samp_off:#x}; 6 vertices")
    check_headers(o.words)
    print('stage 2c: our shaders; computed', ', '.join(f'{next((k for k, v in R.items() if v == r), hex(r))} {before[r]:#x}->{values[r]:#x}' for r in values))
    return prog, fs_offset


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--size', type=int, nargs=2, default=[64, 64])
    ap.add_argument('--clear', type=float, nargs=4, default=[0.0, 0.0, 1.0, 1.0])
    ap.add_argument('--color', type=float, nargs=4, default=[1.0, 0.5, 0.0, 1.0])
    ap.add_argument('--blend', type=float, nargs=4, metavar=('R', 'G', 'B', 'A'),
                    help='stage 3c: the FS outputs this colour over the 64x64 square, alpha-blended (src_alpha, 1 - src_alpha, add)')
    ap.add_argument('--gradient', action='store_true', help='stage 3a: the shaders interpolate a per-vertex colour (red, green, blue)')
    ap.add_argument('--shaders', help='stage 2c: DIR with vs.json and fs.json from a740-compile (our own binaries)')
    args = ap.parse_args()
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

    # Self-check: for Turnip's own case every computed value must equal the recorded one; then the requested target and colours.
    turnip = params(64, 64, (0.0, 0.0, 1.0, 1.0), (1.0, 0.5, 0.0, 1.0))
    seen = apply(o.words, turnip, verify=True)
    missing = set(turnip) - set(seen)
    assert not missing, 'never written: ' + ', '.join(hex(r) for r in sorted(missing))
    W, H = args.size
    apply(o.words, params(W, H, args.clear, args.color), verify=False)
    print(f'self-check: {len(turnip)} registers ({sum(seen.values())} writes) computed = Turnip\'s recorded values; generating {W}x{H}')
    vs = struct.pack('<%dI' % (PROG_VS_BYTES // 4), *mem(PROG_VS, PROG_VS_BYTES // 4))
    fs = struct.pack('<%dI' % (PROG_FS_BYTES // 4), *mem(PROG_FS, PROG_FS_BYTES // 4))
    prog = list(struct.unpack('<%dI' % ((len(vs) + len(fs)) // 4), vs + fs))
    fs_offset = PROG_VS_BYTES
    if args.shaders:
        prog, fs_offset = stage2c(o, Path(args.shaders), args.color)

    if args.blend:
        # Stage 3c: blending on for MRT 0 (Turnip's recording has it off): rgb and alpha both src*SRC_ALPHA + dst*ONE_MINUS_SRC_ALPHA.
        marker = find_packets(o.words, lambda p: op(p) == OPS['CP_SET_MARKER'])[0]
        blend = {R['A6XX_SP_BLEND_CNTL']: 0x101, R['A6XX_RB_BLEND_CNTL']: 0xffff0101, 0x8820: 0x783, 0x8821: 0x07060706}
        seen = apply(o.words, blend, verify=False, start=marker)
        assert set(seen) == set(blend), 'blend registers not all in the draw state'
        draw = find_packets(o.words, lambda p: op(p) == OPS['CP_DRAW_INDX_OFFSET'])[0]
        if o.words[draw + 3] == 3: o.words[draw + 3] = 6
        assert o.words[draw + 3] == 6
        check_headers(o.words)
        print('stage 3c: blending on (SP/RB_BLEND_CNTL, RB_MRT[0] CONTROL/BLEND_CONTROL); 6 vertices')
    def abgr(c): return sum(round(x * 255) << (8 * i) for i, x in enumerate(c))
    with open(args.out, 'w') as f:
        f.write('// GENERATED by 12r/metal/gen-a740-tri.py from Turnip\'s recorded a7xx submission (12r/metal/capture), target and colour\n'
                '// registers computed (stage 2b); do not edit. SPDX-License-Identifier: MIT\n#pragma once\n#include <stdint.h>\n\n'
                '// Relocation kinds: 1 program (VS at 0, FS at 0x%x), 2 colour target (RGBA8 linear), 3 border colours, 4 control (seqno).\n'
                '// kind | 0x80: OR the address\'s high 17 bits into the existing word (a UBO descriptor\'s size field).\n'
                'struct A740TriReloc {uint16_t dword; uint8_t kind; uint32_t offset; uint8_t high;};\n'
                '#define A740_TRI_W %d\n#define A740_TRI_H %d\n#define A740_TRI_PITCH %d   /* bytes */\n'
                '#define A740_TRI_CLEAR 0x%08xu   /* ABGR8 */\n#define A740_TRI_COLOR 0x%08xu   /* ABGR8; a channel may round one step either way */\n'
                '#define A740_TRI_AREA %d   /* the triangle (-0.5,-0.5) (0.5,-0.5) (0,0.5) covers W*H/8 pixels */\n\n'
                % (fs_offset, W, H, (W * 4 + 63) // 64 * 64, abgr(args.clear), abgr(args.color), W * H // 8))
        if args.blend:
            f.write('// Stage 3c: the FS colour A740_TRI_BLEND_SRC (r, g, b, a) blended over the clear colour on the 64x64 square at\n'
                    '// (A740_TRI_SQ_X, A740_TRI_SQ_Y): each channel = src * a + dst * (1 - a), within one step.\n'
                    '#define A740_TRI_BLEND 1\n#define A740_TRI_SQ_X %d\n#define A740_TRI_SQ_Y %d\n'
                    'static const float kA740TriBlendSrc[4] = {%.6ff, %.6ff, %.6ff, %.6ff};\nstatic const float kA740TriClear[4] = {%.6ff, %.6ff, %.6ff, %.6ff};\n\n'
                    % ((W // 4, H // 6) + tuple(args.blend) + tuple(args.clear)))
        if args.gradient:
            f.write('// Stage 3a: the VS writes a per-vertex colour, the FS outputs it interpolated. The phone test computes every pixel\'s\n'
                    '// expected colour from these (clip-space positions, RGBA colours) with barycentrics at the pixel centre.\n'
                    '#define A740_TRI_GRADIENT 1\n'
                    'static const float kA740TriPos[3][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.0f, 0.5f}};\n'
                    'static const float kA740TriRgba[3][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};\n\n')
        tex = getattr(o, 'tex', None)
        if tex:
            f.write('// Stage 3b: relocation kind 5 is the texture buffer: %dx%d RGBA8 linear texels (pitch %d) at 0, the texture descriptor\n'
                    '// at A740_TRI_TEX_DESC_OFF (dword 4 = address low, dword 5 |= address high 17 bits) and the sampler at A740_TRI_TEX_SAMP_OFF.\n'
                    '// The FS samples it nearest at the square (A740_TRI_TEX_X, A740_TRI_TEX_Y) + 64x64: pixel (x, y) shows texel (x - X, y - Y).\n'
                    '#define A740_TRI_TEXTURE 1\n#define A740_TRI_TEX_W %d\n#define A740_TRI_TEX_H %d\n#define A740_TRI_TEX_PITCH %d\n'
                    '#define A740_TRI_TEX_DESC_OFF %d\n#define A740_TRI_TEX_SAMP_OFF %d\n#define A740_TRI_TEX_X %d\n#define A740_TRI_TEX_Y %d\n'
                    % (tex['width'], tex['height'], tex['pitch'], tex['width'], tex['height'], tex['pitch'], tex['desc_off'], tex['samp_off'],
                       W // 4, H // 6))
            arr0 = lambda name, w: f.write('static const uint32_t %s[%d] = {%s};\n' % (name, len(w), ', '.join('0x%08x' % x for x in w)))
            arr0('kA740TriTexDesc', tex['descriptor'])
            arr0('kA740TriTexSamp', tex['sampler'])
            f.write('\n')
        def arr(name, w):
            f.write('static const uint32_t %s[%d] = {' % (name, len(w)))
            for i, x in enumerate(w): f.write(('\n    ' if i % 8 == 0 else ' ') + '0x%08x,' % x)
            f.write('\n};\n')
        arr('kA740TriIb', o.words)
        f.write('static const struct A740TriReloc kA740TriRelocs[%d] = {' % len(o.relocs))
        for i, (d, k, off, hi) in enumerate(o.relocs): f.write(('\n    ' if i % 4 == 0 else ' ') + '{%d, %d, %d, %d},' % (d, k, off, hi))
        f.write('\n};\n')
        arr('kA740TriProgram', prog)
    print(f'{args.out}: IB {len(o.words)} dwords, {len(packets(o.words))} packets, {len(o.relocs)} relocations; program {len(prog)} dwords')


if __name__ == '__main__':
    main()
