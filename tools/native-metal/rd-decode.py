#!/usr/bin/env python3
"""rd-decode.py: list the command streams of a Mesa FD_RD_DUMP file (.rd, gzip or plain) as PM4 packets, with the A7XX register
names of Mesa's generated a6xx.xml.h and the CP opcode names of adreno_pm4.xml.h. Follows CP_INDIRECT_BUFFER (IB2) and
CP_SET_DRAW_STATE groups into the dumped buffers.

usage: rd-decode.py FILE.rd [--headers DIR]     (DIR: Mesa's build src/freedreno/registers/adreno)
"""
import argparse
import gzip
import re
import struct
from pathlib import Path

RD_GPUADDR, RD_BUFFER_CONTENTS, RD_CMDSTREAM_ADDR = 3, 12, 6


def tables(headers):
    regs, ops = {}, {}
    text = (headers / 'a6xx.xml.h').read_text()
    for prefix in ('REG_A6XX_', 'REG_A7XX_'):   # A7XX names win where both exist
        for name, value in re.findall(r'#define (' + prefix + r'\w+)\s+(0x[0-9a-fA-F]+)\s*$', text, re.M):
            regs[int(value, 16)] = name[4:]
    pm4 = (headers / 'adreno_pm4.xml.h').read_text()
    block = re.search(r'enum adreno_pm4_type3_packets \{(.*?)\};', pm4, re.S).group(1)
    for name, value in re.findall(r'(CP_\w+)\s*=\s*(\d+)', block):
        ops[int(value)] = name   # later (a7xx) names win where an opcode number is shared (92: CP_CONTEXT_REG_BUNCH)
    return regs, ops


def load(path):
    data = Path(path).read_bytes()
    if data[:2] == b'\x1f\x8b':
        data = gzip.decompress(data)
    buffers, streams, at, gpuaddr = [], [], 0, None
    while at + 8 <= len(data):
        kind, size = struct.unpack_from('<II', data, at)
        body = data[at + 8:at + 8 + size]
        at += 8 + size
        if kind == RD_GPUADDR:   # {u32 addr_lo, u32 size, u32 addr_hi} (Mesa decode/rdutil.h parse_addr)
            lo, length = struct.unpack_from('<II', body, 0)
            hi = struct.unpack_from('<I', body, 8)[0] if len(body) > 8 else 0
            gpuaddr = (lo | hi << 32, length)
        elif kind == RD_BUFFER_CONTENTS and gpuaddr:
            buffers.append((gpuaddr[0], body))
        elif kind == RD_CMDSTREAM_ADDR:   # {u32 addr_lo, u32 dwords, u32 addr_hi}
            lo, dwords = struct.unpack_from('<II', body, 0)
            hi = struct.unpack_from('<I', body, 8)[0] if len(body) > 8 else 0
            streams.append((lo | hi << 32, dwords))
    return buffers, streams


def read(buffers, addr, dwords):
    for base, body in buffers:
        if base <= addr and addr + dwords * 4 <= base + len(body):
            return list(struct.unpack_from('<%dI' % dwords, body, addr - base))
    return None


def walk(buffers, addr, dwords, regs, ops, depth=0, out=None):
    words = read(buffers, addr, dwords)
    pad = '  ' * depth
    if words is None:
        out.append(f'{pad}(IB {addr:#x} x{dwords} not in dump)')
        return
    i = 0
    while i < len(words):
        h = words[i]
        kind = h >> 28
        if kind == 4:
            count, reg = h & 0x7f, (h >> 8) & 0x3ffff
            vals = words[i + 1:i + 1 + count]
            for k, v in enumerate(vals):
                out.append(f'{pad}{regs.get(reg + k, "reg_%05x" % (reg + k))} = {v:#010x}')
            i += 1 + count
        elif kind == 7:
            count, op = h & 0x3fff, (h >> 16) & 0x7f
            payload = words[i + 1:i + 1 + count]
            name = ops.get(op, 'CP_%02x' % op)
            out.append(f'{pad}{name}' + (' ' + ' '.join('%08x' % w for w in payload[:12]) if payload else '') +
                       (' ...' if len(payload) > 12 else ''))
            if name == 'CP_CONTEXT_REG_BUNCH':   # (register, value) pairs
                for k in range(0, count - 1, 2):
                    out.append(f'{pad}{regs.get(payload[k], "reg_%05x" % payload[k])} = {payload[k + 1]:#010x}  (bunch)')
            elif name == 'CP_CONTEXT_REG_BUNCH2':   # two header dwords, then (register, value) pairs
                for k in range(2, count - 1, 2):
                    out.append(f'{pad}{regs.get(payload[k], "reg_%05x" % payload[k])} = {payload[k + 1]:#010x}  (bunch2)')
            elif name in ('CP_INDIRECT_BUFFER', 'CP_INDIRECT_BUFFER_PFE', 'CP_INDIRECT_BUFFER_PFD') and count >= 3:
                walk(buffers, payload[0] | payload[1] << 32, payload[2] & 0xfffff, regs, ops, depth + 1, out)
            elif name == 'CP_SET_DRAW_STATE':
                for g in range(0, count - 2, 3):
                    n, a = payload[g] & 0xffff, payload[g + 1] | payload[g + 2] << 32
                    if n and a:
                        out.append(f'{pad}  [group {(payload[g] >> 24) & 0x1f} {n} dwords]')
                        walk(buffers, a, n, regs, ops, depth + 2, out)
            i += 1 + count
        else:
            out.append(f'{pad}?? {h:#010x}')
            i += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rd')
    ap.add_argument('--headers', default=str(Path.home() / '12r-unixnu/ext/mesa-host-build/src/freedreno/registers/adreno'))
    a = ap.parse_args()
    regs, ops = tables(Path(a.headers))
    buffers, streams = load(a.rd)
    out = []
    for n, (addr, dwords) in enumerate(streams):
        out.append(f'== stream {n}: {addr:#x} x{dwords}')
        walk(buffers, addr, dwords, regs, ops, 0, out)
    print('\n'.join(out))


if __name__ == '__main__':
    main()
