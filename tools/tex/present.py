"""The transfer queue's blit (eglSwapBuffers' present, the GLES texture upload),
built from formulas instead of copied from a capture.

A transfer is a 3D render with no geometry: only the background object, whose
pixel program samples the source surface (one DOUTT, `mov o0, pa0`), and an
end-of-tile program whose PBE words name the destination. libsrv_um builds it
in FUN_0002ca20 (the command), FUN_0002d440 (the tile clip) and the pixel /
end-of-tile programs in the transfer context's own buffers.

    python3 present.py --check CAPTURE.bin [--xfer N]     compare with the DDK's words
    python3 present.py --blit SRC,W,H,PITCH DST,PITCH      mkreplay --poke arguments that turn the
                                                          capture's swap into this blit (EGL's
                                                          transfer context buffers, see TEX.md)

Every word here is checked against the swap and the texture-upload transfers of
the tools/tex captures (see TEX.md).
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "decode"))

PDS_BASE = 0x2E000000
USE_BASE = 0x37000000


def limm(reg, v):
    """LIMM rN, #v (libsrv_um FUN_000301d0's formula; STATE.md section 4)."""
    lo = (v & 0x1FFFFF) | reg << 21
    hi = 0xFCA00000 | (v >> 21 & 0x1F) << 4 | (v >> 26) << 12
    return lo, hi


def pbe(w, h, stride, addr, fmt=0, twiddled=False, dither=False, x0=0, y0=0, scanout=False):
    """The four PBE words and the sideband word (libGLESv2 FUN_0004e7a0).

    stride is in pixels; fmt is the PBE format code (0 BGRA8888, 1 RGBA8888,
    3 RGB565, 4 RGBA5551, 5 RGBA4444); twiddled sets sideband b13.
    """
    w0 = y0 << 16 | x0
    w1 = (y0 + h - 1) << 16 | (x0 + w - 1)
    w2 = (stride - 1 >> 1) << 16 | (0 if scanout else 0x8000)
    w3 = addr & ~3
    side = 0x200 | fmt & 0x3F | (0x2000 if twiddled else 0) | (0x400 if dither else 0)
    return [w0, w1, w2, w3], side


def eot_program(words, side):
    """The end-of-tile USSE program: phas, LIMM r0-r3, EMITPIX (PBE words), EMITPIX (sideband)."""
    ins = [(0x00000000, 0xFA440700)]
    ins += [limm(r, v) for r, v in enumerate(words)]
    ins.append((0x00200080, 0xFB200100))
    ins.append(((side & 0x3F) << 22 | 0x00208180, 0xFB240000 | (side & 0x3000) << 10 | (side >> 3 & 0x1F8)))
    return [x for lo_hi in ins for x in lo_hi]


def strided_tex(w, h, stride, addr, fmt=0x0C):
    """DOUTT words of a strided source (T1 layout 6): T0 carries stride-1 in b9-11 (low 3) and b17-26."""
    s = stride - 1
    t0 = (s & 7) << 9 | (s >> 3) << 17
    t1 = 6 << 29 | fmt << 24 | (w - 1) << 12 | (h - 1)
    return [t0, t1, addr, 0xA0800000]


def twiddled_tex(w, h, addr, fmt=0x0D):
    """DOUTT words of a twiddled source (glGenerateMipmap's previous level): clamp, nearest, no mips."""
    return [0x001E0090, fmt << 24 | (w - 1) << 12 | (h - 1), addr, 0xA0800000]


def pixel_program(copy_use, tex):
    """The background object's pixel PDS program: DOUTU (copy shader), DOUTI, DOUTT (the source)."""
    douto = ((copy_use - USE_BASE) >> 3) << 4
    data = [douto, 0x02000004, 0, 0,
            0x40012001, 0, tex[2], tex[3],
            tex[0], tex[1], 0, 0]
    code = [0x07000305, 0x07040202, 0x07082364, 0xAF000000]
    return data + code


def bg_block(w, h, primary, secondary, prows=3, srows=1):
    """The background object block (libsrv_um FUN_0001f990's layout plus the PDS triple)."""
    b = [0] * 24
    b[0] = srows << 26 | (secondary - PDS_BASE) >> 4
    b[1] = 0x00030001
    b[2] = prows << 26 | (primary - PDS_BASE) >> 4
    b[5] = b[8] = 0x3F800000
    b[10] = 0x202
    b[15] = 0x100000
    b[16] = 0x1000
    b[18] = ((w + 0x1000) * 0x100) << 8 | 0x10
    b[20] = (h + 0x1000) * 0x100
    b[21] = 0x1000
    return b


def command(bgblock, w, h, event, event_rows=5, event_code=1, base=None):
    """The transfer command's client words 0-15 (libsrv_um FUN_0002ca20 + FUN_0002d440)."""
    base = bgblock & 0xFFF00000 if base is None else base
    c = [0] * 16
    c[0] = 1 << 25 | (bgblock - base) >> 4
    c[1] = 0x200
    c[2] = 0x3F800000   # background depth
    c[3] = 3
    c[5] = 0x100
    c[6] = base
    c[7] = 0x80000000
    c[8] = 0x88
    c[9] = event & ~0xF
    c[10] = event_rows
    c[11] = event_code
    c[12] = 0                                   # first tile x<<16 | y
    c[13] = ((w + 31 >> 5) - 1) << 16 | ((h + 15 >> 4) - 1)
    c[14] = 0x80
    c[15] = PDS_BASE
    return c


# EGL's swap transfer context in the probe captures (TEX.md section 3)
SWAP = dict(ccb=0x32031000, bg=0x3E107000, sec=0x2E003000, prim=0x2E005000, event=0x2E003020,
            copy=0x370018E0, eot=0x37002000)


def blit_pokes(src, w, h, spitch, dst, dpitch, fmt=0, ctx=SWAP):
    """(dev addr, words) that make the context's transfer copy w x h BGRA8888 from src to dst."""
    words, side = pbe(w, h, dpitch, dst, fmt=fmt)
    tex = strided_tex(w, h, spitch, src)
    return [
        (ctx["ccb"], command(ctx["bg"], w, h, ctx["event"])),
        (ctx["bg"], bg_block(w, h, ctx["prim"], ctx["sec"])),
        (ctx["prim"], pixel_program(ctx["copy"], tex)),
        (ctx["eot"], eot_program(words, side)),
    ]


def check(path, n):
    import memmap
    import pvrlog

    log = pvrlog.load(path)
    mm = memmap.build(log)
    mem = memmap.Memory(mm, [s for s in log.of(pvrlog.Snap) if s.reason == "xfer" and s.kick == n])
    r = [r for r in log.of(pvrlog.Bridge) if r.name == "SGX_SUBMITTRANSFER"][n - 1]
    ccb_h, off = struct.unpack_from("<II", r.inp, 8)
    ccb = next(b for b in mm.bufs if b.kmeminfo == ccb_h)
    cmd = mem.words(ccb.devva, 16)
    bg = cmd[6] + ((cmd[0] & 0x1FFFFFF) << 4)
    blk = mem.words(bg, 24)
    prim = PDS_BASE + ((blk[2] & 0x3FFFFFF) << 4)
    sec = PDS_BASE + ((blk[0] & 0x3FFFFFF) << 4)
    pp = mem.words(prim, 16)
    tex = [pp[8], pp[9], pp[6], pp[7]]
    sw, sh = (tex[1] >> 12 & 0xFFF) + 1, (tex[1] & 0xFFF) + 1
    strided = tex[1] >> 29 == 6
    sstride = ((tex[0] >> 9 & 7) | (tex[0] >> 17 & 0x3FF) << 3) + 1 if strided else sw
    copy = USE_BASE + ((pp[0] >> 4) << 3)
    ev = mem.words(cmd[9], 4)
    eot = USE_BASE + ((ev[0] & 0xFFFF) >> 4 << 3)
    e = mem.words(eot, 14)
    words = [(lo & 0x1FFFFF) | (hi >> 4 & 0x1F) << 21 | (hi >> 12 & 0x3F) << 26 for lo, hi in zip(e[2:10:2], e[3:10:2])]
    dw, dh = (words[1] & 0xFFFF) + 1, (words[1] >> 16) + 1
    dstride = ((words[2] >> 16 & 0xFFF) + 1) * 2
    side = (e[12] >> 22 & 0x3F) | (e[13] >> 10 & 0x3000) | (e[13] & 0x1F8) << 3
    print(f"transfer {n}: source {sw}x{sh} {'stride ' + str(sstride) if strided else 'twiddled'} at {tex[2]:#x} -> "
          f"dest {dw}x{dh} stride {dstride} at {words[3]:#x}, side {side:#x}")
    want_pbe, want_side = pbe(dw, dh, dstride, words[3], fmt=side & 0x3F, twiddled=bool(side & 0x2000),
                              dither=bool(side & 0x400), scanout=not words[2] & 0x8000)
    want_cmd = command(bg, dw, dh, cmd[9])
    if not strided:
        # glGenerateMipmap keeps the level-0 tile clip for every level; the PBE clip bounds the writes
        covers = cmd[13] >> 16 >= want_cmd[13] >> 16 and cmd[13] & 0xFFFF >= want_cmd[13] & 0xFFFF
        print(f"  (tile clip word 13 = {cmd[13]:#x}, the level-0 size; covers the destination: {covers})")
        if covers:
            want_cmd[13] = cmd[13]
    results = [
        ("command words 0-15", want_cmd, cmd),
        ("background block", bg_block(dw, dh, prim, sec), blk),
        ("source DOUTT words", strided_tex(sw, sh, sstride, tex[2], tex[1] >> 24 & 0x1F) if strided
         else twiddled_tex(sw, sh, tex[2], tex[1] >> 24 & 0x1F), tex),
        ("pixel PDS program", pixel_program(copy, tex), pp),
        ("end-of-tile program", eot_program(want_pbe, want_side), e),
    ]
    bad = 0
    for what, want, got in results:
        diff = [i for i, (a, b) in enumerate(zip(want, got)) if a != b]
        bad += len(diff)
        print(f"  {what}: {len(want) - len(diff)}/{len(want)} words match"
              + "".join(f"\n    [{i}] built {want[i]:#010x} DDK {got[i]:#010x}" for i in diff))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check")
    ap.add_argument("--xfer", type=int, action="append")
    ap.add_argument("--blit", nargs=2, metavar=("SRC,W,H,PITCH", "DST,PITCH"))
    a = ap.parse_args()
    if a.blit:
        src, w, h, sp = (int(x, 0) for x in a.blit[0].split(","))
        dst, dp = (int(x, 0) for x in a.blit[1].split(","))
        print(" ".join(f"--poke {va:#x}:" + ",".join(f"{x:#x}" for x in ws)
                       for va, ws in blit_pokes(src, w, h, sp, dst, dp)))
        return
    if a.check:
        import pvrlog
        n = sum(r.name == "SGX_SUBMITTRANSFER" for r in pvrlog.load(a.check).of(pvrlog.Bridge))
        bad = sum(check(a.check, k) for k in (a.xfer or range(1, n + 1)))
        sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
