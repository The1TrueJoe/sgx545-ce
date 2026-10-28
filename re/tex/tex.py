"""SGX545 texture memory layout: linear texels -> the bytes the TSP reads.

Everything here is measured from the DDK's own uploads (re/tex/TEX.md):

- twiddled (T1 layout 0, power-of-two sizes): square tiles of
  t = min(w, h, 16) texels, Morton order inside a tile with y in the lowest
  bit (index bits y0 x0 y1 x1 ...), the tiles in row-major order;
- not a power of two (T1 layout 3): linear rows, top row first, the pitch
  rounded up to 8 texels;
- each mip level follows the previous one, levels down to 1x1;
- texel bytes are the GL upload's: RGBA8888 is R, G, B, A in memory, RGB565
  and LA88 are the GL ushort, L8/A8 the byte; RGB888 is expanded to RGBA8888
  (A = 0xff); RGBA4444 is stored as ARGB4444 (the ushort rotated right by 4)
  and RGBA5551 as ARGB1555 (rotated right by 1).

    import tex
    data = tex.encode(rgba_bytes, w, h, "rgba8888", levels=[...])
"""

BPP = {"rgba8888": 4, "rgb888": 4, "bgra8888": 4, "rgb565": 2, "rgba4444": 2, "rgba5551": 2,
       "la88": 2, "l8": 1, "a8": 1}


def log2(n):
    assert n > 0 and n & (n - 1) == 0, f"{n} is not a power of two"
    return n.bit_length() - 1


TILE = 16


def twiddle(x, y, w, h):
    """Texel index of (x, y) in a twiddled w x h (both powers of two) image."""
    n = min(log2(w), log2(h), log2(TILE))
    i = 0
    for b in range(n):
        i |= (y >> b & 1) << (2 * b) | (x >> b & 1) << (2 * b + 1)
    tiles_x = w >> n
    return i | ((y >> n) * tiles_x + (x >> n)) << (2 * n)


def level_size(w, h, lv):
    return max(1, w >> lv), max(1, h >> lv)


def level_bytes(w, h, bpp):
    if is_pot(w) and is_pot(h):
        return w * h * bpp
    return pitch(w) * h * bpp


def level_offset(w, h, bpp, lv):
    """Byte offset of mip level lv from the texture base."""
    o = 0
    for k in range(lv):
        o += level_bytes(*level_size(w, h, k), bpp)
    return o


def texel_offset(x, y, w, h, bpp, lv=0):
    """Byte offset of texel (x, y) of level lv of a w x h texture."""
    lw, lh = level_size(w, h, lv)
    if is_pot(lw) and is_pot(lh):
        i = twiddle(x, y, lw, lh)
    else:
        i = y * pitch(lw) + x
    return level_offset(w, h, bpp, lv) + i * bpp


def is_pot(n):
    return n > 0 and n & (n - 1) == 0


def pitch(w):
    """Row pitch in texels of a linear (non-power-of-two) texture."""
    return (w + 7) & ~7


def encode_level(src, w, h, bpp):
    """One level from linear bytes (rows top to bottom, w * bpp bytes each)."""
    if not (is_pot(w) and is_pot(h)):
        p = pitch(w)
        out = bytearray(p * h * bpp)
        for y in range(h):
            out[y * p * bpp:(y * p + w) * bpp] = src[y * w * bpp:(y + 1) * w * bpp]
        return bytes(out)
    out = bytearray(w * h * bpp)
    for y in range(h):
        for x in range(w):
            s = (y * w + x) * bpp
            d = twiddle(x, y, w, h) * bpp
            out[d:d + bpp] = src[s:s + bpp]
    return bytes(out)


def convert(fmt, v):
    """A GL 16-bit texel -> the value in memory."""
    if fmt == "rgba4444":
        return (v >> 4 | v << 12) & 0xFFFF
    if fmt == "rgba5551":
        return (v >> 1 | v << 15) & 0xFFFF
    return v


def encode(levels, w, h, fmt):
    """levels: linear bytes of level 0, 1, ... in the GL upload format -> texture memory."""
    bpp = BPP[fmt]
    out = bytearray()
    for lv, src in enumerate(levels):
        lw, lh = level_size(w, h, lv)
        if fmt == "rgb888":
            src = b"".join(src[3 * i:3 * i + 3] + b"\xff" for i in range(lw * lh))
        elif fmt in ("rgba4444", "rgba5551"):
            src = b"".join(convert(fmt, src[i] | src[i + 1] << 8).to_bytes(2, "little")
                           for i in range(0, 2 * lw * lh, 2))
        out += encode_level(src, lw, lh, bpp)
    return bytes(out)
