"""Per-kick VDM draw statistics from a pvrtrace capture (the DDK's or ours).

    python3 tools/state/vdmstats.py CAPTURE.bin [--draws]

For every kick that has a snapshot it walks the VDM control stream and prints
the draw count, the sum of the index counts, the unique vertices the indices
name, and the distinct values of draw words 2, 3, 5 and the vertices-per-task
nibble (word 4 b28-31). Kicks are grouped into scenes (TA flags b1 ends one).
--draws adds one line per draw. With PVRTRACE_SNAPFILE a long client's capture
holds only the window of kicks after the trigger.
"""
import argparse
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "decode"))
import cmdta  # noqa: E402
import memmap  # noqa: E402
import pvrlog  # noqa: E402


def walk(mem, va, limit=1 << 20):
    draws, other = [], collections.Counter()
    for _ in range(limit):
        w = mem.u32(va)
        if w is None:
            other["unreadable"] += 1
            break
        t = w >> 28
        if t == 0xc:
            break
        if t in (4, 6):
            va += 8
            continue
        if t in (8, 9):
            ws = mem.words(va, 6)
            if ws is None or len(ws) < 6:
                other["short draw"] += 1
                break
            draws.append(ws)
            va += 24
            continue
        other[f"word {w:#010x}"] += 1
        break
    return draws, other


def indices(mem, d):
    w0, w1, w2 = d[0], d[1], d[2]
    n = w0 & 0x3fffff
    wide = bool(w2 & 0x1000000)
    size = 4 if wide else 2
    base = (w1 & ~3) + (w2 & 0xffffff) * size
    raw = mem.read(base, n * size)
    if raw is None or len(raw) < n * size:
        return n, None
    return n, struct.unpack(f"<{n}{'I' if wide else 'H'}", raw)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture")
    ap.add_argument("--draws", action="store_true")
    a = ap.parse_args()
    log = pvrlog.load(a.capture)
    mm = memmap.build(log)
    snapped = {s.kick for s in log.of(pvrlog.Snap) if s.reason == "kick"}
    scene = collections.Counter()
    for n, k in enumerate(cmdta.kicks(log), 1):
        if n not in snapped:
            continue
        mem, ccb, off, head = cmdta.cmd_bytes(log, mm, k, n, 8)
        base = ccb.devva + off - 0x58
        flags = mem.u32(base + 4) or 0
        vbase = mem.u32(base + 0x250)
        draws, other = walk(mem, vbase) if vbase else ([], {"no stream": 1})
        tot = uniq = 0
        w2 = collections.Counter(); w3 = collections.Counter(); w5 = collections.Counter()
        vpt = collections.Counter(); prim = collections.Counter()
        for i, d in enumerate(draws):
            cnt, idx = indices(mem, d)
            u = len(set(idx)) if idx else None
            tot += cnt
            uniq += u or 0
            w2[d[2]] += 1; w3[d[3]] += 1; w5[d[5] & 0x00c00000] += 1
            vpt[(d[4] >> 28) + 1] += 1
            prim[(d[0] >> 26) & 7] += 1
            if a.draws:
                rng = f"{min(idx)}-{max(idx)}" if idx else "?"
                print(f"  kick {n} draw {i}: count {cnt} unique {u} range {rng} prim {(d[0] >> 26) & 7} "
                      f"vpt {(d[4] >> 28) + 1} w2 {d[2]:08x} w3 {d[3]:08x} w5 {d[5]:08x}")
        print(f"kick {n}: flags {flags:#x} draws {len(draws)} indices {tot} unique {uniq} "
              f"vpt {dict(vpt)} prim {dict(prim)} w2 {[hex(x) for x in w2]} w3 {[hex(x) for x in w3]} "
              f"w5b22-23 {[hex(x) for x in w5]}" + (f" stop {dict(other)}" if other else ""))
        scene["kicks"] += 1; scene["draws"] += len(draws); scene["indices"] += tot; scene["unique"] += uniq
        if flags & 2:
            print(f"== scene: {dict(scene)}")
            scene.clear()
    if scene:
        print(f"== (open scene): {dict(scene)}")


if __name__ == "__main__":
    main()
