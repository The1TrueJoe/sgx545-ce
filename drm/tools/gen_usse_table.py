#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright © 2026 Joseph Telaak
"""
Generate the shader validator's USSE decode table (pvrsgx_usse_table.h) from
the isaspec description, mesa/src/sgx/isa/sgx.xml.

Every leaf bitset under #instruction is one form: the bits its own and its
ancestors' <pattern>s fix, its operand slots, and what the validator does
with it (POLICY). A slot is a <derived> field of operand type (#dst, #src,
#src0, #dst1), rebuilt from the pieces its expression names; a slot whose
number field the leaf's patterns fix is left out. ALLOW narrows a field to the
values listed; the generator enumerates them, one entry each. A word the
kernel accepts matches exactly one entry.

  gen_usse_table.py sgx.xml > pvrsgx_usse_table.h
  gen_usse_table.py --json sgx.xml	# the forms, for tests
"""
import itertools
import json
import re
import sys
import xml.etree.ElementTree as ET

# What the validator does with a form, by leaf name; every other form is
# checked operand by operand. Refused: what the threat model
# (Documentation/gpu/powervr_sgx.rst, "Shader validation") keeps from client
# code, and what the description names but doesn't explain.
REFUSE = (
    'ldr', 'str',				# the core's registers
    'setl', 'savl', 'lapc',			# the link register: computed jumps
    'ba',					# absolute branch
    'phas',					# register form: a computed phase address
    'lock', 'release',			# mutexes: can stall other tasks
    'smbo', 'smr', 'smoa', 'imo',		# operand stepping not understood
    'smbo.r', 'smr.r', 'smoa.r', 'smlsi.r',
    'ssum16i', 'firh', 'op16s1', 'op1adrc',	# not for this core
    'op0x98', 'op0xd7', 'op0xfd', 'op0xfe', 'op0xff', 'op0x100',
    'op0xf3.s6', 'op0xf3.s7',		# meaning unknown
    'fdual',				# its second operation's operands aren't modeled
)
POLICY = dict({n: 'USSE_REFUSE' for n in REFUSE}, **{
    'br': 'USSE_BR',			# relative target: inside the object
    'phas.i': 'USSE_PHAS',		# absolute next phase: inside the object
})

# Fields narrowed to the values the compilers emit (field: values).
ALLOW = {
    'br': {'PRED': range(7), 'LINK': [0]},	# no pn; no link (lapc is refused)
    'phas.i': {'WAIT': [0, 1, 2, 7], 'RATE': [0, 1, 2]},
    'op0xd6': {'NVAL': [0], 'F3_6': [0]},	# as the state-emit programs
    # Only the program-start state (efo's F16 off, C10 on), which no compiler
    # changes: format control is then known everywhere without following it.
    'setfc': {'A0': [0], 'A1': [1]},
}

# Field names older descriptions used (new: old).
OLD_NAMES = {'NVAL': 'N'}

# How an operand's number bit 6 reads, by the expression of its MODE
# parameter: a flag under format select (a bit of the word), a flag under C10
# format control (on, as at program start, since setfc is pinned) for
# r/o/pa/sa/i only, or part of the number. efo's F16 format control is off at
# program start, so its numbers are whole.
MODE_EXPRS = {
    None: ('USSE_NUM', None),
    '#zero': ('USSE_NUM', None),
    '#float-smode': ('USSE_SEL', 'FMT'),
    '#test-mode': ('USSE_SEL', 'FMT'),
    '#fc-c10': ('USSE_C10', None),
    '#fc-f16': ('USSE_NUM', None),
    '#efo-mode': ('USSE_NUM', None),
}

# Fields the description leaves free but only one value of which is ever
# emitted: rlp's unused src2 slot, 0x300 (ISA.md "unused2").
PINNED = {
    'U2N': 0x300 & 0x7f,
    'U2B': (0x300 >> 7) & 3,
    'U2E': 0x300 >> 9,
}

# Slots that exist only while a field is set: test writes DST only with DW.
SLOT_WHEN = {
    'test': {'DST': 'DW'},
    'testmask': {'DST': 'DW'},
}

SLOT_KINDS = {
    '#dst': 'USSE_SLOT_DST',
    '#dst1': 'USSE_SLOT_DST1',
    '#src': 'USSE_SLOT_SRC',
    '#src0': 'USSE_SLOT_SRC0',
}


def field_bits(f):
    if f.get('pos') is not None:
        return int(f.get('pos')), int(f.get('pos'))
    return int(f.get('low')), int(f.get('high'))


def width_mask(lo, hi):
    return ((1 << (hi - lo + 1)) - 1) << lo


def patterns(bs):
    mask = match = 0
    for f in bs.findall('field'):
        if f.get('name') not in PINNED:
            continue
        lo, hi = field_bits(f)
        mask |= width_mask(lo, hi)
        match |= PINNED[f.get('name')] << lo
    for p in bs.findall('pattern'):
        bits = p.text.strip()
        if p.get('pos') is not None:
            lo = hi = int(p.get('pos'))
        else:
            lo, hi = int(p.get('low')), int(p.get('high'))
        if len(bits) != hi - lo + 1:
            raise SystemExit('pattern width mismatch in %s' % bs.get('name'))
        for i, c in enumerate(reversed(bits)):
            b = 1 << (lo + i)
            if c in '01':
                if mask & b:
                    raise SystemExit('bit %d fixed twice in %s' % (lo + i, bs.get('name')))
                mask |= b
                if c == '1':
                    match |= b
    return mask, match


TERM = re.compile(r'^\(?\{(\w+)\}(?:\s*<<\s*(\d+))?\)?$')


def slot_pieces(expr, fields, name):
    pieces = []
    for t in expr.split('|'):
        m = TERM.match(t.strip())
        if not m:
            raise SystemExit('%s: operand expression %r' % (name, expr))
        if m.group(1) not in fields:
            raise SystemExit('%s: no field %s' % (name, m.group(1)))
        lo, hi = fields[m.group(1)]
        pieces.append((lo, hi - lo + 1, int(m.group(2) or 0)))
    return pieces


def leaf_forms(root):
    sets = {bs.get('name'): bs for bs in root.findall('bitset')}
    parent = {n: bs.get('extends') for n, bs in sets.items()}
    children = set(parent.values())

    def chain(n):
        while n:
            yield sets[n]
            n = parent[n]

    for n in sets:
        anc = list(chain(n))
        if n in children or anc[-1].get('name') != '#instruction':
            continue	# inner nodes, operand hierarchies
        mask = match = 0
        fields, slots, derived = {}, {}, {}
        for bs in anc:
            for d in bs.findall('derived'):
                derived.setdefault(d.get('name'), d.get('expr'))
        for bs in anc:
            m, v = patterns(bs)
            if mask & m:
                raise SystemExit('bit fixed twice along %s' % n)
            mask |= m
            match |= v
            for f in bs.findall('field'):
                fields.setdefault(f.get('name'), field_bits(f))
            for d in bs.findall('derived'):
                if d.get('type') in SLOT_KINDS:
                    e = d.find('expr')
                    mp = [p.get('name') for p in d.findall('param') if p.get('as') == 'MODE']
                    mexpr = derived.get(mp[0]) if mp else None
                    if mexpr not in MODE_EXPRS:
                        raise SystemExit('%s: MODE expression %r' % (n, mexpr))
                    slots.setdefault(d.get('name'), (d.get('type'), MODE_EXPRS[mexpr],
                                     e.text if e is not None else d.get('expr')))
        out = []
        when = SLOT_WHEN.get(n, {})
        for sname in when:
            if sname not in slots or when[sname] not in fields:
                raise SystemExit('%s: SLOT_WHEN %s' % (n, sname))
        for sname, (kind, (mode, sel), expr) in sorted(slots.items()):
            if sel is not None:
                if sel not in fields:
                    raise SystemExit('%s: no format select field %s' % (n, sel))
                mode = (mode, fields[sel][0])
            else:
                mode = (mode, 0xff)
            p = slot_pieces(expr, fields, n)
            lo, w, _ = p[0]
            if mask & width_mask(lo, lo + w - 1) == width_mask(lo, lo + w - 1):
                continue	# the number is a pattern: nothing to check
            cond = fields[when[sname]][0] if sname in when else 0xff
            out.append((sname, kind, mode, cond, p))
        yield n, mask, match, fields, out


def entries(n, mask, match, fields):
    allow = {}
    for name, vals in ALLOW.get(n, {}).items():
        f = name if name in fields else OLD_NAMES.get(name)
        if f not in fields:
            raise SystemExit('%s: ALLOW names no field %s' % (n, name))
        allow[f] = vals
    names = sorted(allow)
    for vals in itertools.product(*(allow[f] for f in names)):
        m, v = mask, match
        for f, val in zip(names, vals):
            lo, hi = fields[f]
            if m & width_mask(lo, hi):
                raise SystemExit('%s: ALLOW field %s is a pattern' % (n, f))
            m |= width_mask(lo, hi)
            v |= val << lo
        yield m, v


def main():
    as_json = sys.argv[1] == '--json'
    root = ET.parse(sys.argv[-1]).getroot()
    leaves = list(leaf_forms(root))
    names = [l[0] for l in leaves]
    for name in list(POLICY) + list(ALLOW):
        if name not in names:
            raise SystemExit('no leaf %s' % name)

    # Leaves must not overlap: a word matching two would be ambiguous.
    for i, (n1, m1, v1, _, _) in enumerate(leaves):
        for n2, m2, v2, _, _ in leaves[i + 1:]:
            common = m1 & m2
            if (v1 & common) == (v2 & common):
                raise SystemExit('%s and %s overlap' % (n1, n2))

    leaves.sort(key=lambda l: (l[2] >> 59, l[0]))
    slotsets = []
    rows = []
    for n, mask, match, fields, slots in leaves:
        key = tuple((SLOT_KINDS[k], mode, cond, tuple(p)) for _, k, mode, cond, p in slots)
        if key not in slotsets:
            slotsets.append(key)
        for m, v in entries(n, mask, match, fields):
            rows.append((n, m, v, POLICY.get(n, 'USSE_OK'), slotsets.index(key), slots))

    for key in slotsets:
        if len(key) > 4 or any(len(p) > 3 for _, _, _, p in key):
            raise SystemExit('a form has more slots or pieces than the kernel holds')
    if as_json:
        json.dump([{'name': n, 'mask': m, 'match': v, 'kind': k,
                    'slots': [{'name': s, 'type': t, 'mode': m[0], 'sel': m[1], 'when': c,
                               'pieces': p}
                              for s, t, m, c, p in sl]}
                   for n, m, v, k, _, sl in rows], sys.stdout, indent=1)
        return

    out = sys.stdout
    out.write('/* SPDX-License-Identifier: MIT */\n')
    out.write('/* Copyright © 2026 Joseph Telaak */\n\n')
    out.write('/*\n * The USSE forms, their operand slots and what the validator does with\n')
    out.write(' * each, from the isaspec description of the instruction set Mesa\'s\n')
    out.write(' * compiler uses (src/sgx/isa/sgx.xml). Each form is the bits its\n')
    out.write(' * patterns fix; each slot the fields its operand is built from.\n */\n\n')
    out.write('static const struct pvrsgx_usse_slot pvrsgx_usse_slots[][USSE_MAX_SLOTS] = {\n')
    for i, key in enumerate(slotsets):
        if not key:
            out.write('\t[%d] = { },\n' % i)
            continue
        out.write('\t[%d] = {\n' % i)
        for kind, mode, cond, pieces in key:
            fmt = mode[0] if mode[1] == 0xff else 'USSE_SEL(%d)' % mode[1]
            head = '\t\t{ %s, %s, %s,' % (kind, fmt, 'USSE_ALWAYS' if cond == 0xff else str(cond))
            tail = '{%s } },\n' % ','.join(' { %d, %d, %d }' % p for p in pieces)
            # One line if it fits in 100 columns, tabs as 8.
            if len(head.expandtabs(8)) + 1 + len(tail) - 1 <= 100:
                out.write(head + ' ' + tail)
            else:
                out.write(head + '\n\t\t  ' + tail)
        out.write('\t},\n')
    out.write('};\n\n')
    out.write('/* %d forms, %d entries */\n' % (len(leaves), len(rows)))
    out.write('static const struct pvrsgx_usse_form pvrsgx_usse_forms[] = {\n')
    for n, m, v, kind, si, _ in rows:
        out.write('\t{ 0x%016xull, 0x%016xull, %s, %d },\t/* %s */\n' % (m, v, kind, si, n))
    out.write('};\n')


if __name__ == '__main__':
    main()
