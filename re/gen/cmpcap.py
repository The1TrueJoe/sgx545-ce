"""Compare a generated frame (re/gen/frame -t) with a DDK capture, structure by structure.

    python3 re/gen/cmpcap.py REF.bin GEN.bin

Both are pvrtrace logs of a one-triangle frame (the probe's, and frame's
default scene). Regions are paired by following the same pointers in both:
TA command, render details, RT data, PB, free list, background block, VDM
stream, every PDS and USSE program, TA blocks, vertices, uniforms. A word
differs only if it is neither equal nor the paired address (raw, >>4 in a
VDM/PDS word, a pixel PDS state word, or a DOUTU word).

Against ref.bin the generator's frame leaves 9 regions with differences, all
of them expected: kick 2's VDM tail (the DDK writes it after kick 1), the
vertex secondaries' own address (which falls inside the DDK's 25-dword TA
block DMA, so the map pairs it wrongly), the word after the DDK's 3-entry
clear index buffer, the TA block DMA's padding, the render target address
in the background texture words and the end-of-tile LIMM, and the `phas.i`
targets.
"""
import os
import sys

R = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(R, "decode"))
import memmap  # noqa: E402
import pvrlog  # noqa: E402


def load(path):
    log = pvrlog.load(path)
    mm = memmap.build(log)
    snaps = [s for s in log.of(pvrlog.Snap) if s.reason == "kick" and s.kick == 1]
    return log, mm, memmap.Memory(mm, snaps)


A = load(sys.argv[1])
B = load(sys.argv[2])
ma, mb = A[2], B[2]
pairs=[]   # (name, va_a, va_b, nwords)
def P(name, a, b, n): pairs.append((name, a, b, n))
def wa(va,n=1): return ma.words(va,n)
def wb(va,n=1): return mb.words(va,n)
ccb_a=[x for x in A[1].bufs if x.size==0x10420][0].devva; ccb_b=[x for x in B[1].bufs if x.size==0x10420][0].devva
P('cmd', ccb_a, ccb_b, 0x16); P('cmdtail', ccb_a+0x238, ccb_b+0x238, 0x1c)
ha, hb = wa(ccb_a,0x16), wb(ccb_b,0x16)
P('rd', ha[0xe], hb[0xe], 0x1ec//4); P('hwsync', ha[0xf], hb[0xf], 10); P('rtdata', ha[0x10], hb[0x10], 0x22)
P('pbdesc', ha[9], hb[9], 15)
pa, pb = wa(ha[9],15), wb(hb[9],15)
P('pbblocks', pa[14], pb[14], 4); P('freelist', pa[4], pb[4], 0x422)
ra, rb = wa(ha[0xe],0x7b), wb(hb[0xe],0x7b)
P('bgblock', 0x10000000+((ra[0x12]&0xffffff)<<4), 0x10000000+((rb[0x12]&0xffffff)<<4), 0x80)
P('rd_sync', ra[0xd0//4], rb[0xd0//4], 2); P('rd_w8', ra[0x110//4], rb[0x110//4], 2); P('rd_w4', ra[0x114//4], rb[0x114//4], 1)
P('hwsync_access', wa(ha[0xf])[0], wb(hb[0xf])[0], 2)
P('rtword', wa(ha[0x10])[0], wb(hb[0x10])[0], 1)
P('pixev', ra[0x5c//4], rb[0x5c//4], 0xc4//4)
ta, tb = wa(ccb_a+0x238,0x1c), wb(ccb_b+0x238,0x1c)
P('vdm', ta[6], tb[6], 28)
va, vb = wa(ta[6],28), wb(tb[6],28)
def pv(w): return (w & 0x0fffffff) << 4
def pp(w): return 0x2e000000 + ((w & 0x3ffffff) << 4)
names=['clear_state',None,'clear_draw',None,None,None,'clear_vfetch',None,'tri_state',None,'vsec_state',None,'tri_draw',None,None,None,'tri_vfetch',None,'empty_state',None,'terminate']
P('clear_state', pv(va[0]), pv(vb[0]), 20); P('clear_vfetch', pv(va[6]), pv(vb[6]), 12)
P('tri_state', pv(va[8]), pv(vb[8]), 20); P('vsec_state', pv(va[10]), pv(vb[10]), 11)
P('tri_vfetch', pv(va[16]), pv(vb[16]), 12); P('empty_state', pv(va[18]), pv(vb[18]), 6); P('terminate', pv(va[20]), pv(vb[20]), 18)
P('idx_clear', va[3], vb[3], 2); P('idx_tri', va[13], vb[13], 2)
for nm in ('clear','tri'):
    sa, sb = wa(pv(va[0 if nm=='clear' else 8]),1)[0], wb(pv(vb[0 if nm=='clear' else 8]),1)[0]
    P(nm+'_block', sa, sb, 12 if nm=='clear' else 25)
    ba, bb = wa(sa,25), wb(sb,25)
    off = 3 if nm=='clear' else 3
    P(nm+'_pds_sec', pp(ba[3]), pp(bb[3]), 4*(ba[3]>>26)+4)
    P(nm+'_pds_pri', pp(ba[5]), pp(bb[5]), 4*(ba[5]>>26)+4)
for nm, i in (('clear_vb', 6), ('tri_vb', 16)):
    fa, fb = wa(pv(va[i]),1)[0], wb(pv(vb[i]),1)[0]
    P(nm, fa, fb, 12 if nm=='clear_vb' else 6)
vsa, vsb = wa(pv(va[10]),1)[0], wb(pv(vb[10]),1)[0]
P('vsec_data', vsa, vsb, 10)
bga, bgb = ha[4], hb[4]
P('bg_pds', pp(bga), pp(bgb), 16)
def usse(w, vertex):
    base = 0x3c000000 if vertex else 0x37000000
    return base + ((w & 0xf) - (10 if vertex else 0)) * 0x80000 + ((w >> 4) & 0x3ffff) * 8
# USSE programs from DOUTU words
def addu(name, ra_, rb_, idx, vertex, n):
    P(name, usse(wa(ra_+4*idx)[0], vertex), usse(wb(rb_+4*idx)[0], vertex), 2*n)
addu('u_clear_state', pv(va[0]), pv(vb[0]), 8, 1, 9)
addu('u_tri_state', pv(va[8]), pv(vb[8]), 8, 1, 10)
addu('u_vsec', pv(va[10]), pv(vb[10]), 4, 1, 1)
addu('u_clear_vs', pv(va[6]), pv(vb[6]), 4, 1, 14)
addu('u_tri_vs', pv(va[16]), pv(vb[16]), 4, 1, 12)
addu('u_empty_state', pv(va[18]), pv(vb[18]), 0, 1, 1)
addu('u_terminate', pv(va[20]), pv(vb[20]), 8, 1, 9)
cba=[x for x in pairs if x[0]=='clear_block'][0]; tba=[x for x in pairs if x[0]=='tri_block'][0]
ca_, cb_ = wa(cba[1],12), wb(cba[2],12); ta_, tb_ = wa(tba[1],25), wb(tba[2],25)
addu('u_clear_sec', pp(ca_[3]), pp(cb_[3]), 0, 0, 1)
addu('u_clear_pri', pp(ca_[5]), pp(cb_[5]), 0, 0, 2)
addu('u_tri_sec', pp(ta_[3]), pp(tb_[3]), 4, 0, 7)
addu('u_tri_pri', pp(ta_[5]), pp(tb_[5]), 0, 0, 2)
P('uniforms', wa(pp(ta_[3]))[0], wb(pp(tb_[3]))[0], 11)
addu('u_bg', pp(bga), pp(bgb), 0, 0, 2)
pea, peb = ra[0x5c//4], rb[0x5c//4]
addu('u_pe_ptoff', pea, peb, 0, 0, 2); addu('u_pe_emit', pea, peb, 4, 0, 3); addu('u_eot', pea, peb, 3, 0, 7)
# address map
amap=[(a,b,4*n) for _,a,b,n in pairs]
def mapped(x):
    for a,b,l in amap:
        if a<=x<a+l: return b+(x-a)
    return None
bad=0
for name,a,b,n in pairs:
    xa, xb = wa(a,n), wb(b,n)
    if xa is None or xb is None: print('MISSING', name, hex(a), hex(b)); continue
    diffs=[]
    for i,(x,y) in enumerate(zip(xa,xb)):
        if x==y: continue
        m=mapped(x)
        if m is not None and m==y: continue
        m=mapped(pv(x))
        if m is not None and (x&0xf0000000)|(m>>4)==y: continue
        m=mapped(pp(x))
        if m is not None and (x&0xfc000000)|((m-0x2e000000)>>4)==y: continue
        for vtx in (0,1):
            m=mapped(usse(x,vtx))
            if m is not None:
                base=0x3c000000 if vtx else 0x37000000; off=m-base
                if (x&0xff000000)|((off>>19)+(10 if vtx else 0))|((off&0x7ffff)>>3)<<4==y: break
        else:
            diffs.append((i,x,y))
    print(f"{name:16s} {a:#010x} {b:#010x} {n:4d}w", 'same' if not diffs else f'{len(diffs)} DIFF')
    for i,x,y in diffs[:8]: print(f"      +{4*i:#05x} cap {x:08x} gen {y:08x}")
    bad+=bool(diffs)
print('regions with differences:', bad)
