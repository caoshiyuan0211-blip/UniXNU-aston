#!/usr/bin/env python3
"""diff-draw.py HAKIOS.bin A740_HEADER.h -- registers of hakios-metal's draw (raw words) vs the 12R's proven a7xx draw stream."""
import re, struct, sys
sys.argv, args = [sys.argv[0]], sys.argv[1:]
src = open(__import__('os').path.join(__import__('os').path.dirname(__file__), '..', 'gen-a740-tri.py')).read().split('\ndef main')[0]
g = {'__file__': __import__('os').path.join(__import__('os').path.dirname(__file__), '..', 'gen-a740-tri.py')}
exec(src, g)
R = g['regmap'](); name = {v: k for k, v in R.items()}; OPS = g['OPS']; opname = {v: k for k, v in OPS.items()}
def decode(w):
    regs, ops, i = {}, [], 0
    while i < len(w):
        h = w[i]
        if h >> 28 == 4:
            n, r = h & 0x7f, (h >> 8) & 0x3ffff
            for k in range(n): regs[r + k] = w[i + 1 + k]
            i += 1 + n
        elif h >> 28 == 7:
            n, op = h & 0x3fff, (h >> 16) & 0x7f
            if op == OPS.get('CP_CONTEXT_REG_BUNCH'):
                for k in range(1, n, 2): regs[w[i + k]] = w[i + k + 1]
            ops.append(opname.get(op, str(op)))
            i += 1 + n
        else:
            i += 1
    return regs, ops
hk = list(struct.unpack('<%dI' % (len(open(args[0], 'rb').read()) // 4), open(args[0], 'rb').read()))
hdr = open(args[1]).read()
ib = [int(x, 16) for x in re.search(r'kA740TriIb\[\d+\] = \{(.*?)\};', hdr, re.S).group(1).replace(',', ' ').split()]
H, hops = decode(hk); A, aops = decode(ib)
n = lambda r: name.get(r, hex(r))
print('hakios packets:', ' '.join(hops)); print('a740 packets:  ', ' '.join(aops))
print(f'\n== in the A740 stream, NOT written by hakios ({len(set(A) - set(H))}):')
for r in sorted(set(A) - set(H)): print(f'  {r:05x} {n(r):40s} = {A[r]:08x}')
print(f'\n== written by hakios, NOT in the A740 stream ({len(set(H) - set(A))}):')
for r in sorted(set(H) - set(A)): print(f'  {r:05x} {n(r):40s} = {H[r]:08x}')
print(f'\n== both, different values ({sum(1 for r in set(H) & set(A) if H[r] != A[r])}; {sum(1 for r in set(H) & set(A) if H[r] == A[r])} equal):')
for r in sorted(set(H) & set(A)):
    if H[r] != A[r]: print(f'  {r:05x} {n(r):40s} hakios {H[r]:08x}  a740 {A[r]:08x}')
