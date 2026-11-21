#!/usr/bin/env python3
"""Structural checks of out/A740TriProgram.h: every header valid, only flat packets, every capture address relocated."""
import re, importlib.util, collections
from pathlib import Path
spec=importlib.util.spec_from_file_location('rd','rd-decode.py'); rd=importlib.util.module_from_spec(spec); spec.loader.exec_module(rd)
t=open('out/A740TriProgram.h').read()
ib=[int(x,16) for x in re.search(r'kA740TriIb\[\d+\] = \{(.*?)\};',t,re.S).group(1).replace(',',' ').split()]
rel=[tuple(int(x) for x in m) for m in re.findall(r'\{(\d+), (\d+), (\d+), (\d+)\}',re.search(r'kA740TriRelocs.*?\};',t,re.S).group(0))]
regs,ops=rd.tables(Path.home()/'12r-unixnu/ext/mesa-host-build/src/freedreno/registers/adreno')
at={d:(k,o,h) for d,k,o,h in rel}
count=collections.Counter(); bad=[]; i=0; last=None
written={}
while i<len(ib):
    h=ib[i]
    if h>>28==4:
        n=h&0x7f; r=(h>>8)&0x3ffff
        for k in range(n): written[r+k]=(ib[i+1+k], i+1+k)
        i+=1+n
    elif h>>28==7:
        n=h&0x3fff; nm=ops.get((h>>16)&0x7f); count[nm]+=1
        if nm=='CP_CONTEXT_REG_BUNCH':
            for k in range(1,n,2): written[ib[i+k]]=(ib[i+k+1], i+k+1)
        i+=1+n
    else: bad.append(i); break
assert not bad, f'bad header at {bad}'
for forbidden in ('CP_SET_AMBLE','CP_REG_TEST','CP_BV_BR_COUNT_OPS','CP_COND_REG_EXEC','CP_INDIRECT_BUFFER_PFE','CP_INDIRECT_BUFFER'):
    assert count[forbidden]==0, forbidden
import collections as c
print('packets:', dict(c.Counter({k:v for k,v in count.items() if v})))
assert count['CP_DRAW_INDX_OFFSET']==1 and count['CP_BLIT']==1, 'one draw and one clear blit'
def val(name): r=[k for k,v in regs.items() if v==name][0]; return written[r]
for name in ('A6XX_SP_VS_OBJ_START','A6XX_SP_FS_OBJ_START','A6XX_RB_2D_DST','A6XX_SP_TP_BORDER_COLOR_BASE_ADDR'):
    v,d=val(name); assert d in at and (d+1 in at or d+2 in at), name+' not relocated'   # pkt4: next word; bunch: after the next register number
v,d=written[0x8822]; assert v==0x30, hex(v)          # RB_MRT[0].BUF_INFO linear RGBA8
v,d=written[0x8825]; assert d in at and at[d][0]==2   # RB_MRT[0].BASE -> target
assert written[0x8903][0]==0 and written[0x8904][0]==0 and written[0x8905][0]==0   # no flag buffer
v,d=val('A6XX_RB_2D_DST_INFO'); assert v==0x30, hex(v)
# no value that still looks like a capture address high half (1) next to a low half that is not relocated
for r,(v,d) in written.items():
    if v==1 and d not in at and (d-2) in at: print('suspect', regs.get(r,hex(r)), d)
print('relocations', len(rel), sorted(set(k&0x7f for _,k,_,_ in rel)))
print('OK')
