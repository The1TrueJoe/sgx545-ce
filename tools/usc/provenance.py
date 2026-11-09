"""Per USSE form: words our compiler emitted in the A/B suites (all ran on the
GPU with the DDK's pixels), DDK corpus words, and hwt test lines using it.
Usage: python3 tools/usc/provenance.py SCRATCH (the dir holding out/<suite>).
Run from tools/isa."""
import sys, os, glob, re, collections
sys.path.insert(0, '.')
import hw, usp
def key(x):
    n = x.form.name
    if n == 'pck':
        F = ["u8","s8","o8","u16","s16","f16","f32","c10"]
        return f"pck.{F[x.form.fval>>38&7]}.{F[x.form.fval>>41&7]}"
    if n in ('movc',):
        return f"movc.t{x.form.fval>>40&7}"
    return n
def words_of(p):
    s = usp.load(p); out=[]
    for b in s["blocks"]:
        if b["type"] in ("code","branch"): out += b["insts"]
        elif b["type"]=="sample5" and not b["sample"]["nondep"]: out.append(b["sample"]["smp"])
    return out
def count(paths):
    c = collections.Counter()
    for p in paths:
        try: ws = words_of(p)
        except Exception: continue
        for lo,hi in ws:
            x = hw.decode(lo,hi)
            if x: c[key(x)] += 1
    return c
S = sys.argv[1]
ours = count([p for d in ['y_c1p','q_c1','y_f16','y_mcp/f','y_mcp/v','q_sys','q_ix'] for p in glob.glob(f'{S}/out/{d}/*.usp')])
corp = count(glob.glob('corpus/c1/*.usp')+glob.glob('corpus/c2/*.usp')+glob.glob('corpus/c3/*.usp'))
# hwt: assemble each test's lines
hwtc = collections.Counter()
import importlib
for f in sorted(glob.glob('hwt_p*.py')):
    txt = open(f).read()
    for line in re.findall(r'"([a-z][a-z0-9.]*\s[^"\n]*)"', txt):
        try:
            x = hw.decode(*hw.encode(hw.parse(line)))
            if x: hwtc[key(x)] += 1
        except Exception: pass
print(f"{'form':22} {'ours(A/B hw)':>12} {'DDK corpus':>10} {'hwt':>5}")
for k in sorted(ours, key=lambda k: -ours[k]):
    print(f"{k:22} {ours[k]:12} {corp.get(k,0):10} {hwtc.get(k,0):5}")
