"""Generate SGX545 TA/3D state words from parameters, using only the formulas in STATE.md.

    python3 tools/state/build.py --check CAPTURE.bin [--w 720 --h 480] [GL state options]

--check rebuilds, from the GL-level parameters, every word the next step has to
author (render-target numbers, PB descriptor and free list, the triangle's TA state
block, the PBE words, the HW render context) and compares them with the capture.
A mismatch means STATE.md is missing something.
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "decode"))
import cmdta  # noqa: E402
import memmap  # noqa: E402
import pvrlog  # noqa: E402


def f32(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


# ---------------------------------------------------------------- render target (SGXAddRenderTarget)

def rt_numbers(w, h):
    xt, yt = (w + 31) >> 5, (h + 15) >> 4
    if w * h < 0x4b000:
        mode, mx, my = 0, (((xt + 1) >> 1) + 3) & ~3, (((yt + 1) >> 1) + 3) & ~3
        xs, ys = [mx] * 3, [my] * 3
    else:
        mode, mx, my = 1, (((xt + 3) >> 2) + 3) & ~3, (((yt + 3) >> 2) + 3) & ~3
        xs, ys = [mx, 2 * mx, 3 * mx], [my, 2 * my, 3 * my]
    n = 2 if mode == 0 else 4
    padx, pady = mx * n, my * n
    big = 1 << max(padx - 1, pady - 1).bit_length()
    return {
        "te_aa": 0x20000000,
        "mtile1": mode << 31 | xs[0] << 22 | xs[1] << 12 | xs[2],
        "mtile2": ys[0] << 22 | ys[1] << 12 | ys[2],
        "te_screen": (yt - 1) << 12 | (xt - 1),
        "mt_tiles": mx * my,
        "screen": (h - 1) << 16 | (w - 1),
        "region_bytes": padx * pady * 12,
        "tailptr_bytes": big * big * 4,
        "xt": xt,
    }


# ---------------------------------------------------------------- parameter buffer

def pb_descriptor(pages, freelist_va, blocklist_va):
    last = pages - 1
    zls = (pages - 0x22) & 0xfffff
    ta = pages + 0x10
    return [0, pages, last << 16 | 0, (last - 1) << 16, freelist_va, 0, last << 16, (last - 1) << 16,
            0, 0, ta, zls, zls, min(zls, ta) - 0x100 if min(zls, ta) > 0x100 else 0, blocklist_va]


def free_list(pages):
    return [((i - 1) & 0xffff) << 16 | ((i + 1) if i + 1 < pages else 0xffff) for i in range(pages)]


# ---------------------------------------------------------------- ISP / MTE (libGLESv2 FUN_00049a00)

def ispa(depth_func=None, depth_write=False, stencil=None, cmask=0xf, passtype=0):
    v = 0x200
    if depth_func is None:
        v |= 7 << 22 | 0x100000
    else:
        v |= (depth_func - 0x200) << 22 | (0 if depth_write else 0x100000)
    if cmask == 0:
        v |= 0x200000
        passtype = 0
    v |= passtype << 25
    if stencil:
        v |= 0x100 | (stencil["ref"] & 0xff)
    return v


def ispb(cmask=0xf, line_width=1.0, polyoff=False):
    return min(int(round(16 * line_width)), 255) << 8 | cmask << 4 | (0x800000 if polyoff else 0)


SOPS = {0x1e00: 0, 0: 1, 0x1e01: 2, 0x1e02: 3, 0x1e03: 4, 0x150a: 5, 0x8507: 6, 0x8508: 7}


def ispc(func, mask, sfail, zfail, zpass, wmask):
    return ((func - 0x200) << 25 | SOPS[sfail] << 22 | SOPS[zfail] << 19 | SOPS[zpass] << 16
            | (mask & 0xff) << 8 | (wmask & 0xff))


def mte_ctrl(cull=None, front=0x901):
    if cull is None:
        return 0x1555620
    if front == 0x901:
        return 0x1555620 | {0x404: 2, 0x405: 1}.get(cull, 0)
    return 0x1555620 | {0x404: 1, 0x405: 2}.get(cull, 0)


def viewport(x, y, w, h, surf_h, n=0.0, f=1.0, pbuffer=False):
    sx, sy = w * 0.5, h * 0.5
    if pbuffer:
        yo, ys = y + sy, sy
    else:
        yo, ys = surf_h - (y + sy), -sy
    return [f32(x + sx), f32(sx), f32(yo), f32(ys), f32((n + f) * 0.5), f32((f - n) * 0.5)]


def pds_word(addr, data_bytes):
    return (data_bytes // 16) << 26 | (addr - 0x2e000000) >> 4


def ta_block(isp_a, isp_b, pds, vp, outsel, mte, tsp=(0, 0), isp_c=None):
    """The triangle form: ISPA ISPB [ISPC] PDS viewport w0 output-select wclamp MTE-ctrl TSP."""
    hdr = 0x1 | 0x2 | 0x40 | 0x100 | 0x200 | 0x400 | 0x800 | 0x1000 | 0x4000 | 0x8000
    words = [isp_a, isp_b]
    if isp_c is not None:
        hdr |= 0x4
        words.append(isp_c)
    words += list(pds) + list(vp) + [0, outsel, 0x3727c5ac, mte] + list(tsp)
    return [hdr] + words


# ---------------------------------------------------------------- PBE (libGLESv2 FUN_0004e7a0)

def pbe(w, h, stride_px, addr, fmt_code=0, dither=True, scanout=False):
    w0 = 0
    w1 = (h - 1) << 16 | (w - 1)
    w2 = ((stride_px - 1) >> 1) << 16 | (0 if scanout else 0x8000)
    w3 = addr & ~3
    side = 0x200 | (fmt_code & 0x3f) | (0x400 if dither else 0)
    return [w0, w1, w2, w3], side


def limm(v, reg):
    return v & 0x1fffff | reg << 21, 0xfca00000 | (v >> 21 & 0x1f) << 4 | (v >> 26) << 12


def eot_program(pbe_words, side):
    """FUN_000301d0: header, 4 x LIMM, EMITPIX, EMITPIX(sideband); (lo, hi) pairs."""
    out = [(0, 0xfa440700)]
    out += [limm(v, r) for r, v in enumerate(pbe_words)]
    out.append((0x00200080, 0xfb200100))
    out.append(((side & 0x3f) << 22 | 0x00208180, 0xfb240000 | (side & 0x3000) << 10 | (side >> 3 & 0x1f8)))
    return out


# ---------------------------------------------------------------- HW render context (SGXCreateRenderContext)

def hw_render_context(ccb, ccb_ctl, pb_desc, scratch, pid):
    w = [0] * 17
    w[0], w[3], w[4], w[6], w[7], w[16] = 0x21, ccb, ccb_ctl, pb_desc, scratch, pid
    return w


# ---------------------------------------------------------------- check against a capture

def check(path, a):
    log = pvrlog.load(path)
    mm = memmap.build(log)
    k = cmdta.kicks(log)[0]
    snaps = [s for s in log.of(pvrlog.Snap) if s.reason == "kick" and s.kick == 1]
    mem = memmap.Memory(mm, snaps)
    _, ccb, off, _ = cmdta.cmd_bytes(log, mm, k, 1, 4)
    base = ccb.devva + off - 0x58
    hdr, tail = mem.words(base, 0x16), mem.words(base + 0x238, 0x1c)
    ok = True

    def cmp(name, want, got):
        nonlocal ok
        good = want == got
        ok &= good
        print(f"{'ok ' if good else 'BAD'} {name}: " + (" ".join(f"{x:08x}" for x in want) if good else
              "want " + " ".join(f"{x:08x}" for x in want) + " got " + " ".join(f"{x:08x}" for x in got)))

    rt = rt_numbers(a.w, a.h)
    cmp("TE AA, MTILE1, MTILE2, TE screen, MT tiles", [rt["te_aa"], rt["mtile1"], rt["mtile2"], rt["te_screen"],
        rt["mt_tiles"]], tail[0:5])
    cmp("screen size", [rt["screen"]], [tail[12]])
    pbd = hdr[0x24 // 4]
    got = mem.words(pbd, 15)
    want = pb_descriptor(got[1], got[4], got[14])
    cmp("PB descriptor", want, got)
    fl = mem.words(got[4], got[1])
    cmp("free list (first 4, last 2)", free_list(got[1])[:4] + free_list(got[1])[-2:], fl[:4] + fl[-2:])
    # the triangle's TA state block: the VDM stream's second state program
    v = tail[6]
    blocks = []
    for _ in range(64):
        w = mem.u32(v)
        if w >> 28 == 4:
            blocks.append((w & 0xfffffff) << 4)
            v += 8
        elif w >> 28 == 8:
            v += 24
        else:
            break
    src = mem.u32(blocks[1])                        # ds0[0] of the state program = the DMA source
    n = (mem.u32(src) & 0x4) and 19 or 18
    cap = mem.words(src, n)
    pds = cap[(3 if n == 18 else 4):(6 if n == 18 else 7)]
    st = None
    if a.stencil:
        s = [int(x, 0) for x in a.stencil.split()]
        st = {"ref": s[1]}
    want = ta_block(ispa(a.depth_func, a.depth_write, st, a.cmask, a.passtype),
                    ispb(a.cmask, 1.0, a.polyoff),
                    pds, viewport(*a.viewport, a.h, *a.depthrange, pbuffer=a.pbuffer), a.outsel << 16 | 0x3000,
                    mte_ctrl(a.cull, a.front), (a.tsp0, a.tsp1),
                    ispc(s[0], s[2], s[3], s[4], s[5], s[6]) if a.stencil else None)
    cmp("triangle TA state block (PDS words taken from the capture)", want, cap)
    rd = hdr[0x38 // 4]
    ev = mem.u32(rd + 0x5c)
    eot_w0 = mem.u32(ev + 0xc)                      # pixel event ds1[1]: DOUTU word of the EOT program
    eot = 0x37000000 + (eot_w0 & 0xf) * 0x80000 + ((eot_w0 >> 4) & 0x3ffff) * 8
    pw, side = pbe(a.w, a.h, a.stride or (a.w + 31) & ~31, a.addr or 0, a.fmt,
                   not a.nodither)
    got = mem.words(eot, 14)
    gotp = [(got[2 * i], got[2 * i + 1]) for i in range(7)]
    if not a.addr:                                   # the address is the capture's
        pw[3] = (gotp[4][0] & 0x1fffff) | ((gotp[4][1] >> 4) & 0x1f) << 21 | (gotp[4][1] >> 12 & 0x3f) << 26
    want = [x for p in eot_program(pw, side) for x in p]
    cmp("end-of-tile program (PBE words)", want, got)
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", required=True)
    ap.add_argument("--w", type=int, default=720)
    ap.add_argument("--h", type=int, default=480)
    ap.add_argument("--stride", type=int)
    ap.add_argument("--addr", type=lambda x: int(x, 0))
    ap.add_argument("--fmt", type=int, default=0)
    ap.add_argument("--nodither", action="store_true")
    ap.add_argument("--pbuffer", action="store_true")
    ap.add_argument("--depth-func", type=lambda x: int(x, 0))
    ap.add_argument("--depth-write", type=int, default=1)
    ap.add_argument("--depthrange", type=float, nargs=2, default=(0.0, 1.0))
    ap.add_argument("--stencil", help="'func ref mask sfail zfail zpass wmask' as in PROBE_STENCIL")
    ap.add_argument("--cmask", type=lambda x: int(x, 0), default=0xf)
    ap.add_argument("--passtype", type=int, default=0)
    ap.add_argument("--polyoff", action="store_true")
    ap.add_argument("--cull", type=lambda x: int(x, 0))
    ap.add_argument("--front", type=lambda x: int(x, 0), default=0x901)
    ap.add_argument("--viewport", type=float, nargs=4)
    ap.add_argument("--outsel", type=int, default=4, help="vertex output dwords")
    ap.add_argument("--tsp0", type=lambda x: int(x, 0), default=0)
    ap.add_argument("--tsp1", type=lambda x: int(x, 0), default=0)
    a = ap.parse_args()
    if a.viewport is None:
        a.viewport = (0, 0, a.w, a.h)
    sys.exit(0 if check(a.check, a) else 1)


if __name__ == "__main__":
    main()
