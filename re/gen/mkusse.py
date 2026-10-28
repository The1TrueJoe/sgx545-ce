"""Assemble the generator's fixed USSE programs into a C header (re/gen/usse_progs.h).

    python3 re/gen/mkusse.py > re/gen/usse_progs.h

Every program is source text in re/isa's syntax (what isa/dis.py prints), put
through isa/hw.py's encoder: no instruction word is copied from a capture.
`name:` starts a label; `@name` as the first operand of `phas.i` is that
label's instruction index from the program start. The C side adds the
program's offset from its code heap (in instructions) to those words, which
are listed in <prog>_reloc (phas.i's pc field is l0-17, absolute; ISA.md).

The programs are the ones a GL frame needs on top of the shaders (see
re/gen/GEN.md): the TA state-emit programs (one per state-block length), the
empty task, the clear's vertex and pixel programs, the background object's
pixel program, the pixel-event tasks, and the shaders the frame program
draws with.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "isa"))
import hw  # noqa: E402

# The prologue every vertex/state task the DDK builds starts with. test.and.ne
# of #0 and #0 never sets p0, so the predicated load never runs; kept because
# the DDK always emits it (a hardware-erratum workaround is the likely reason).
PROLOGUE = [
    "nop [nosched]",
    "test.and.ne p0, #0, #0 [skipinv nodst=0]",
    "(p0) ld.a.d pa0, pa0, #0 [skipinv rpt=4]",
    "wdf",
]


# The last phase of every vertex program: the finalizer's epilogue. The phase
# before it ends with emit.t3 (the vertex's outputs); this one sends emit.t1
# with f3_20, as the state-emit programs end. A vertex program's earlier phase
# points here with phas.i; without it the task runs on into whatever follows.
VS_EPILOGUE = [
    "phas.i #0, #0 [wait=7]",
    "emit.t1 r0, #0, #0 [end f12 f10 f3_20]",
]


def state_emit(n):
    """Copy n DMA'd state dwords (pa0..) to the outputs and emit them to the MTE.
    mov takes at most rpt=16, so longer blocks take several movs."""
    lines = ["phas.i #0, #0 [wait=7]"] + PROLOGUE
    r = 0
    while r < n:
        k = min(16, n - r)
        lines.append(f"mov o{r}, pa{r} [skipinv rcnt rpt={k}]")
        r += k
    return lines + ["emit.t3 r0, #0, #0", "op0xd6 #0", "emit.t1 o0, #0, #0 [end f3_20]"]


PROGS = {
    # a task that does nothing (empty secondary updates, vertex and pixel)
    "empty": ["phas.i #0, #0 [end wait=7]"],

    # clear: screen-space x, y, z and an ARGB8888 colour per vertex (16 bytes)
    # -> position (w = c3 = 1.0) and the colour as four floats 0..1
    "clear_vs": [
        "phas.i @end, #0 [wait=2]",
    ] + PROLOGUE + [
        "mov o0, pa0 [skipinv rcnt rpt=3]",
        "mov o3, c3 [skipinv rcnt]",
        "pck.f32.u8 o4, pa3.l2, pa3.l2 [skipinv rcnt scale]",
        "pck.f32.u8 o5, pa3.l1, pa3.l1 [skipinv rcnt scale]",
        "pck.f32.u8 o6, pa3, pa3 [skipinv rcnt scale]",
        "pck.f32.u8 o7, pa3.l3, pa3.l3 [skipinv rcnt scale]",
        "emit.t3 r0, #0, #0 [end f12 f10]",
        "end:",
    ] + VS_EPILOGUE,
    # the iterated colour (PDS DOUTI colour x4) is already the packed pixel
    "copy_fs": [
        "phas.i #0, #0 [wait=7]",
        "mov o0, pa0 [skipinv end rcnt]",
    ],

    # vertex shader: gl_Position = vec4(a0.xy, 0, 1). Phase 0 fills pa2/pa3,
    # phase 1 copies pa0-pa3 out.
    "vs_xy": [
        "phas.i @p1, #0",
    ] + PROLOGUE + [
        "limm pa3, #0x3f800000 [skipinv]",
        "limm pa2, #0 [skipinv end]",
        "p1:",
        "phas.i @p2, #0 [wait=2]",
        "mov o0, pa0 [skipinv rcnt rpt=4]",
        "emit.t3 r0, #0, #0 [end f12 f10]",
        "p2:",
    ] + VS_EPILOGUE,
    # gl_Position = vec4(a0.xyz, 1)
    "vs_xyz": [
        "phas.i @p1, #0",
    ] + PROLOGUE + [
        "limm pa3, #0x3f800000 [skipinv end]",
        "p1:",
        "phas.i @p2, #0 [wait=2]",
        "mov o0, pa0 [skipinv rcnt rpt=4]",
        "emit.t3 r0, #0, #0 [end f12 f10]",
        "p2:",
    ] + VS_EPILOGUE,

    # fragment shader gl_FragColor = c (mediump uniform vec4): the secondary
    # program packs the FP16 uniform (DMA'd to pa9, pa10) to BGRA8888 in pa9,
    # which the main program reads as sa9
    "fs_uniform_sec": [
        "phas.i #0, #0 [wait=7]",
        "nop [nosched]",
        "test.and.ne p0, #0, #0 [skipinv]",
        "(p0) ld.a.d pa0, pa0, #0 [skipinv moe]",
        "wdf",
        "pck.u8.f16 pa9, pa9.l2, pa9 [skipinv dmask=6 scale]",
        "pck.u8.f16 pa9, pa10, pa10.l2 [skipinv end dmask=9 scale]",
    ],
    "fs_uniform": [
        "phas.i #0, #0 [wait=7]",
        "mov o0, sa9 [skipinv end mask=1]",
    ],

    # pixel event tasks (the pixel-event PDS program picks one per event)
    "pe_ptoff": [
        "phas.i #0, #0 [wait=7]",
        "ptoff [end]",
    ],
    "pe_emit": [
        "phas.i #0, #0 [wait=7]",
        "limm r0, #0 [skipinv]",
        "emit.t0 r0, #0, r0 [end f3_20 imm=512]",
    ],
}

for n in range(1, 33):
    PROGS[f"state_emit_{n}"] = state_emit(n)


def assemble(lines):
    labels, insts = {}, []
    for ln in lines:
        if ln.endswith(":"):
            labels[ln[:-1]] = len(insts)
        else:
            insts.append(ln)
    words, reloc = [], []
    for i, ln in enumerate(insts):
        if "@" in ln:
            lab = ln.split("@", 1)[1].split(",", 1)[0].strip()
            ln = ln.replace("@" + lab, f"#{labels[lab]}")
            reloc.append(i)
        lo, hi = hw.encode(hw.parse(ln))
        x = hw.decode(lo, hi)
        if x is None or x.unknown:
            sys.exit(f"mkusse: {ln!r} does not round-trip")
        words += [lo, hi]
    return words, reloc


def main():
    out = ["/* Generated by re/gen/mkusse.py from USSE source text; do not edit. */",
           "#ifndef USSE_PROGS_H", "#define USSE_PROGS_H", "#include <stdint.h>", "",
           "struct usse_prog { const char *name; const uint32_t *w; unsigned n; const uint8_t *reloc; unsigned nreloc; };",
           ""]
    names = []
    for name, lines in PROGS.items():
        words, reloc = assemble(lines)
        c = name
        out.append(f"/* {name}:")
        for ln in lines:
            out.append(f" *   {ln}")
        out.append(" */")
        out.append(f"static const uint32_t usse_{c}_w[] = {{")
        for i in range(0, len(words), 2):
            out.append(f"\t0x{words[i]:08x}, 0x{words[i + 1]:08x},")
        out.append("};")
        if reloc:
            out.append(f"static const uint8_t usse_{c}_reloc[] = {{ {', '.join(map(str, reloc))} }};")
        out.append(f"static const struct usse_prog usse_{c} = {{ \"{name}\", usse_{c}_w, {len(words) // 2}, "
                   f"{f'usse_{c}_reloc' if reloc else '0'}, {len(reloc)} }};")
        out.append("")
        names.append(c)
    out.append("static const struct usse_prog *const usse_state_emit[33] = { 0,")
    out.append("\t" + ", ".join(f"&usse_state_emit_{n}" for n in range(1, 33)))
    out.append("};")
    out += ["", "#endif", ""]
    sys.stdout.write("\n".join(out))


if __name__ == "__main__":
    main()
